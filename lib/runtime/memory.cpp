//
// Created by Qing Xu on 2021/11/15.
//

#include "joyeer/runtime/memory.h"
#include "joyeer/runtime/bytecode.h"
#include <new>


void MemoryBuffer::write(Bytecode bytecode) {
    tryResizeIfNeed(pos + sizeof(Bytecode));
    Byte* bytes = (Byte*) & bytecode;
    memcpy(buffer + pos, bytes, sizeof(Bytecode));
    pos += sizeof(Bytecode);
}

void MemoryBuffer::resize(size_t newSize) {
    if(newSize == 0) { // this check is not strictly needed,
        std::free(buffer);  // but zero-size realloc is deprecated in C
        buffer = nullptr;
    } else {
        if(void* mem = std::realloc(buffer, newSize))
            buffer = (Byte *)mem;
        else
            throw std::bad_alloc(); //TODO: runtime exception??
    }

    capacity = newSize;
}
