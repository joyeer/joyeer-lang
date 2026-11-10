// The runtime's thread, mutex and condition functions (decision 0016), driven through the
// exported C ABI with C++ entry functions. These tests keep their entry
// functions to atomics, plain memory under a lock, and the functions under test; the runtime's own
// thread safety is tested in thread_safety_test.cpp (decision 0017).

#include "joyeer/runtime.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

// The compiler's task block (decision 0018, taskRecordBytes in compiler/backend/native_collections.cpp)
// reserves exactly this many bytes for the thread record; change them together.
static_assert(JOYEER_THREAD_BYTES == 128, "update taskRecordBytes in the backend with this value");

#ifdef _WIN32
#include <windows.h>
static_assert(sizeof(SRWLOCK) <= JOYEER_LOCK_BYTES && alignof(SRWLOCK) <= 8,
              "a Windows SRWLOCK must fit the caller-supplied lock storage");
static_assert(sizeof(CONDITION_VARIABLE) <= JOYEER_LOCK_BYTES && alignof(CONDITION_VARIABLE) <= 8,
              "a Windows CONDITION_VARIABLE must fit the caller-supplied lock storage");
#else
#include <pthread.h>
static_assert(sizeof(pthread_mutex_t) <= JOYEER_LOCK_BYTES && alignof(pthread_mutex_t) <= 8,
              "a pthread_mutex_t must fit the caller-supplied lock storage");
static_assert(sizeof(pthread_cond_t) <= JOYEER_LOCK_BYTES && alignof(pthread_cond_t) <= 8,
              "a pthread_cond_t must fit the caller-supplied lock storage");
static_assert(sizeof(pthread_attr_t) <= JOYEER_THREAD_BYTES - 32,
              "a pthread_attr_t must fit the thread record");
#endif

