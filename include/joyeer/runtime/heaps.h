//
// Created by Qing Xu on 2021/11/15.
//

#ifndef __joyeer_runtime_heaps_h__
#define __joyeer_runtime_heaps_h__

#include <vector>
#include "joyeer/runtime/types.h"

constexpr size_t kPageSize = 256 * 1024;
constexpr size_t kMaxPageNumberInSingleSpace = 1024 * 1024; // the max number of page in one single space
constexpr AddressPtr kInvalid = -1;


// the default page size : 256k
struct Page {

    char8_t data[kPageSize]{};
    AddressPtr used = 0;

    [[nodiscard]] AddressPtr allocate(size_t size);
};


// the space contains multi-pages
struct Space {
    std::vector<Page*> pages { new Page() };
    size_t cur = 0;

    AddressPtr allocate(size_t size) {
        return pages[cur]->allocate(size);
    }
};

constexpr static int kStaticSpace = 0;
constexpr static int kNewSpace = 1;
constexpr static int kOldSpace = 2;
constexpr static int kTotalSpaceCount = 3;

struct Heap {

#define DECLARE_SPACE(space) new Space(),

    Space* spaces[kTotalSpaceCount] {
            DECLARE_SPACE(kStaticSpace)
            DECLARE_SPACE(kNewSpace)
            DECLARE_SPACE(kOldSpace)
    };

#define SPACE(space) spaces[space]
#undef DECLARE_SPACE

    // allocate a memory for a given size
    AddressPtr allocate(size_t size) {
        auto address = SPACE(kNewSpace)->allocate(size);
        return address;
    }
};



#endif //__joyeer_runtime_heaps_h__
