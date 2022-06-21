#ifndef __joyeer_runtime_bytecode_h__
#define __joyeer_runtime_bytecode_h__

#include "joyeer/runtime/memory.h"

enum Opcode : uint8_t {
    OP_NOP = 0x00,

    OP_OCONST_NIL,
    OP_ICONST,
    OP_SCONST,

    OP_ISTORE,

    OP_OLOAD,
    OP_ILOAD,

    OP_NEW,
    OP_PUTFIELD,
    OP_GETFIELD,
    OP_PUTSTATIC,
    OP_GETSTATIC,

    OP_IAND,
    OP_ICMP_G,
    OP_ICMP_GE,
    OP_ICMP_L,
    OP_ICMP_LE,
    OP_ICMP_NE,
    OP_ICMP_EQ,
    OP_IADD,
    OP_ISUB,
    OP_IMUL,
    OP_IDIV,
    OP_IREM,
    OP_INEG,

    OP_IFEQ,    // == 0
    OP_IFNE,    // != 0
    OP_IFLT,    // < 0
    OP_IFLE,    // <= 0
    OP_IFGT,    // > 0
    OP_IFGE,    // >= 0

    OP_RETURN,
    OP_IRETURN,
    OP_ORETURN,

    OP_ONEWARRAY,

    OP_INVOKE,
    OP_DUP,
    OP_POP,
    OP_GOTO,

    OP_DEBUG,
};


/**
 * format 1
 * | - 8 -  | - 56 - |
 * | opcode | value  |
 * | ------ | ------ |
 *
 * format 2
 * | - 8 -  | - 24 - | - 32 - |
 * | opcode | value1 | value2 |
 * | ------ | ------ | -------|
 */

struct Bytecode {
    Byte bytes[8];

    Bytecode(int8_t op, int64_t value);

    Bytecode(int8_t op, int32_t value1, int32_t value2) ;
};

#define OP_FROM_BYTECODE(bytecode) (Opcode)((bytecode).bytes[0])

#define VALUE_FROM_BYTECODE(bytecode) (int64_t)(*(int64_t*)(bytecode).bytes >> 8)

#define VAL1_FROM_BYTECODE(bytecode) (int32_t)((*(int64_t*)(bytecode).bytes & 0x00000000FFFFFFFF) >> 8)

#define VAL2_FROM_BYTECODE(bytecode) (int32_t)((*(int64_t*)(bytecode).bytes & 0xFFFFFFFF00000000) >> 32)

struct Bytecodes {
    Bytecodes(Byte* bytes, int length);
    ~Bytecodes();

    Byte *bytecodes;
    Int size = -1;
};

class BytecodeWriter {
public:
    void write(Bytecode bytecode);
    void write(Bytecodes* bytecodes);

    [[nodiscard]] Bytecodes* getBytecodes() const;

    [[nodiscard]] int size() const {
        return buffer.pos / kValueSize;
    };
private:
    MemoryBuffer buffer {};
};


#endif