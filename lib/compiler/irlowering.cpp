#include "joyeer/compiler/irlowering.h"

#include <cassert>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace joyeer::lowering {

namespace {

class Builder {
public:
    Result build(const typing::TypeCheckedModel::Ptr& checkedModel, std::string sourceName) {
        assert(checkedModel != nullptr);
        model = checkedModel;
        module = std::make_shared<ir::Module>();
        module->sourceName = std::move(sourceName);

        snapshotTypes();
        collectFunctions();
        lowerFunctions();

        if (diagnostics.empty()) {
            const auto verification = ir::Verifier().verify(*module);
            for (const auto& error : verification.errors) {
                report(
                        DiagnosticId::verificationFailed,
                        {},
                        std::string(ir::verificationErrorName(error.id)) +
                                ": " + error.message);
            }
        }
        return Result { module, std::move(diagnostics) };
    }

private:
    typing::TypeCheckedModel::Ptr model;
    std::shared_ptr<ir::Module> module;
    std::vector<Diagnostic> diagnostics;
    std::unordered_map<semantic::SymbolId, ir::FunctionId> functions;
    std::unordered_map<semantic::SymbolId, ir::Value> slots;
    ir::FunctionId currentFunctionId = ir::invalidFunctionId;
    ir::BlockId currentBlockId = ir::invalidBlockId;
    ir::ValueId nextValue = 0;

    void report(DiagnosticId id, SourceSpan span, std::string message) {
        diagnostics.push_back(Diagnostic { id, span, std::move(message) });
    }

    void snapshotTypes() {
        const auto& types = model->types();
        for (typing::TypeId id = 0; id < types.size(); ++id) {
            module->types.push_back(ir::TypeName { id, types.displayName(id) });
        }
    }

    void collectFunctions() {
        collectPrint();
        const auto& semanticModel = *model->semanticModel();
        const auto& root = semanticModel.root();
        if (root == nullptr) return;
        for (const auto& item : root->items) {
            if (item->kind == syntax::Kind::functionDecl) {
                collectFunction(std::static_pointer_cast<syntax::FunctionDeclSyntax>(item));
            } else if (item->kind == syntax::Kind::bindingDecl) {
                report(
                        DiagnosticId::unsupportedSyntax,
                        item->span,
                        "top-level bindings are not supported by primitive IR lowering");
            }
        }
    }

    void collectPrint() {
        const auto& semanticModel = *model->semanticModel();
        const auto* prelude = semanticModel.scope(semanticModel.preludeScope());
        if (prelude == nullptr) return;
        const auto found = prelude->values.find("print");
        if (found == prelude->values.end()) return;
        const auto* signature = model->callable(found->second);
        if (signature == nullptr || signature->parameters.size() != 1) return;

        ir::Function function;
        function.id = static_cast<ir::FunctionId>(module->functions.size());
        function.symbol = found->second;
        function.name = "print";
        function.parameters.push_back(ir::Parameter {
            ir::Value { 0, signature->parameters[0], ir::ValueCategory::value },
            std::nullopt,
            "value",
            false,
            {},
            true,
        });
        function.resultType = signature->result;
        function.isExternal = true;
        functions.emplace(found->second, function.id);
        module->functions.push_back(std::move(function));
    }

    void collectFunction(const syntax::FunctionDeclSyntax::Ptr& declaration) {
        const auto& semanticModel = *model->semanticModel();
        const auto symbol = semanticModel.declaredSymbol(declaration);
        if (!symbol.has_value()) {
            report(DiagnosticId::missingSymbol, declaration->span, "function has no symbol");
            return;
        }
        const auto* signature = model->callable(*symbol);
        if (signature == nullptr) {
            report(
                    DiagnosticId::missingType,
                    declaration->span,
                    "function has no typed callable signature");
            return;
        }

        ir::Function function;
        function.id = static_cast<ir::FunctionId>(module->functions.size());
        function.symbol = symbol;
        const auto* semanticSymbol = semanticModel.symbol(*symbol);
        function.name = semanticSymbol == nullptr ? std::string() : semanticSymbol->name;
        function.resultType = signature->result;
        function.returnsValue = signature->result != model->types().voidType() &&
                signature->result != model->types().neverType();
        function.entry = 0;

        for (size_t index = 0; index < declaration->parameters.size(); ++index) {
            const auto& parameter = declaration->parameters[index];
            const auto parameterSymbol = semanticModel.declaredSymbol(parameter);
            const auto parameterType = parameterSymbol.has_value()
                    ? model->typeOf(*parameterSymbol)
                    : std::optional<typing::TypeId>();
            if (!parameterSymbol.has_value() || !parameterType.has_value()) {
                report(
                        parameterSymbol.has_value()
                                ? DiagnosticId::missingType
                                : DiagnosticId::missingSymbol,
                        parameter->span,
                        "parameter is missing a typed symbol");
                continue;
            }
            function.parameters.push_back(ir::Parameter {
                ir::Value {
                    static_cast<ir::ValueId>(index),
                    *parameterType,
                    ir::ValueCategory::value,
                },
                parameterSymbol,
                parameter->name == nullptr ? std::string() : parameter->name->rawValue,
                parameter->inoutKeyword != nullptr,
                parameter->span,
            });
        }

        functions.emplace(*symbol, function.id);
        module->functions.push_back(std::move(function));
    }

