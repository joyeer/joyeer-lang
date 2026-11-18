#ifndef __joyeer_ir_ir_h__
#define __joyeer_ir_ir_h__

// Umbrella header for the Joyeer IR. Prefer the focused headers in new code:
//
//   value.h        IDs and typed SSA values
//   opcode.h       the instruction set and opcode queries
//   debuginfo.h    source tables and debug scopes, variables and locations
//   definitions.h  types, structs, enums, their roles, and constants
//   module.h       instructions, blocks, functions and the module
//   destruction.h  which types need a destroy
//   dominance.h    block dominance
//   verifier.h     the structural verifier (implementation in verifier/)
//   dump.h         text dumps for tests and debugging

#include "debuginfo.h"
#include "definitions.h"
#include "destruction.h"
#include "dominance.h"
#include "dump.h"
#include "module.h"
#include "opcode.h"
#include "value.h"
#include "verifier.h"

#endif
