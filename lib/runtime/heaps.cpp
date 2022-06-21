//
// Created by Qing Xu on 2021/11/15.
//

#include "joyeer/runtime/heaps.h"
#include <cassert>

AddressPtr Page::allocate(size_t size) {
    if(used + size > kPageSize) {
        assert(false);
        return kInvalid;
    }

    AddressPtr r = used;
    used += static_cast<AddressPtr>(size);
    for(int i = 0 ; i < size ; i ++) {
        assert(data[r + i] == 0);
    }
    memset(&data[r], 0, size);
    return reinterpret_cast<AddressPtr>(&data[r]);
}