    void lowerFunctions() {
        const auto& semanticModel = *model->semanticModel();
        const auto& root = semanticModel.root();
        if (root == nullptr) return;
        for (const auto& item : root->items) {
            if (item->kind != syntax::Kind::functionDecl) continue;
            const auto declaration = std::static_pointer_cast<syntax::FunctionDeclSyntax>(item);
            const auto symbol = semanticModel.declaredSymbol(declaration);
            if (!symbol.has_value()) continue;
            const auto found = functions.find(*symbol);
            if (found != functions.end()) lowerFunction(declaration, found->second);
        }
    }

    void lowerFunction(
            const syntax::FunctionDeclSyntax::Ptr& declaration,
            ir::FunctionId functionId) {
        currentFunctionId = functionId;
        currentBlockId = 0;
        slots.clear();
        auto& function = currentFunction();
        function.blocks.push_back(ir::BasicBlock { 0, "entry", {} });
        nextValue = static_cast<ir::ValueId>(function.parameters.size());
        const auto diagnosticStart = diagnostics.size();

        for (const auto& parameter : function.parameters) {
            const auto address = emitValue(
                    ir::Opcode::stackAllocate,
                    parameter.value.type,
                    ir::ValueCategory::address,
                    {},
                    parameter.span,
                    parameter.symbol);
            emitStore(parameter.value, address, parameter.span, parameter.symbol);
            if (parameter.symbol.has_value()) slots[*parameter.symbol] = address;
        }

        lowerBlock(declaration->body);
        if (currentBlockTerminated()) return;
        if (function.returnsValue) {
            if (diagnostics.size() == diagnosticStart) {
                report(
                        DiagnosticId::missingReturn,
                        declaration->span,
                        "value-returning function reaches the end without returning");
            }
            emit(makeInstruction(ir::Opcode::unreachable, declaration->span));
        } else if (function.resultType == model->types().neverType()) {
            emit(makeInstruction(ir::Opcode::unreachable, declaration->span));
        } else {
            emit(makeInstruction(
                    ir::Opcode::returnVoid,
                    declaration->body == nullptr ? declaration->span : declaration->body->span));
        }
    }

    std::optional<ir::Value> lowerBlock(const syntax::BlockExprSyntax::Ptr& block) {
        if (block == nullptr) return std::nullopt;
        std::optional<ir::Value> result;
        for (const auto& item : block->items) {
            if (currentBlockTerminated()) break;
            if (item->kind == syntax::Kind::bindingDecl) {
                lowerBinding(std::static_pointer_cast<syntax::BindingDeclSyntax>(item));
                result.reset();
            } else if (item->kind == syntax::Kind::whileStmt) {
                lowerWhile(std::static_pointer_cast<syntax::WhileStmtSyntax>(item));
                result.reset();
            } else if (isExpressionKind(item->kind)) {
                result = lowerExpression(std::static_pointer_cast<syntax::ExprSyntax>(item));
            }
        }
        return result;
    }

    void lowerBinding(const syntax::BindingDeclSyntax::Ptr& declaration) {
        const auto symbol = model->semanticModel()->declaredSymbol(declaration);
        const auto type = symbol.has_value()
                ? model->typeOf(*symbol)
                : std::optional<typing::TypeId>();
        if (!symbol.has_value() || !type.has_value()) {
            report(
                    symbol.has_value() ? DiagnosticId::missingType : DiagnosticId::missingSymbol,
                    declaration->span,
                    "binding is missing a typed symbol");
            return;
        }
        if (declaration->initializer == nullptr) {
            report(
                    DiagnosticId::unsupportedSyntax,
                    declaration->span,
                    "bindings without initializers are not supported by primitive IR lowering");
            return;
        }
        const auto initializer = lowerExpression(declaration->initializer);
        if (!initializer.has_value()) return;
        const auto address = emitValue(
                ir::Opcode::stackAllocate,
                *type,
                ir::ValueCategory::address,
                {},
                declaration->span,
                symbol);
        emitStore(*initializer, address, declaration->span, symbol);
        slots[*symbol] = address;
    }

