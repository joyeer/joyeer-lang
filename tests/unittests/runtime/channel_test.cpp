// The runtime's channels (decision 0019), driven through the exported C ABI. A wait that could
// never end traps when no unjoined thread exists, so every test that blocks starts its threads with
// `joyeer_thread_start_abi`. Entry functions use no gtest assertions (gtest is not thread safe): they
// record what they saw in atomics and the test thread asserts after the join.

#include "joyeer/runtime.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace {

struct alignas(8) Thread {
    unsigned char bytes[JOYEER_THREAD_BYTES];
};

struct Pair {
    void* sender = nullptr;
    void* receiver = nullptr;
};

std::atomic<int> destroyed {0};

void countDestroy(void*) {
    destroyed.fetch_add(1);
}

Pair make(int64_t capacity, int64_t size = sizeof(int64_t), int64_t alignment = alignof(int64_t),
        JoyeerDestroyValueFn destroy = nullptr) {
    Pair pair;
    EXPECT_EQ(joyeer_channel_create_abi(capacity, size, alignment, destroy, &pair.sender, &pair.receiver), 0);
    return pair;
}

int64_t send(void* sender, int64_t value) {
    return joyeer_channel_send_abi(sender, &value);
}

// Receives into `value`; returns whether a value arrived.
bool receive(void* receiver, int64_t& value) {
    return joyeer_channel_receive_abi(receiver, &value) == 1;
}

void start(Thread& thread, JoyeerThreadEntry entry, void* argument) {
    std::memset(thread.bytes, 0, sizeof(thread.bytes));
    ASSERT_EQ(joyeer_thread_start_abi(&thread, entry, argument), 0);
}

} // namespace

TEST(ChannelTest, KeepsOrderAcrossWraparound) {
    const auto baseline = joyeer_runtime_active_allocations();
    const auto pair = make(3);
    for (int64_t value = 1; value <= 3; ++value) ASSERT_EQ(send(pair.sender, value), 1);
    int64_t received = 0;
    ASSERT_TRUE(receive(pair.receiver, received));
    EXPECT_EQ(received, 1);
    ASSERT_TRUE(receive(pair.receiver, received));
    EXPECT_EQ(received, 2);
    ASSERT_EQ(send(pair.sender, 4), 1);
    ASSERT_EQ(send(pair.sender, 5), 1);
    for (int64_t expected = 3; expected <= 5; ++expected) {
        ASSERT_TRUE(receive(pair.receiver, received));
        EXPECT_EQ(received, expected);
    }
    joyeer_channel_close_abi(pair.sender);
    joyeer_channel_close_abi(pair.receiver);
    EXPECT_EQ(joyeer_runtime_active_allocations(), baseline);
}

TEST(ChannelTest, SendToAClosedReceiverReturnsZeroAndKeepsOwnership) {
    destroyed = 0;
    const auto pair = make(2, sizeof(int64_t), alignof(int64_t), countDestroy);
    joyeer_channel_close_abi(pair.receiver);
    EXPECT_EQ(send(pair.sender, 7), 0);
    // The channel did not take the value, so it did not destroy it either.
    EXPECT_EQ(destroyed.load(), 0);
    joyeer_channel_close_abi(pair.sender);
}

TEST(ChannelTest, ClosingTheSenderLeavesQueuedValuesForTheReceiver) {
    destroyed = 0;
    const auto pair = make(4, sizeof(int64_t), alignof(int64_t), countDestroy);
    ASSERT_EQ(send(pair.sender, 10), 1);
    ASSERT_EQ(send(pair.sender, 20), 1);
    joyeer_channel_close_abi(pair.sender);
    int64_t received = 0;
    ASSERT_TRUE(receive(pair.receiver, received));
    EXPECT_EQ(received, 10);
    ASSERT_TRUE(receive(pair.receiver, received));
    EXPECT_EQ(received, 20);
    // Empty and closed: reports closed without waiting, even with no other thread.
    EXPECT_FALSE(receive(pair.receiver, received));
    EXPECT_EQ(destroyed.load(), 0);
    joyeer_channel_close_abi(pair.receiver);
}

