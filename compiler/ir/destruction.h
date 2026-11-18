#ifndef __joyeer_ir_destruction_h__
#define __joyeer_ir_destruction_h__

// Which types need a destroy: Strings, inert-zero collections, deinit owners, and aggregates
// holding any of them. Used by the verifier's `destroy` check and by the backend.

#include "module.h"

#include <unordered_map>

namespace joyeer::ir {

// Snapshot of one module's type graph. Rebuild after changing type definitions.
class DestructionAnalysis {
public:
    explicit DestructionAnalysis(const Module& module);
    [[nodiscard]] bool requiresDestruction(TypeId type) const;

private:
    std::unordered_map<TypeId, bool> required;
};

[[nodiscard]] bool requiresDestruction(const Module& module, TypeId type);

} // namespace joyeer::ir

#endif