    std::optional<ir::Value> lowerExpression(const syntax::ExprPtr& expression) {
        if (expression == nullptr) return std::nullopt;
        switch (expression->kind) {
            case syntax::Kind::literalExpr:
                return lowerLiteral(std::static_pointer_cast<syntax::LiteralExprSyntax>(expression));
            case syntax::Kind::nameExpr:
                return lowerName(expression);
            case syntax::Kind::parenthesizedExpr:
                return lowerExpression(
                        std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression);
            case syntax::Kind::prefixExpr:
                return lowerPrefix(std::static_pointer_cast<syntax::PrefixExprSyntax>(expression));
            case syntax::Kind::accessExpr:
                return lowerExpression(
                        std::static_pointer_cast<syntax::AccessExprSyntax>(expression)->operand);
            case syntax::Kind::binaryExpr:
                return lowerBinary(std::static_pointer_cast<syntax::BinaryExprSyntax>(expression));
            case syntax::Kind::assignmentExpr:
                lowerAssignment(std::static_pointer_cast<syntax::AssignmentExprSyntax>(expression));
                return std::nullopt;
            case syntax::Kind::callExpr:
                return lowerCall(std::static_pointer_cast<syntax::CallExprSyntax>(expression));
            case syntax::Kind::returnExpr:
                lowerReturn(std::static_pointer_cast<syntax::ReturnExprSyntax>(expression));
                return std::nullopt;
            case syntax::Kind::blockExpr:
                return lowerBlock(std::static_pointer_cast<syntax::BlockExprSyntax>(expression));
            case syntax::Kind::ifExpr:
                return lowerIf(std::static_pointer_cast<syntax::IfExprSyntax>(expression));
            default:
                report(
                        DiagnosticId::unsupportedSyntax,
                        expression->span,
                        std::string("IR lowering is not implemented for ") +
                                syntax::kindName(expression->kind));
                return std::nullopt;
        }
    }

    std::optional<ir::Value> lowerLiteral(
            const syntax::LiteralExprSyntax::Ptr& expression) {
        const auto type = model->typeOf(expression);
        if (!type.has_value() || expression->literal == nullptr) {
            report(DiagnosticId::missingType, expression->span, "literal has no type");
            return std::nullopt;
        }

        ir::Opcode opcode;
        switch (expression->literal->kind) {
            case decimalLiteral: opcode = ir::Opcode::integerConstant; break;
            case booleanLiteral: opcode = ir::Opcode::booleanConstant; break;
            case stringLiteral: opcode = ir::Opcode::stringConstant; break;
            case byteLiteral: opcode = ir::Opcode::byteConstant; break;
            default:
                report(
                        DiagnosticId::unsupportedSyntax,
                        expression->span,
                        "literal kind is not supported by primitive IR lowering");
                return std::nullopt;
        }

        auto instruction = makeInstruction(opcode, expression->span);
        instruction.result = makeValue(*type, ir::ValueCategory::value);
        if (expression->literal->kind == booleanLiteral) {
            instruction.integerValue = expression->literal->rawValue == Literals::TRUE ? 1 : 0;
        } else if (expression->literal->kind != stringLiteral) {
            instruction.integerValue = expression->literal->intValue;
        } else {
            instruction.text = expression->literal->rawValue;
        }
        const auto result = *instruction.result;
        emit(std::move(instruction));
        return result;
    }

    std::optional<ir::Value> lowerName(const syntax::NodePtr& expression) {
        const auto symbol = model->referencedSymbol(expression);
        if (!symbol.has_value() || !slots.contains(*symbol)) {
            report(
                    DiagnosticId::missingSymbol,
                    expression->span,
                    "name does not refer to a lowered local storage slot");
            return std::nullopt;
        }
        const auto address = slots.at(*symbol);
        return emitValue(
                ir::Opcode::load,
                address.type,
                ir::ValueCategory::value,
                { address.id },
                expression->span,
                symbol);
    }

