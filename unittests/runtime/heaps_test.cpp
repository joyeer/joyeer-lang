//
// Created by Qing Xu on 2021/11/18.
//

#include "joyeer/runtime/heaps.h"
#include <gtest/gtest.h>

TEST(PageTest, Page) {
    Heap *heap = new Heap();
    heap->allocate(100);
}


