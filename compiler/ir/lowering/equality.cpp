// Equality through `Equatable` (decision 0031, spec 20.13): `x == nil` as a tag test, `==` and
// `!=` on a non-builtin conformer (derived, array or hand-written) as a call of its witness,
// `t.equals(other:)` on a builtin, derived or array conformer in a generic instance, and the
// generated derived witness of each concrete type the program needs, which compares stored
// fields or enum payloads in declaration order.

#include "internal.h"

namespace joyeer::lowering::detail {

// One generated function per concrete type with a derived `Equatable` conformance (decision 0031, spec
// 20.13.5), named `Type.equals` and taking both values borrowed. It is not a member: only `==` reaches it.
void Builder::collectDerivedEqualities() {
    for (const auto type : model->derivedEqualities()) {
        const auto* record = model->types().type(type);
        if (record == nullptr) continue;
        ir::Function function;
        function.id = static_cast<ir::FunctionId>(module->functions.size());
        function.name = model->types().displayName(type, true) + ".equals";
        const auto* owner = model->semanticModel()->symbol(record->symbol);
        function.isRootModule = owner == nullptr || owner->isRootModule;
        const auto declaration = owner != nullptr && owner->declaration.has_value()
                ? model->semanticModel()->node(*owner->declaration) : nullptr;
        if (declaration == nullptr) {
            report(DiagnosticId::missingType, SourceSpan {},
                   "a derived 'Equatable' conformance has no type declaration");
            continue;
        }
        // Every instruction of the body is located at the type's declaration, in the function's own root scope
        // (a type introduces no debug scope), so each call carries a location (decision 0031).
        const auto span = declaration->span;
        if (module->sourceFile(0) != nullptr) {
            function.debugScope = std::nullopt;
            function.debugLocation = ir::DebugLocation { span, false, std::nullopt };
        }
        for (const auto* name : { "self", "other" }) {
            ir::Parameter parameter;
            parameter.value = ir::Value {
                static_cast<ir::ValueId>(function.parameters.size()), type, ir::ValueCategory::value };
            parameter.name = name;
            parameter.span = span;
            if (std::string(name) == "other") parameter.label = "other";
            function.parameters.push_back(std::move(parameter));
        }
        function.resultType = model->types().boolType();
        function.returnsValue = true;
        function.entry = 0;
        derivedEqualityFunctions.emplace(type, function.id);
        module->functions.push_back(std::move(function));
    }
}

// The body of a derived witness (spec 20.13.5): a struct compares its stored fields in declaration order; an
// enum dispatches on both tags and compares the payloads of equal tags. Each stops at the first pair that
// differs. The operands are borrowed, so nothing is copied or destroyed.
void Builder::lowerDerivedEquality(typing::TypeId type, ir::FunctionId functionId) {
    currentFunctionId = functionId;
    currentBlockId = 0;
    slots.clear();
    ownership.clear();
    liveOwnedTemporaries.clear();
    scopes.clear();
    boundParameters.clear();
    pushScope();
    auto& function = currentFunction();
    currentDebugScope = function.debugScope;
    function.blocks.push_back(ir::BasicBlock { 0, "entry", {} });
    nextValue = static_cast<ir::ValueId>(function.parameters.size());
    const auto self = function.parameters[0].value;
    const auto other = function.parameters[1].value;
    const auto span = function.debugLocation.has_value() ? function.debugLocation->span : SourceSpan {};
    recordValue(self, ValueOwnership::borrowed);
    recordValue(other, ValueOwnership::borrowed);
    std::optional<ir::BlockId> falseBlock;
    const auto differs = [&]() {
        if (!falseBlock.has_value()) falseBlock = createBlock("equals.differ");
        return *falseBlock;
    };
    // Compares the members `extract(index)` yields, in order, and returns their verdict.
    const auto compareMembers = [&](const std::vector<typing::TypeId>& types,
                                    const std::function<std::pair<ir::Value, ir::Value>(size_t)>& extract) {
        // `Never` has no value and every `Void` value is equal, so neither is compared.
        std::vector<size_t> compared;
        for (size_t index = 0; index < types.size(); ++index) {
            if (types[index] != model->types().neverType() && types[index] != model->types().voidType()) {
                compared.push_back(index);
            }
        }
        if (compared.empty()) {
            emitDerivedReturn(emitBoolLiteral(true, span), span);
            return;
        }
        for (size_t position = 0; position < compared.size(); ++position) {
            const auto index = compared[position];
            const auto [left, right] = extract(index);
            const auto same = emitMemberEquality(left, right, types[index], span);
            if (!same.has_value()) {
                emit(makeInstruction(ir::Opcode::unreachable, span));
                return;
            }
            if (position + 1 == compared.size()) {
                emitDerivedReturn(*same, span);
                return;
            }
            const auto next = createBlock("equals.next");
            emitConditionalBranch(*same, next, differs(), span);
            switchToBlock(next);
        }
    };

    const auto structure = std::find_if(module->structures.begin(), module->structures.end(),
            [type](const auto& candidate) { return candidate.type == type; });
    const auto enumeration = std::find_if(module->enumerations.begin(), module->enumerations.end(),
            [type](const auto& candidate) { return candidate.type == type; });
    if (structure != module->structures.end()) {
        // The hidden live flag of a struct with `deinit` has no source symbol and is not a member.
        std::vector<ir::FieldDefinition> fields;
        for (const auto& field : structure->fields) {
            if (field.symbol != semantic::invalidSymbolId) fields.push_back(field);
        }
        std::vector<typing::TypeId> types;
        for (const auto& field : fields) types.push_back(field.type);
        compareMembers(types, [&](size_t index) {
            const auto& field = fields[index];
            return std::pair {
                emitValue(ir::Opcode::extractField, field.type, ir::ValueCategory::value, { self.id }, span,
                          field.symbol),
                emitValue(ir::Opcode::extractField, field.type, ir::ValueCategory::value, { other.id }, span,
                          field.symbol) };
        });
    } else if (enumeration != module->enumerations.end()) {
        const auto cases = enumeration->cases;
        const auto casePattern = [type](const ir::EnumCaseDefinition& enumCase) {
            ir::Pattern pattern;
            pattern.kind = ir::PatternKind::enumCase;
            pattern.type = type;
            pattern.symbol = enumCase.symbol;
            for (const auto payloadType : enumCase.payloadTypes) {
                ir::Pattern wildcard;
                wildcard.kind = ir::PatternKind::wildcard;
                wildcard.type = payloadType;
                pattern.payloads.push_back(std::move(wildcard));
            }
            return pattern;
        };
        std::vector<ir::BlockId> caseBlocks;
        auto dispatch = makeInstruction(ir::Opcode::switchPattern, span);
        dispatch.operands = { self.id };
        for (const auto& enumCase : cases) {
            caseBlocks.push_back(createBlock("equals.case"));
            dispatch.switchCases.push_back(ir::SwitchCase { casePattern(enumCase), caseBlocks.back() });
        }
        emit(std::move(dispatch));
        for (size_t index = 0; index < cases.size(); ++index) {
            switchToBlock(caseBlocks[index]);
            const auto matched = createBlock("equals.same");
            auto second = makeInstruction(ir::Opcode::switchPattern, span);
            second.operands = { other.id };
            second.switchCases.push_back(ir::SwitchCase { casePattern(cases[index]), matched });
            ir::Pattern anything;
            anything.kind = ir::PatternKind::wildcard;
            anything.type = type;
            second.switchCases.push_back(ir::SwitchCase { anything, differs() });
            emit(std::move(second));
            switchToBlock(matched);
            const auto& enumCase = cases[index];
            compareMembers(enumCase.payloadTypes, [&](size_t payload) {
                const auto left = emitExtractPayload(
                        enumCase.payloadTypes[payload], self.id, enumCase.symbol, static_cast<int64_t>(payload), span);
                const auto right = emitExtractPayload(
                        enumCase.payloadTypes[payload], other.id, enumCase.symbol, static_cast<int64_t>(payload), span);
                recordValue(left, ValueOwnership::borrowed);
                recordValue(right, ValueOwnership::borrowed);
                return std::pair { left, right };
            });
        }
    } else {
        report(DiagnosticId::missingType, span, "a derived 'Equatable' conformance has no concrete definition");
    }
    if (falseBlock.has_value()) {
        switchToBlock(*falseBlock);
        emitDerivedReturn(emitBoolLiteral(false, span), span);
    }
    // Every member value is borrowed and every result trivial, so the one scope holds nothing to clean up.
    assert(scopes.size() == 1 && scopes.back().cleanups.empty());
    scopes.clear();
}

void Builder::emitDerivedReturn(ir::Value value, SourceSpan span) {
    auto instruction = makeInstruction(ir::Opcode::ret, span);
    instruction.operands = { value.id };
    emit(std::move(instruction));
}

// Equality of two borrowed member values in a derived witness: the builtin operation, or a call of the member
// type's own witness.
std::optional<ir::Value> Builder::emitMemberEquality(ir::Value left, ir::Value right, typing::TypeId type, SourceSpan span) {
    const auto boolean = model->types().boolType();
    if (type == model->types().voidType()) return emitBoolLiteral(true, span);
    if (model->isBuiltinEquatable(type)) {
        if (isString(type)) return emitStringOperator(equalEqual, left, right, boolean, span);
        return emitValue(ir::ComparePredicate::equal, boolean, ir::ValueCategory::value, { left.id, right.id },
                         span);
    }
    const auto callee = equalityCallee(type);
    if (!callee.has_value()) {
        report(DiagnosticId::missingSymbol, span, "a member of a derived 'Equatable' conformance has no witness");
        return std::nullopt;
    }
    // Not implicit: a call inside a function with debug information needs a location.
    auto call = makeInstruction(ir::Opcode::call, span);
    call.callee = *callee;
    call.operands = { left.id, right.id };
    call.result = makeValue(boolean, ir::ValueCategory::value);
    const auto result = *call.result;
    emit(std::move(call));
    recordValue(result, ValueOwnership::trivial);
    return result;
}

// The function `==` calls for a conformer other than a builtin: its generated derived witness, or its
// hand-written witness (an instance for a generic type).
std::optional<ir::FunctionId> Builder::equalityCallee(typing::TypeId type) {
    if (const auto found = derivedEqualityFunctions.find(type); found != derivedEqualityFunctions.end()) {
        return found->second;
    }
    // `[X]`: the `array_equal` instance for `X` (spec 20.13.3, item 4).
    if (model->isArrayRoleType(type)) {
        const auto function = model->arrayEqualFunction();
        const auto* record = model->types().type(type);
        if (!function.has_value() || record == nullptr || record->arguments.empty()) return std::nullopt;
        const auto instance = model->instanceIndex(*function, { record->arguments[0] });
        if (!instance.has_value() || *instance >= instanceFunctions.size() ||
            instanceFunctions[*instance] == ir::invalidFunctionId) {
            return std::nullopt;
        }
        return instanceFunctions[*instance];
    }
    const auto requirement = model->equatableRequirement();
    return requirement.has_value() ? witnessFunction(type, *requirement) : std::nullopt;
}

// `x == nil` (spec 20.13.4): a tag test of the borrowed optional, true for `==` when it is absent. It calls no
// witness, so it works whatever the payload is.
std::optional<ir::Value> Builder::lowerPresenceTest(const syntax::ExprPtr& operand, bool equal, SourceSpan span) {
    const auto value = lowerExpression(operand);
    if (!value.has_value() || currentBlockTerminated()) return std::nullopt;
    const auto presentCase = findRoleCase(value->type, ir::EnumerationRole::optional, false);
    const auto* definition = presentCase.has_value() ? enumCaseDefinition(value->type, *presentCase) : nullptr;
    if (definition == nullptr) {
        report(DiagnosticId::missingSymbol, span, "a presence test needs the optional's present case");
        return std::nullopt;
    }
    const auto boolean = model->types().boolType();
    const auto result = emitValue(ir::Opcode::stackAllocate, boolean, ir::ValueCategory::address, {}, span);
    const auto presentBlock = createBlock("presence.some");
    const auto absentBlock = createBlock("presence.none");
    const auto mergeBlock = createBlock("presence.merge");
    ir::Pattern present;
    present.kind = ir::PatternKind::enumCase;
    present.type = value->type;
    present.symbol = *presentCase;
    for (const auto payloadType : definition->payloadTypes) {
        ir::Pattern wildcard;
        wildcard.kind = ir::PatternKind::wildcard;
        wildcard.type = payloadType;
        present.payloads.push_back(std::move(wildcard));
    }
    ir::Pattern anything;
    anything.kind = ir::PatternKind::wildcard;
    anything.type = value->type;
    auto dispatch = makeInstruction(ir::Opcode::switchPattern, span);
    dispatch.operands = { value->id };
    dispatch.switchCases.push_back(ir::SwitchCase { present, presentBlock });
    dispatch.switchCases.push_back(ir::SwitchCase { anything, absentBlock });
    emit(std::move(dispatch));
    switchToBlock(presentBlock);
    emitRawStore(emitBoolLiteral(!equal, span), result, span);
    emitBranch(mergeBlock, span);
    switchToBlock(absentBlock);
    emitRawStore(emitBoolLiteral(equal, span), result, span);
    emitBranch(mergeBlock, span);
    switchToBlock(mergeBlock);
    return emitValue(ir::Opcode::load, boolean, ir::ValueCategory::value, { result.id }, span);
}

// `left == right` on a hand-written conformance (decision 0031, spec 20.13.2): a call of the witness with both
// operands borrowed; `!=` compares the result with `false`.
std::optional<ir::Value> Builder::lowerWitnessEquality(
        const syntax::ExprPtr& leftOperand, const syntax::ExprPtr& rightOperand, typing::TypeId operandType,
        bool equal, typing::TypeId type, SourceSpan span) {
    const auto callee = equalityCallee(operandType);
    if (!callee.has_value()) {
        report(DiagnosticId::missingSymbol, span, "the 'Equatable' conformance has no witness to call");
        return std::nullopt;
    }
    const auto left = lowerExpression(leftOperand);
    if (!left.has_value() || currentBlockTerminated()) return std::nullopt;
    const auto right = lowerExpression(rightOperand);
    if (!right.has_value() || currentBlockTerminated()) return std::nullopt;
    auto call = makeInstruction(ir::Opcode::call, span);
    call.callee = *callee;
    call.operands = { left->id, right->id };
    call.result = makeValue(type, ir::ValueCategory::value);
    const auto result = *call.result;
    emit(std::move(call));
    recordValue(result, ValueOwnership::trivial);
    if (equal) return result;
    const auto no = emitBoolLiteral(false, span);
    return emitValue(ir::ComparePredicate::equal, type, ir::ValueCategory::value, { result.id, no.id }, span);
}

// `t.equals(other: u)` in an instance whose `T` is a builtin or derived conformer (decision 0031): the builtin
// `==` or the derived witness, since neither is a method. Returns nothing for any other call.
std::optional<std::optional<ir::Value>> Builder::lowerBuiltinEqualsRequirement(
        const syntax::CallExprSyntax::Ptr& expression, const semantic::Symbol* targetSymbol) {
    const auto requirement = model->equatableRequirement();
    if (targetSymbol == nullptr || !requirement.has_value() || targetSymbol->id != *requirement ||
        expression->callee->kind != syntax::Kind::memberExpr || expression->arguments.size() != 1) {
        return std::nullopt;
    }
    const auto base = std::static_pointer_cast<syntax::MemberExprSyntax>(expression->callee)->base;
    const auto baseType = typeOf(base);
    const auto type = typeOf(expression);
    if (!baseType.has_value() || !type.has_value()) return std::nullopt;
    // A derived conformer and an array have no `equals` method either; the generated witness or the
    // `arrayEqual` instance stands in (decision 0031).
    if (model->isDerivedEquatable(*baseType) || model->isArrayRoleType(*baseType)) {
        return lowerWitnessEquality(base, expression->arguments[0]->value, *baseType, true, *type,
                                    expression->span);
    }
    if (!model->isBuiltinEquatable(*baseType)) return std::nullopt;
    return lowerBuiltinBinary(equalEqual, base, expression->arguments[0]->value, *type, expression->span);
}

} // namespace joyeer::lowering::detail