    std::optional<ir::Value> lowerPrefix(const syntax::PrefixExprSyntax::Ptr& expression) {
        const auto operand = lowerExpression(expression->operand);
        const auto type = model->typeOf(expression);
        if (!operand.has_value() || !type.has_value()) return std::nullopt;
        if (expression->op == nullptr || expression->op->kind != minus) {
            report(
                    DiagnosticId::unsupportedSyntax,
                    expression->span,
                    "prefix operator is not supported by primitive IR lowering");
            return std::nullopt;
        }

        auto zero = makeInstruction(ir::Opcode::integerConstant, expression->op->span);
        zero.result = makeValue(*type, ir::ValueCategory::value);
        const auto zeroValue = *zero.result;
        emit(std::move(zero));
        return emitValue(
                ir::Opcode::subtract,
                *type,
                ir::ValueCategory::value,
                { zeroValue.id, operand->id },
                expression->span);
    }

    std::optional<ir::Value> lowerBinary(const syntax::BinaryExprSyntax::Ptr& expression) {
        const auto left = lowerExpression(expression->left);
        const auto right = lowerExpression(expression->right);
        const auto type = model->typeOf(expression);
        if (!left.has_value() || !right.has_value() || !type.has_value() ||
            expression->op == nullptr) {
            return std::nullopt;
        }
        const auto opcode = binaryOpcode(expression->op->kind);
        if (!opcode.has_value()) {
            report(
                    DiagnosticId::unsupportedSyntax,
                    expression->span,
                    "binary operator is not supported by primitive IR lowering");
            return std::nullopt;
        }
        return emitValue(
                *opcode,
                *type,
                ir::ValueCategory::value,
                { left->id, right->id },
                expression->span);
    }

    std::optional<ir::Opcode> binaryOpcode(TokenKind token) const {
        switch (token) {
            case plus: return ir::Opcode::add;
            case minus: return ir::Opcode::subtract;
            case multiply: return ir::Opcode::multiply;
            case less: return ir::Opcode::less;
            case lessEqual: return ir::Opcode::lessEqual;
            case greater: return ir::Opcode::greater;
            case greaterEqual: return ir::Opcode::greaterEqual;
            case equalEqual: return ir::Opcode::equal;
            case notEqual: return ir::Opcode::notEqual;
            case andAnd: return ir::Opcode::logicalAnd;
            default: return std::nullopt;
        }
    }

    void lowerAssignment(const syntax::AssignmentExprSyntax::Ptr& expression) {
        const auto address = lowerAddress(expression->target);
        const auto value = lowerExpression(expression->value);
        if (address.has_value() && value.has_value()) {
            emitStore(*value, *address, expression->span);
        }
    }

    std::optional<ir::Value> lowerAddress(const syntax::ExprPtr& expression) {
        if (expression == nullptr) return std::nullopt;
        if (expression->kind == syntax::Kind::accessExpr) {
            return lowerAddress(
                    std::static_pointer_cast<syntax::AccessExprSyntax>(expression)->operand);
        }
        if (expression->kind == syntax::Kind::parenthesizedExpr) {
            return lowerAddress(
                    std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression);
        }
        if (expression->kind == syntax::Kind::nameExpr) {
            const auto symbol = model->referencedSymbol(expression);
            if (symbol.has_value() && slots.contains(*symbol)) return slots.at(*symbol);
        }
        report(
                DiagnosticId::unsupportedSyntax,
                expression->span,
                "assignment target is not supported by primitive IR lowering");
        return std::nullopt;
    }

