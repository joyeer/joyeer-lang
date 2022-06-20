//
// Created by Qing Xu on 2021/11/15.
//

#ifndef __joyeer_vm_gc_h__
#define __joyeer_vm_gc_h__

#include "heaps.h"

enum MemoryArea : uint8_t {
    Permanent,
    Dynamic
};

class GC {
public:
    AddressPtr allocate(MemoryArea area, Class* klass);
    AddressPtr allocate(Type* type, size_t size);
    static void write(AddressPtr memory, Value value, int offset);
    static Value read(AddressPtr memory, int offset);
private:
    AddressPtr allocateForStatic(Class* klass);
private:
    Heap* heap { new Heap() };
};


#endif //__joyeer_vm_gc_h__
