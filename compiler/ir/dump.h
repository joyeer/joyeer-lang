#ifndef __joyeer_ir_dump_h__
#define __joyeer_ir_dump_h__

// Deterministic text forms of a module and of verification errors, for tests and debugging.

#include "module.h"
#include "verifier.h"

#include <string>

namespace joyeer::ir {

[[nodiscard]] std::string dump(const Module& module);
[[nodiscard]] std::string dump(const VerificationResult& result);

} // namespace joyeer::ir

#endif