    std::optional<ir::Value> lowerIf(const syntax::IfExprSyntax::Ptr& expression) {
        const auto condition = lowerExpression(expression->condition);
        const auto resultType = model->typeOf(expression);
        if (!condition.has_value() || !resultType.has_value()) return std::nullopt;

        const auto producesResult = *resultType != model->types().voidType() &&
                *resultType != model->types().neverType();
        const auto resultAddress = producesResult
                ? std::optional<ir::Value>(emitValue(
                        ir::Opcode::stackAllocate,
                        *resultType,
                        ir::ValueCategory::address,
                        {},
                        expression->span))
                : std::optional<ir::Value>();

        const auto thenBlock = createBlock("if.then");
        const auto elseBlock = createBlock("if.else");
        const auto mergeBlock = createBlock("if.merge");
        emitConditionalBranch(*condition, thenBlock, elseBlock, expression->condition->span);

        bool hasMergePredecessor = false;
        switchToBlock(thenBlock);
        const auto thenValue = lowerBlock(expression->thenBranch);
        if (!currentBlockTerminated()) {
            if (resultAddress.has_value()) {
                if (thenValue.has_value()) {
                    emitStore(*thenValue, *resultAddress, expression->thenBranch->span);
                } else {
                    report(
                            DiagnosticId::missingType,
                            expression->thenBranch->span,
                            "value-producing if branch did not lower a value");
                }
            }
            emitBranch(mergeBlock, expression->thenBranch->span);
            hasMergePredecessor = true;
        }

        switchToBlock(elseBlock);
        const auto elseValue = lowerExpression(expression->elseBranch);
        if (!currentBlockTerminated()) {
            if (resultAddress.has_value()) {
                if (elseValue.has_value()) {
                    emitStore(*elseValue, *resultAddress, expression->elseBranch->span);
                } else {
                    report(
                            DiagnosticId::missingType,
                            expression->span,
                            "value-producing else branch did not lower a value");
                }
            }
            emitBranch(mergeBlock, expression->span);
            hasMergePredecessor = true;
        }

        switchToBlock(mergeBlock);
        if (!hasMergePredecessor) {
            emit(makeInstruction(ir::Opcode::unreachable, expression->span));
            return std::nullopt;
        }
        if (!resultAddress.has_value()) return std::nullopt;
        return emitValue(
                ir::Opcode::load,
                resultAddress->type,
                ir::ValueCategory::value,
                { resultAddress->id },
                expression->span);
    }

    void lowerWhile(const syntax::WhileStmtSyntax::Ptr& statement) {
        const auto headerBlock = createBlock("while.header");
        const auto bodyBlock = createBlock("while.body");
        const auto exitBlock = createBlock("while.exit");
        emitBranch(headerBlock, statement->span);

        switchToBlock(headerBlock);
        const auto condition = lowerExpression(statement->condition);
        if (condition.has_value()) {
            emitConditionalBranch(
                    *condition,
                    bodyBlock,
                    exitBlock,
                    statement->condition->span);
        } else {
            emit(makeInstruction(ir::Opcode::unreachable, statement->condition->span));
        }

        switchToBlock(bodyBlock);
        lowerBlock(statement->body);
        if (!currentBlockTerminated()) emitBranch(headerBlock, statement->body->span);

        switchToBlock(exitBlock);
    }

    std::optional<ir::Value> lowerCall(const syntax::CallExprSyntax::Ptr& expression) {
        const auto target = model->callTarget(expression);
        if (!target.has_value() || !functions.contains(*target)) {
            report(
                    DiagnosticId::missingSymbol,
                    expression->span,
                    "call target has no IR function declaration");
            return std::nullopt;
        }

        std::vector<ir::ValueId> arguments;
        for (const auto& argument : expression->arguments) {
            const auto value = lowerExpression(argument->value);
            if (!value.has_value()) return std::nullopt;
            arguments.push_back(value->id);
        }

        auto instruction = makeInstruction(ir::Opcode::call, expression->span);
        instruction.operands = std::move(arguments);
        instruction.callee = functions.at(*target);
        const auto type = model->typeOf(expression);
        if (type.has_value() && *type != model->types().voidType() &&
            *type != model->types().neverType()) {
            instruction.result = makeValue(*type, ir::ValueCategory::value);
        }
        const auto result = instruction.result;
        emit(std::move(instruction));
        return result;
    }

    void lowerReturn(const syntax::ReturnExprSyntax::Ptr& expression) {
        if (expression->value == nullptr) {
            emit(makeInstruction(ir::Opcode::returnVoid, expression->span));
            return;
        }
        const auto value = lowerExpression(expression->value);
        if (!value.has_value()) return;
        auto instruction = makeInstruction(ir::Opcode::returnValue, expression->span);
        instruction.operands = { value->id };
        emit(std::move(instruction));
    }

    ir::Value emitValue(
            ir::Opcode opcode,
            typing::TypeId type,
            ir::ValueCategory category,
            std::vector<ir::ValueId> operands,
            SourceSpan span,
            std::optional<semantic::SymbolId> symbol = std::nullopt) {
        auto instruction = makeInstruction(opcode, span);
        instruction.result = makeValue(type, category);
        instruction.operands = std::move(operands);
        instruction.symbol = symbol;
        const auto result = *instruction.result;
        emit(std::move(instruction));
        return result;
    }

