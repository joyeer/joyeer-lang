#include "joyeer/runtime/bytecode.h"
#include <memory>
#include <unordered_map>
#include <sstream>



//-------------------------------------------------------
// Bytecode
//-------------------------------------------------------


Bytecode::Bytecode(int8_t op, int64_t value) {
    Byte* valBytes = (Byte*)(&value);
    bytes[0] = op;
    bytes[1] = valBytes[0];
    bytes[2] = valBytes[1];
    bytes[3] = valBytes[2];
    bytes[4] = valBytes[3];
    bytes[5] = valBytes[4];
    bytes[6] = valBytes[5];
    bytes[7] = valBytes[6];
}

Bytecode::Bytecode(int8_t op, int32_t value1, int32_t value2) {
    bytes[0] = op;
    bytes[1] = (Byte)(value1 & 0xff);
    bytes[2] = (Byte)(value1 >> 8 & 0xff);
    bytes[3] = (Byte)(value1 >> 16 & 0xff);
    bytes[4] = (Byte)(value2 & 0xff);
    bytes[5] = (Byte)(value2 >> 8 & 0xff);
    bytes[6] = (Byte)(value2 >> 16 & 0xff);
    bytes[7] = (Byte)(value2 >> 24 & 0xff);
}

Bytecodes::Bytecodes(Byte *bytes, int length) {
    bytecodes = static_cast<Byte *>(malloc(length));
    memcpy(bytecodes, bytes, length);
    size = length / sizeof(Bytecode);
}

Bytecodes::~Bytecodes() {
    free((void*)bytecodes);
}


//-------------------------------------------------------
// BytecodeWriter
//-------------------------------------------------------

void BytecodeWriter::write(Bytecode bytecode) {
    buffer.write(bytecode);
}

void BytecodeWriter::write(Bytecodes *bytecodes) {
    for(auto i = 0; i < bytecodes->size; i ++ ) {
        auto bytecode = ((Bytecode*)bytecodes->bytecodes)[i];
        buffer.write(bytecode);
    }
}

Bytecodes *BytecodeWriter::getBytecodes() const {
    return new Bytecodes(buffer.buffer, buffer.pos);
}
