//
// Created by Qing Xu on 2021/11/15.
//

#include "joyeer/runtime/heaps.h"
#include <cassert>

intptr_t Page::allocate(size_t size) {
    if(used + size > kPageSize) {
        assert(false);
        return kInvalid;
    }

    intptr_t r = used;
    used += static_cast<intptr_t>(size);
    for(int i = 0 ; i < size ; i ++) {
        assert(data[r + i] == 0);
    }
    memset(&data[r], 0, size);
    return reinterpret_cast<intptr_t>(&data[r]);
}