    void emitStore(
            ir::Value value,
            ir::Value address,
            SourceSpan span,
            std::optional<semantic::SymbolId> symbol = std::nullopt) {
        auto instruction = makeInstruction(ir::Opcode::store, span);
        instruction.operands = { value.id, address.id };
        instruction.symbol = symbol;
        emit(std::move(instruction));
    }

    ir::BlockId createBlock(std::string name) {
        auto& blocks = currentFunction().blocks;
        const auto id = static_cast<ir::BlockId>(blocks.size());
        blocks.push_back(ir::BasicBlock { id, std::move(name), {} });
        return id;
    }

    void switchToBlock(ir::BlockId block) {
        assert(block < currentFunction().blocks.size());
        currentBlockId = block;
    }

    void emitBranch(ir::BlockId target, SourceSpan span) {
        auto instruction = makeInstruction(ir::Opcode::branch, span);
        instruction.targets = { target };
        emit(std::move(instruction));
    }

    void emitConditionalBranch(
            ir::Value condition,
            ir::BlockId trueBlock,
            ir::BlockId falseBlock,
            SourceSpan span) {
        auto instruction = makeInstruction(ir::Opcode::conditionalBranch, span);
        instruction.operands = { condition.id };
        instruction.targets = { trueBlock, falseBlock };
        emit(std::move(instruction));
    }

    ir::Instruction makeInstruction(ir::Opcode opcode, SourceSpan span) const {
        auto instruction = ir::Instruction { opcode };
        instruction.span = span;
        return instruction;
    }

    ir::Value makeValue(typing::TypeId type, ir::ValueCategory category) {
        return ir::Value { nextValue++, type, category };
    }

    void emit(ir::Instruction instruction) {
        currentBlock().instructions.push_back(std::move(instruction));
    }

    bool currentBlockTerminated() const {
        const auto& instructions = currentBlock().instructions;
        return !instructions.empty() && ir::isTerminator(instructions.back().opcode);
    }

    ir::Function& currentFunction() {
        assert(currentFunctionId < module->functions.size());
        return module->functions[currentFunctionId];
    }

    const ir::Function& currentFunction() const {
        assert(currentFunctionId < module->functions.size());
        return module->functions[currentFunctionId];
    }

    ir::BasicBlock& currentBlock() {
        auto& blocks = currentFunction().blocks;
        assert(currentBlockId < blocks.size());
        return blocks[currentBlockId];
    }

    const ir::BasicBlock& currentBlock() const {
        const auto& blocks = currentFunction().blocks;
        assert(currentBlockId < blocks.size());
        return blocks[currentBlockId];
    }

    bool isExpressionKind(syntax::Kind kind) const {
        switch (kind) {
            case syntax::Kind::errorExpr:
            case syntax::Kind::nameExpr:
            case syntax::Kind::literalExpr:
            case syntax::Kind::parenthesizedExpr:
            case syntax::Kind::prefixExpr:
            case syntax::Kind::accessExpr:
            case syntax::Kind::binaryExpr:
            case syntax::Kind::assignmentExpr:
            case syntax::Kind::memberExpr:
            case syntax::Kind::callExpr:
            case syntax::Kind::subscriptExpr:
            case syntax::Kind::arrayExpr:
            case syntax::Kind::dictionaryExpr:
            case syntax::Kind::contextualCaseExpr:
            case syntax::Kind::blockExpr:
            case syntax::Kind::ifExpr:
            case syntax::Kind::returnExpr:
            case syntax::Kind::matchExpr:
                return true;
            default:
                return false;
        }
    }
};

} // namespace

Result Lowerer::lower(
        const typing::TypeCheckedModel::Ptr& model,
        std::string sourceName) const {
    return Builder().build(model, std::move(sourceName));
}

const char* diagnosticName(DiagnosticId id) {
    switch (id) {
        case DiagnosticId::unsupportedSyntax: return "ir-lowering.unsupported-syntax";
        case DiagnosticId::missingType: return "ir-lowering.missing-type";
        case DiagnosticId::missingSymbol: return "ir-lowering.missing-symbol";
        case DiagnosticId::missingReturn: return "ir-lowering.missing-return";
        case DiagnosticId::verificationFailed: return "ir-lowering.verification-failed";
    }
    return "ir-lowering.unknown";
}

std::string dump(const std::vector<Diagnostic>& diagnostics) {
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << diagnosticName(diagnostic.id) << '@'
            << diagnostic.span.offset << ':' << diagnostic.span.length
            << ": " << diagnostic.message << '\n';
    }
    return out.str();
}

} // namespace joyeer::lowering