namespace {

struct alignas(8) Thread {
    unsigned char bytes[JOYEER_THREAD_BYTES];
};

struct alignas(8) Lock {
    unsigned char bytes[JOYEER_LOCK_BYTES];
};

Thread makeThread() {
    Thread thread;
    for (auto& byte : thread.bytes) byte = 0;
    return thread;
}

Lock makeLock() {
    Lock lock;
    for (auto& byte : lock.bytes) byte = 0;
    return lock;
}

void setFlag(void* argument) {
    static_cast<std::atomic<int>*>(argument)->store(1);
}

void addOne(void* argument) {
    static_cast<std::atomic<int>*>(argument)->fetch_add(1);
}

TEST(RuntimeThreadTest, StartsAndJoinsAThread) {
    std::atomic<int> flag { 0 };
    auto thread = makeThread();
    ASSERT_EQ(joyeer_thread_start_abi(&thread, setFlag, &flag), 0);
    ASSERT_EQ(joyeer_thread_join_abi(&thread), 0);
    EXPECT_EQ(flag.load(), 1);
}

TEST(RuntimeThreadTest, ManyShortThreadsAllRun) {
    constexpr int count = 64;
    std::atomic<int> total { 0 };
    std::vector<Thread> threads(count, makeThread());
    for (auto& thread : threads) {
        ASSERT_EQ(joyeer_thread_start_abi(&thread, addOne, &total), 0);
    }
    for (auto& thread : threads) ASSERT_EQ(joyeer_thread_join_abi(&thread), 0);
    EXPECT_EQ(total.load(), count);
}

struct Message {
    int value = 0;
};

void writeValue(void* argument) {
    static_cast<Message*>(argument)->value = 42;
}

// A plain write made before a thread exits is visible after the join, with no other synchronization.
TEST(RuntimeThreadTest, AWriteBeforeExitIsSeenAfterJoin) {
    for (int round = 0; round < 200; ++round) {
        Message message;
        auto thread = makeThread();
        ASSERT_EQ(joyeer_thread_start_abi(&thread, writeValue, &message), 0);
        ASSERT_EQ(joyeer_thread_join_abi(&thread), 0);
        ASSERT_EQ(message.value, 42);
    }
}

struct Counter {
    Lock lock = makeLock();
    long long value = 0; // deliberately not atomic: only the mutex protects it
};

void incrementUnderLock(void* argument) {
    auto* counter = static_cast<Counter*>(argument);
    for (int index = 0; index < 20000; ++index) {
        joyeer_mutex_lock_abi(&counter->lock);
        counter->value = counter->value + 1;
        joyeer_mutex_unlock_abi(&counter->lock);
    }
}

TEST(RuntimeThreadTest, AMutexGivesMutualExclusion) {
    Counter counter;
    ASSERT_EQ(joyeer_mutex_init_abi(&counter.lock), 0);
    constexpr int count = 8;
    std::vector<Thread> threads(count, makeThread());
    for (auto& thread : threads) {
        ASSERT_EQ(joyeer_thread_start_abi(&thread, incrementUnderLock, &counter), 0);
    }
    for (auto& thread : threads) ASSERT_EQ(joyeer_thread_join_abi(&thread), 0);
    EXPECT_EQ(counter.value, 20000LL * count);
    EXPECT_EQ(joyeer_mutex_destroy_abi(&counter.lock), 0);
}

// A ping-pong between two threads over one mutex and condition: neither wake is lost, or the
// test hangs.
struct PingPong {
    Lock lock = makeLock();
    Lock condition = makeLock();
    int turn = 0; // 0: the main thread moves, 1: the worker moves
    int rounds = 0;
};

void worker(void* argument) {
    auto* state = static_cast<PingPong*>(argument);
    for (int round = 0; round < state->rounds; ++round) {
        joyeer_mutex_lock_abi(&state->lock);
        while (state->turn != 1) joyeer_condition_wait_abi(&state->condition, &state->lock);
        state->turn = 0;
        joyeer_condition_signal_abi(&state->condition);
        joyeer_mutex_unlock_abi(&state->lock);
    }
}

TEST(RuntimeThreadTest, WaitAndWakeLoseNoWakeup) {
    PingPong state;
    state.rounds = 2000;
    ASSERT_EQ(joyeer_mutex_init_abi(&state.lock), 0);
    ASSERT_EQ(joyeer_condition_init_abi(&state.condition), 0);
    auto thread = makeThread();
    ASSERT_EQ(joyeer_thread_start_abi(&thread, worker, &state), 0);
    for (int round = 0; round < state.rounds; ++round) {
        joyeer_mutex_lock_abi(&state.lock);
        state.turn = 1;
        joyeer_condition_signal_abi(&state.condition);
        while (state.turn != 0) joyeer_condition_wait_abi(&state.condition, &state.lock);
        joyeer_mutex_unlock_abi(&state.lock);
    }
    ASSERT_EQ(joyeer_thread_join_abi(&thread), 0);
    EXPECT_EQ(joyeer_condition_destroy_abi(&state.condition), 0);
    EXPECT_EQ(joyeer_mutex_destroy_abi(&state.lock), 0);
}

struct Gate {
    Lock lock = makeLock();
    Lock condition = makeLock();
    bool open = false;
    std::atomic<int> waiting { 0 };
};

void waitForGate(void* argument) {
    auto* gate = static_cast<Gate*>(argument);
    joyeer_mutex_lock_abi(&gate->lock);
    gate->waiting.fetch_add(1);
    while (!gate->open) joyeer_condition_wait_abi(&gate->condition, &gate->lock);
    joyeer_mutex_unlock_abi(&gate->lock);
}

// At most JOYEER_MAX_THREADS threads are unjoined at once; the next start fails with a checkable
// code, and joining frees room.
TEST(RuntimeThreadTest, TheLiveThreadLimitIsEnforcedAndJoiningFreesRoom) {
    Gate gate;
    ASSERT_EQ(joyeer_mutex_init_abi(&gate.lock), 0);
    ASSERT_EQ(joyeer_condition_init_abi(&gate.condition), 0);
    std::vector<Thread> threads(JOYEER_MAX_THREADS, makeThread());
    for (auto& thread : threads) {
        ASSERT_EQ(joyeer_thread_start_abi(&thread, waitForGate, &gate), 0);
    }
    auto extra = makeThread();
    EXPECT_EQ(joyeer_thread_start_abi(&extra, waitForGate, &gate), JOYEER_THREAD_LIMIT);

    joyeer_mutex_lock_abi(&gate.lock);
    gate.open = true;
    joyeer_condition_broadcast_abi(&gate.condition);
    joyeer_mutex_unlock_abi(&gate.lock);
    for (auto& thread : threads) ASSERT_EQ(joyeer_thread_join_abi(&thread), 0);

    // The count is back to zero, so a thread starts again.
    std::atomic<int> flag { 0 };
    ASSERT_EQ(joyeer_thread_start_abi(&extra, setFlag, &flag), 0);
    ASSERT_EQ(joyeer_thread_join_abi(&extra), 0);
    EXPECT_EQ(flag.load(), 1);
    EXPECT_EQ(joyeer_condition_destroy_abi(&gate.condition), 0);
    EXPECT_EQ(joyeer_mutex_destroy_abi(&gate.lock), 0);
}

TEST(RuntimeThreadTest, NullArgumentsAreRejectedNotDereferenced) {
    std::atomic<int> flag { 0 };
    auto thread = makeThread();
    EXPECT_EQ(joyeer_thread_start_abi(nullptr, setFlag, &flag), JOYEER_THREAD_INVALID);
    EXPECT_EQ(joyeer_thread_start_abi(&thread, nullptr, &flag), JOYEER_THREAD_INVALID);
    EXPECT_EQ(joyeer_thread_join_abi(nullptr), JOYEER_THREAD_INVALID);
    EXPECT_EQ(joyeer_mutex_init_abi(nullptr), JOYEER_THREAD_INVALID);
    EXPECT_EQ(joyeer_mutex_lock_abi(nullptr), JOYEER_THREAD_INVALID);
    EXPECT_EQ(joyeer_condition_wait_abi(nullptr, nullptr), JOYEER_THREAD_INVALID);
    // The limit count is unchanged by the rejected starts.
    ASSERT_EQ(joyeer_thread_start_abi(&thread, setFlag, &flag), 0);
    ASSERT_EQ(joyeer_thread_join_abi(&thread), 0);
}

} // namespace
