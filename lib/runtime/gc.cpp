//
// Created by Qing Xu on 2021/11/15.
//

#include "joyeer/runtime/gc.h"
#include <cassert>

intptr_t GC::allocate(MemoryArea area, Class *klass) {
    if(area == MemoryArea::Permanent) {
        return allocateForStatic(klass);
    } else {
//        return reinterpret_cast<intptr_t>(allocateForDynamic(klass));
    }

    assert(false);
}

intptr_t GC::allocate(Type *type, size_t size) {
    auto address = heap->allocate(size);
    auto head = reinterpret_cast<ObjectHead*>(address);
    head->absent = false;
    head->typeSlot = type->slot;
    return address;
}

intptr_t GC::allocateForStatic(Class *klass) {
    size_t size = klass->getStaticSize();
    auto address = heap->allocate(size);
    return address;
}

void GC::write(intptr_t memory, Value value, int offset) {
    Value *address = reinterpret_cast<Value*>(memory) + offset;
    *address = value;
}

Value GC::read(intptr_t memory, int offset) {
    return *(reinterpret_cast<Value*>(memory) + offset);
}