TEST(ChannelTest, ClosingTheReceiverDestroysQueuedValues) {
    destroyed = 0;
    const auto baseline = joyeer_runtime_active_allocations();
    const auto pair = make(4, sizeof(int64_t), alignof(int64_t), countDestroy);
    for (int64_t value = 0; value < 3; ++value) ASSERT_EQ(send(pair.sender, value), 1);
    int64_t received = 0;
    ASSERT_TRUE(receive(pair.receiver, received));
    joyeer_channel_close_abi(pair.receiver);
    EXPECT_EQ(destroyed.load(), 2);
    joyeer_channel_close_abi(pair.sender);
    EXPECT_EQ(destroyed.load(), 2);
    EXPECT_EQ(joyeer_runtime_active_allocations(), baseline);
}

TEST(ChannelTest, BothCloseOrdersFreeTheBlockExactlyOnce) {
    const auto baseline = joyeer_runtime_active_allocations();
    for (int round = 0; round < 500; ++round) {
        const auto pair = make(2);
        send(pair.sender, round);
        if (round % 2 == 0) {
            joyeer_channel_close_abi(pair.sender);
            joyeer_channel_close_abi(pair.receiver);
        } else {
            joyeer_channel_close_abi(pair.receiver);
            joyeer_channel_close_abi(pair.sender);
        }
    }
    EXPECT_EQ(joyeer_runtime_active_allocations(), baseline);
    joyeer_channel_close_abi(nullptr);
}

TEST(ChannelTest, AZeroSizeElementNeedsNoRing) {
    const auto baseline = joyeer_runtime_active_allocations();
    const auto pair = make(3, 0, 1);
    for (int index = 0; index < 8; ++index) {
        ASSERT_EQ(joyeer_channel_send_abi(pair.sender, nullptr), 1);
        ASSERT_EQ(joyeer_channel_receive_abi(pair.receiver, nullptr), 1);
    }
    joyeer_channel_close_abi(pair.sender);
    joyeer_channel_close_abi(pair.receiver);
    EXPECT_EQ(joyeer_runtime_active_allocations(), baseline);
}

namespace {

struct Wide {
    alignas(16) unsigned char bytes[48];
};

} // namespace

TEST(ChannelTest, AWideAlignedElementKeepsItsBytes) {
    Pair pair;
    ASSERT_EQ(joyeer_channel_create_abi(2, sizeof(Wide), alignof(Wide), nullptr, &pair.sender, &pair.receiver), 0);
    Wide sent {};
    for (size_t index = 0; index < sizeof(sent.bytes); ++index) sent.bytes[index] = static_cast<unsigned char>(index + 1);
    ASSERT_EQ(joyeer_channel_send_abi(pair.sender, &sent), 1);
    Wide received {};
    ASSERT_EQ(joyeer_channel_receive_abi(pair.receiver, &received), 1);
    EXPECT_EQ(std::memcmp(sent.bytes, received.bytes, sizeof(sent.bytes)), 0);
    joyeer_channel_close_abi(pair.sender);
    joyeer_channel_close_abi(pair.receiver);
}

namespace {

constexpr int64_t messages = 10000;

struct Producer {
    void* sender;
    std::atomic<int64_t> sent {0};
};

void produce(void* argument) {
    auto* producer = static_cast<Producer*>(argument);
    for (int64_t value = 1; value <= messages; ++value) {
        if (joyeer_channel_send_abi(producer->sender, &value) != 1) return;
        producer->sent.fetch_add(1);
    }
    joyeer_channel_close_abi(producer->sender);
}

} // namespace

