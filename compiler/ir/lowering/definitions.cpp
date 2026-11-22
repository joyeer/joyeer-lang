// Module-wide type data, collected before any function: the type table snapshot and the struct
// and enum definitions, including one per concrete instance of a generic type and the builtin
// enums.

#include "internal.h"

#include <algorithm>

namespace joyeer::lowering::detail {

void Builder::snapshotTypes() {
    const auto& types = model->types();
    for (typing::TypeId id = 0; id < types.size(); ++id) {
        const auto* type = types.type(id);
        assert(type != nullptr);
        // A type that mentions a type parameter exists only in a generic body; IR sees each
        // instance's concrete types, so it is an error type here and the verifier rejects any use.
        if (types.mentionsTypeParameter(id)) {
            module->types.push_back(ir::TypeName {
                id, types.displayName(id), typing::TypeKind::error, semantic::invalidSymbolId, {}, {} });
            continue;
        }
        module->types.push_back(ir::TypeName {
            id,
            types.displayName(id),
            type->kind,
            type->symbol,
            type->arguments,
            type->effects,
        });
        const auto* symbol = model->semanticModel()->symbol(type->symbol);
        if (symbol != nullptr && symbol->declaration.has_value()) {
            module->types.back().declarationSpan = symbol->span;
        }
    }
}

void Builder::collectAggregateDefinitions() {
    const auto& semanticModel = *model->semanticModel();
    const auto& root = semanticModel.compilationRoot();
    if (root != nullptr) {
        for (const auto& item : root->items) {
            if (item->kind == syntax::Kind::structDecl) {
                const auto declaration = std::static_pointer_cast<syntax::StructDeclSyntax>(item);
                if (declaration->genericParameters.empty()) collectStructure(declaration);
            } else if (item->kind == syntax::Kind::enumDecl) {
                const auto declaration = std::static_pointer_cast<syntax::EnumDeclSyntax>(item);
                if (declaration->genericParameters.empty()) collectEnumeration(declaration);
            }
        }
        collectGenericTypeInstances(*root);
    }
    for (typing::TypeId id = 0; id < model->types().size(); ++id) {
        const auto* type = model->types().type(id);
        const auto* symbol = type == nullptr
                ? nullptr
                : semanticModel.symbol(type->symbol);
        if (type != nullptr && !model->types().mentionsTypeParameter(id) &&
            type->kind == typing::TypeKind::enumeration &&
            symbol != nullptr &&
            symbol->kind == semantic::SymbolKind::builtinType) {
            collectBuiltinEnumeration(*type);
        }
    }
}

// One definition per concrete instance of a generic struct or enum (spec 20.12): the type
// checker interned each instance and its member types before lowering.
void Builder::collectGenericTypeInstances(const syntax::SourceFileSyntax& root) {
    const auto& semanticModel = *model->semanticModel();
    for (const auto& item : root.items) {
        const bool isStructure = item->kind == syntax::Kind::structDecl;
        if (!isStructure && item->kind != syntax::Kind::enumDecl) continue;
        const auto symbol = semanticModel.declaredSymbol(item);
        if (!symbol.has_value() || model->genericParameters(*symbol) == nullptr) continue;
        for (typing::TypeId id = 0; id < model->types().size(); ++id) {
            const auto* type = model->types().type(id);
            if (type == nullptr || type->symbol != *symbol || type->arguments.empty() ||
                model->types().mentionsTypeParameter(id)) {
                continue;
            }
            if (isStructure) {
                collectStructure(std::static_pointer_cast<syntax::StructDeclSyntax>(item), id);
            } else {
                collectEnumeration(std::static_pointer_cast<syntax::EnumDeclSyntax>(item), id);
            }
        }
    }
}

// `instance` is the concrete type of a generic declaration's instance; its field types are the
// declared ones with the instance's arguments substituted.
void Builder::collectStructure(
        const syntax::StructDeclSyntax::Ptr& declaration,
        std::optional<typing::TypeId> instance) {
    const auto& semanticModel = *model->semanticModel();
    const auto symbol = semanticModel.declaredSymbol(declaration);
    const auto type = instance.has_value()
            ? instance
            : symbol.has_value() ? typeOf(*symbol) : std::optional<typing::TypeId>();
    if (!symbol.has_value() || !type.has_value()) return;

    ir::StructureDefinition structure;
    structure.symbol = *symbol;
    structure.type = *type;
    structure.name = declaration->name == nullptr
            ? std::string()
            : declaration->name->rawValue;
    if (instance.has_value()) structure.name = model->types().displayName(*instance);
    for (const auto& field : declaration->fields) {
        const auto fieldSymbol = semanticModel.declaredSymbol(field);
        auto fieldType = fieldSymbol.has_value()
                ? typeOf(*fieldSymbol)
                : std::optional<typing::TypeId>();
        if (!fieldSymbol.has_value() || !fieldType.has_value()) continue;
        if (instance.has_value()) fieldType = model->memberTypeOf(*instance, *fieldType);
        const auto* semanticField = semanticModel.symbol(*fieldSymbol);
        structure.fields.push_back(ir::FieldDefinition {
            *fieldSymbol,
            field->name == nullptr ? std::string() : field->name->rawValue,
            *fieldType,
            semanticField != nullptr && semanticField->isMutable,
        });
    }
    // The library array and dictionary roles have no live flag: their zero value is destroyable (decisions
    // 0025, 0027). The dictionary entry is never a value and declares no `deinit`.
    const auto* structureSymbol = semanticModel.symbol(*symbol);
    const auto libraryRole = structureSymbol != nullptr ? structureSymbol->libraryRole : semantic::LibraryRole::none;
    structure.role = libraryRole == semantic::LibraryRole::arrayType ? ir::StructureRole::array
            : libraryRole == semantic::LibraryRole::dictType ? ir::StructureRole::dict
            : libraryRole == semantic::LibraryRole::dictEntry ? ir::StructureRole::dictEntry
                                                              : ir::StructureRole::none;
    if (declaration->deinitializer != nullptr && structure.role == ir::StructureRole::none) {
        // See ir::StructureDefinition::deinitializer: the live flag is always last.
        structure.fields.push_back(ir::FieldDefinition {
            semantic::invalidSymbolId,
            ir::liveFieldName,
            model->types().boolType(),
            false,
        });
    }
    module->structures.push_back(std::move(structure));
}

void Builder::collectEnumeration(
        const syntax::EnumDeclSyntax::Ptr& declaration,
        std::optional<typing::TypeId> instance) {
    const auto& semanticModel = *model->semanticModel();
    const auto symbol = semanticModel.declaredSymbol(declaration);
    const auto type = instance.has_value()
            ? instance
            : symbol.has_value() ? typeOf(*symbol) : std::optional<typing::TypeId>();
    if (!symbol.has_value() || !type.has_value()) return;

    ir::EnumerationDefinition enumeration;
    enumeration.symbol = *symbol;
    enumeration.type = *type;
    enumeration.name = declaration->name == nullptr
            ? std::string()
            : declaration->name->rawValue;
    if (instance.has_value()) enumeration.name = model->types().displayName(*instance);
    for (const auto& enumCase : declaration->cases) {
        const auto caseSymbol = semanticModel.declaredSymbol(enumCase);
        if (!caseSymbol.has_value()) continue;
        const auto* signature = model->callable(*caseSymbol);
        std::vector<typing::TypeId> payloads = signature == nullptr
                ? std::vector<typing::TypeId>() : signature->parameters;
        if (instance.has_value()) {
            for (auto& payload : payloads) payload = model->memberTypeOf(*instance, payload);
        }
        enumeration.cases.push_back(ir::EnumCaseDefinition {
            *caseSymbol,
            enumCase->name == nullptr ? std::string() : enumCase->name->rawValue,
            std::move(payloads),
        });
    }
    if (symbol.has_value()) {
        const auto* declared = semanticModel.symbol(*symbol);
        const auto role = declared == nullptr ? semantic::LibraryRole::none : declared->libraryRole;
        if (role == semantic::LibraryRole::optional || role == semantic::LibraryRole::result) {
            enumeration.role = role == semantic::LibraryRole::optional
                    ? ir::EnumerationRole::optional : ir::EnumerationRole::result;
            // Positional binding (decision 0024): optional's present case is the one with a payload;
            // result's success case is the first. The type checker validated the shape.
            for (uint32_t index = 0; index < enumeration.cases.size(); ++index) {
                const bool payload = !enumeration.cases[index].payloadTypes.empty();
                if (role == semantic::LibraryRole::optional) {
                    (payload ? enumeration.firstRoleCase : enumeration.secondRoleCase) = index;
                } else {
                    (index == 0 ? enumeration.firstRoleCase : enumeration.secondRoleCase) = index;
                }
            }
        }
    }
    module->enumerations.push_back(std::move(enumeration));
}

void Builder::collectBuiltinEnumeration(const typing::TypeRecord& type) {
    const auto& semanticModel = *model->semanticModel();
    const auto* symbol = semanticModel.symbol(type.symbol);
    if (symbol == nullptr || !symbol->memberScope.has_value()) return;
    const auto* members = semanticModel.scope(*symbol->memberScope);
    if (members == nullptr) return;

    std::vector<semantic::SymbolId> caseSymbols;
    for (const auto& [name, caseSymbol] : members->values) {
        static_cast<void>(name);
        caseSymbols.push_back(caseSymbol);
    }
    std::sort(caseSymbols.begin(), caseSymbols.end());

    ir::EnumerationDefinition enumeration;
    enumeration.symbol = type.symbol;
    enumeration.type = type.id;
    enumeration.name = model->types().displayName(type.id);
    for (const auto caseSymbol : caseSymbols) {
        const auto* enumCase = semanticModel.symbol(caseSymbol);
        if (enumCase == nullptr) continue;
        std::vector<typing::TypeId> payloadTypes;
        if (type.kind == typing::TypeKind::enumeration &&
            enumCase->callable.has_value()) {
            for (const auto& parameter : enumCase->callable->parameters) {
                if (!parameter.type.has_value()) continue;
                const auto payload = model->types().typeForSymbol(*parameter.type);
                if (payload.has_value()) payloadTypes.push_back(*payload);
            }
        }
        enumeration.cases.push_back(ir::EnumCaseDefinition {
            caseSymbol,
            enumCase->name,
            std::move(payloadTypes),
        });
    }
    module->enumerations.push_back(std::move(enumeration));
}

} // namespace joyeer::lowering::detail
