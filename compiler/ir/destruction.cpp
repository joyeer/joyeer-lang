#include "destruction.h"

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace joyeer::ir {

DestructionAnalysis::DestructionAnalysis(const Module& module) {
    std::unordered_map<TypeId, const TypeName*> types;
    std::unordered_map<TypeId, const StructureDefinition*> structures;
    std::unordered_map<TypeId, const EnumerationDefinition*> enumerations;
    for (const auto& type : module.types) types.emplace(type.id, &type);
    for (const auto& structure : module.structures) {
        structures.emplace(structure.type, &structure);
    }
    for (const auto& enumeration : module.enumerations) {
        enumerations.emplace(enumeration.type, &enumeration);
    }

    std::unordered_set<TypeId> visiting;
    std::function<bool(TypeId)> visit = [&](TypeId id) {
        const auto cached = required.find(id);
        if (cached != required.end()) return cached->second;
        const auto found = types.find(id);
        if (found == types.end()) return false;
        if (!visiting.insert(id).second) return true;

        bool result = false;
        switch (found->second->kind) {
            case typing::TypeKind::string:
                result = true;
                break;
            case typing::TypeKind::structure: {
                const auto structure = structures.find(id);
                if (structure != structures.end()) {
                    result = isInertZero(structure->second->role) ||
                            structure->second->deinitializer.has_value() ||
                            std::any_of(
                            structure->second->fields.begin(),
                            structure->second->fields.end(),
                            [&visit](const auto& field) { return visit(field.type); });
                }
                break;
            }
            case typing::TypeKind::enumeration: {
                const auto enumeration = enumerations.find(id);
                if (enumeration != enumerations.end()) {
                    result = std::any_of(
                            enumeration->second->cases.begin(),
                            enumeration->second->cases.end(),
                            [&visit](const auto& enumCase) {
                                return std::any_of(
                                        enumCase.payloadTypes.begin(),
                                        enumCase.payloadTypes.end(),
                                        [&visit](TypeId payload) { return visit(payload); });
                            });
                }
                break;
            }
            default:
                break;
        }
        visiting.erase(id);
        required.emplace(id, result);
        return result;
    };
    for (const auto& type : module.types) visit(type.id);
}

bool DestructionAnalysis::requiresDestruction(TypeId type) const {
    const auto found = required.find(type);
    return found != required.end() && found->second;
}

bool requiresDestruction(const Module& module, TypeId type) {
    return DestructionAnalysis(module).requiresDestruction(type);
}

} // namespace joyeer::ir