// A producer thread sends 10000 values through a queue of four and closes; the test thread receives
// them in order and then sees the closed result.
TEST(ChannelTest, AProducerAndAConsumerExchangeManyValuesInOrder) {
    const auto baseline = joyeer_runtime_active_allocations();
    const auto pair = make(4);
    Producer producer {pair.sender};
    Thread thread;
    start(thread, produce, &producer);
    int64_t sum = 0;
    int64_t expected = 1;
    int64_t received = 0;
    bool inOrder = true;
    while (receive(pair.receiver, received)) {
        inOrder &= received == expected++;
        sum += received;
    }
    ASSERT_EQ(joyeer_thread_join_abi(&thread), 0);
    EXPECT_TRUE(inOrder);
    EXPECT_EQ(expected - 1, messages);
    EXPECT_EQ(sum, messages * (messages + 1) / 2);
    joyeer_channel_close_abi(pair.receiver);
    EXPECT_EQ(joyeer_runtime_active_allocations(), baseline);
}

namespace {

struct Waiter {
    void* end;
    std::atomic<int> result {-1};
};

void waitToReceive(void* argument) {
    auto* waiter = static_cast<Waiter*>(argument);
    int64_t value = 0;
    waiter->result = static_cast<int>(joyeer_channel_receive_abi(waiter->end, &value));
}

void waitToSend(void* argument) {
    auto* waiter = static_cast<Waiter*>(argument);
    int64_t value = 99;
    waiter->result = static_cast<int>(joyeer_channel_send_abi(waiter->end, &value));
}

} // namespace

TEST(ChannelTest, ClosingTheSenderWakesABlockedReceiver) {
    const auto pair = make(2);
    Waiter waiter {pair.receiver};
    Thread thread;
    start(thread, waitToReceive, &waiter);
    joyeer_channel_close_abi(pair.sender);
    ASSERT_EQ(joyeer_thread_join_abi(&thread), 0);
    EXPECT_EQ(waiter.result.load(), 0);
    joyeer_channel_close_abi(pair.receiver);
}

TEST(ChannelTest, ClosingTheReceiverWakesABlockedSenderAndDestroysTheQueue) {
    destroyed = 0;
    const auto pair = make(1, sizeof(int64_t), alignof(int64_t), countDestroy);
    ASSERT_EQ(send(pair.sender, 1), 1);
    Waiter waiter {pair.sender};
    Thread thread;
    start(thread, waitToSend, &waiter);
    joyeer_channel_close_abi(pair.receiver);
    ASSERT_EQ(joyeer_thread_join_abi(&thread), 0);
    EXPECT_EQ(waiter.result.load(), 0);
    // Only the queued value was destroyed; the one that was never sent is its sender's.
    EXPECT_EQ(destroyed.load(), 1);
    joyeer_channel_close_abi(pair.sender);
}

TEST(ChannelDeathTest, InvalidArgumentsTrap) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    Pair pair;
    EXPECT_DEATH(joyeer_channel_create_abi(0, 8, 8, nullptr, &pair.sender, &pair.receiver),
            "invalid channel capacity");
    EXPECT_DEATH(joyeer_channel_create_abi(INT64_MAX / 2, 64, 8, nullptr, &pair.sender, &pair.receiver),
            "channel too large");
    EXPECT_DEATH(joyeer_channel_send_abi(nullptr, nullptr), "invalid channel sender");
    EXPECT_DEATH(joyeer_channel_receive_abi(nullptr, nullptr), "invalid channel receiver");
}

// With no other thread nothing can ever wake a blocked operation, so it traps instead of hanging.
TEST(ChannelDeathTest, ABlockingOperationWithNoOtherThreadTraps) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
            {
                const auto pair = make(1);
                int64_t value = 0;
                joyeer_channel_receive_abi(pair.receiver, &value);
            },
            "channel deadlock");
    EXPECT_DEATH(
            {
                const auto pair = make(1);
                send(pair.sender, 1);
                send(pair.sender, 2);
            },
            "channel deadlock");
}
