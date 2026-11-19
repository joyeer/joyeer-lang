#include "opcode.h"

namespace joyeer::ir {

bool producesValue(Opcode opcode) {
    switch (opcode) {
        case Opcode::integerConstant:
        case Opcode::floatingConstant:
        case Opcode::booleanConstant:
        case Opcode::stringConstant:
        case Opcode::byteConstant:
        case Opcode::unitConstant:
        case Opcode::stackAllocate:
        case Opcode::load:
        case Opcode::copyValue:
        case Opcode::take:
        case Opcode::add:
        case Opcode::subtract:
        case Opcode::multiply:
        case Opcode::divide:
        case Opcode::remainder:
        case Opcode::less:
        case Opcode::lessEqual:
        case Opcode::greater:
        case Opcode::greaterEqual:
        case Opcode::equal:
        case Opcode::notEqual:
        case Opcode::bitAnd:
        case Opcode::bitOr:
        case Opcode::bitXor:
        case Opcode::shiftLeft:
        case Opcode::shiftRight:
        case Opcode::convertChecked:
        case Opcode::convertWrapping:
        case Opcode::constantValue:
        case Opcode::integerFits:
        case Opcode::constructStruct:
        case Opcode::fieldAddress:
        case Opcode::extractField:
        case Opcode::constructEnum:
        case Opcode::extractPayload:
        case Opcode::payloadAddress:
        case Opcode::stringUtf8:
        case Opcode::count:
        case Opcode::subscript:
        case Opcode::functionAddress:
        case Opcode::typeSize:
        case Opcode::typeAlignment:
        case Opcode::integerAddress:
        case Opcode::pointerToInteger:
        case Opcode::cloneCallback:
        case Opcode::destroyCallback:
        case Opcode::slotAddress:
        case Opcode::stackBlock:
            return true;
        default:
            return false;
    }
}

bool isTerminator(Opcode opcode) {
    switch (opcode) {
        case Opcode::branch:
        case Opcode::conditionalBranch:
        case Opcode::switchPattern:
        case Opcode::returnValue:
        case Opcode::returnVoid:
        case Opcode::unreachable:
            return true;
        default:
            return false;
    }
}

const char* opcodeName(Opcode opcode) {
    switch (opcode) {
        case Opcode::integerConstant: return "integer";
        case Opcode::floatingConstant: return "floating";
        case Opcode::booleanConstant: return "boolean";
        case Opcode::stringConstant: return "string";
        case Opcode::byteConstant: return "byte";
        case Opcode::unitConstant: return "unit";
        case Opcode::stackAllocate: return "alloc_stack";
        case Opcode::zeroInitialize: return "zero_init";
        case Opcode::load: return "load";
        case Opcode::store: return "store";
        case Opcode::copyValue: return "copy";
        case Opcode::take: return "take";
        case Opcode::destroy: return "destroy";
        case Opcode::add: return "add";
        case Opcode::subtract: return "sub";
        case Opcode::multiply: return "mul";
        case Opcode::divide: return "div";
        case Opcode::remainder: return "rem";
        case Opcode::less: return "lt";
        case Opcode::lessEqual: return "le";
        case Opcode::greater: return "gt";
        case Opcode::greaterEqual: return "ge";
        case Opcode::equal: return "eq";
        case Opcode::notEqual: return "ne";
        case Opcode::bitAnd: return "bit_and";
        case Opcode::bitOr: return "bit_or";
        case Opcode::bitXor: return "bit_xor";
        case Opcode::shiftLeft: return "shl";
        case Opcode::shiftRight: return "shr";
        case Opcode::convertChecked: return "convert";
        case Opcode::convertWrapping: return "convert_wrapping";
        case Opcode::constantValue: return "constant";
        case Opcode::integerFits: return "fits";
        case Opcode::typeSize: return "type_size";
        case Opcode::typeAlignment: return "type_alignment";
        case Opcode::integerAddress: return "integer_address";
        case Opcode::pointerToInteger: return "pointer_to_integer";
        case Opcode::destroyRange: return "destroy_range";
        case Opcode::cloneAt: return "clone_at";
        case Opcode::cloneCallback: return "clone_callback";
        case Opcode::destroyCallback: return "destroy_callback";
        case Opcode::linearRangeGuard: return "linear_range_guard";
        case Opcode::slotAddress: return "slot_address";
        case Opcode::stackBlock: return "stack_block";
        case Opcode::call: return "call";
        case Opcode::functionAddress: return "function_address";
        case Opcode::callIndirect: return "call_indirect";
        case Opcode::constructStruct: return "construct_struct";
        case Opcode::fieldAddress: return "field_addr";
        case Opcode::extractField: return "extract_field";
        case Opcode::constructEnum: return "construct_enum";
        case Opcode::extractPayload: return "extract_payload";
        case Opcode::payloadAddress: return "payload_address";
        case Opcode::stringUtf8: return "string_utf8";
        case Opcode::count: return "count";
        case Opcode::subscript: return "subscript";
        case Opcode::branch: return "br";
        case Opcode::conditionalBranch: return "cond_br";
        case Opcode::switchPattern: return "switch_pattern";
        case Opcode::returnValue: return "ret";
        case Opcode::returnVoid: return "ret_void";
        case Opcode::unreachable: return "unreachable";
    }
    return "unknown";
}

} // namespace joyeer::ir
