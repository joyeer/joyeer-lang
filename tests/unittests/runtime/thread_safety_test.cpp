// The runtime's thread safety (decision 0017): the allocator, the standard output buffer and a
// panic under several threads started with the thread API (decision 0016). Locking is taken only
// while a thread exists, so every test here starts its threads through `joyeer_thread_start_abi`.
//
// Entry functions are C++ `void(void *)`; none of them uses gtest assertions, which are not
// thread safe, so they record what they saw and the test thread asserts.

#include "joyeer/runtime.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#define JOYEER_OPEN(path) _open(path, _O_CREAT | _O_TRUNC | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE)
#define JOYEER_DUP _dup
#define JOYEER_DUP2 _dup2
#define JOYEER_CLOSE _close
#define JOYEER_FILENO _fileno
#else
#include <fcntl.h>
#include <unistd.h>
#define JOYEER_OPEN(path) ::open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600)
#define JOYEER_DUP dup
#define JOYEER_DUP2 dup2
#define JOYEER_CLOSE close
#define JOYEER_FILENO fileno
#endif

namespace {

struct alignas(8) Thread {
    unsigned char bytes[JOYEER_THREAD_BYTES];
};

std::vector<Thread> makeThreads(size_t count) {
    std::vector<Thread> threads(count);
    for (auto& thread : threads) std::memset(thread.bytes, 0, sizeof(thread.bytes));
    return threads;
}

void startAll(std::vector<Thread>& threads, JoyeerThreadEntry entry, std::vector<void*> arguments) {
    for (size_t index = 0; index < threads.size(); ++index) {
        ASSERT_EQ(joyeer_thread_start_abi(&threads[index], entry, arguments[index]), 0);
    }
}

void joinAll(std::vector<Thread>& threads) {
    for (auto& thread : threads) ASSERT_EQ(joyeer_thread_join_abi(&thread), 0);
}

// ---- The allocator --------------------------------------------------------------------

constexpr int blocksPerThread = 4000;
constexpr int sizes[] = { 16, 24, 48, 100, 512, 1000, 4096, 40000 };

struct StressShared {
    std::vector<std::vector<unsigned char*>> blocks;
    std::vector<std::vector<int>> blockSizes;
    std::atomic<int> allocated { 0 };
    std::atomic<int> corrupt { 0 };
    int threads = 0;
};

struct StressArgument {
    StressShared* shared;
    int index;
};

unsigned char patternByte(int thread, int block, int offset) {
    return static_cast<unsigned char>(thread * 31 + block * 7 + offset);
}

// Allocates and fills blocks, waits until every thread has, then frees the blocks of the next
// thread over: every free is on a different thread than the allocation.
void allocateThenFreeNeighbours(void* raw) {
    auto* argument = static_cast<StressArgument*>(raw);
    auto& shared = *argument->shared;
    auto& mine = shared.blocks[argument->index];
    auto& mySizes = shared.blockSizes[argument->index];
    for (int block = 0; block < blocksPerThread; ++block) {
        const int size = sizes[(block + argument->index) % (sizeof(sizes) / sizeof(sizes[0]))];
        auto* address = static_cast<unsigned char*>(joyeer_alloc(static_cast<size_t>(size)));
        for (int offset = 0; offset < size; offset += 97) {
            address[offset] = patternByte(argument->index, block, offset);
        }
        mine.push_back(address);
        mySizes.push_back(size);
    }
    shared.allocated.fetch_add(1);
    while (shared.allocated.load() < shared.threads) { /* wait for every thread's blocks */ }
    const int neighbour = (argument->index + 1) % shared.threads;
    auto& theirs = shared.blocks[neighbour];
    auto& theirSizes = shared.blockSizes[neighbour];
    for (int block = 0; block < blocksPerThread; ++block) {
        for (int offset = 0; offset < theirSizes[block]; offset += 97) {
            if (theirs[block][offset] != patternByte(neighbour, block, offset)) shared.corrupt.fetch_add(1);
        }
        joyeer_free(theirs[block], static_cast<size_t>(theirSizes[block]));
    }
}

class RuntimeThreadSafetyTest : public testing::Test {
protected:
    void SetUp() override { before = joyeer_runtime_active_allocations(); }
    int64_t before = 0;
};

TEST_F(RuntimeThreadSafetyTest, BlocksFreedOnAnotherThreadReturnTheCounterToZero) {
    for (const int threadCount : { 2, 4, 8 }) {
        StressShared shared;
        shared.threads = threadCount;
        shared.blocks.resize(static_cast<size_t>(threadCount));
        shared.blockSizes.resize(static_cast<size_t>(threadCount));
        std::vector<StressArgument> arguments;
        for (int index = 0; index < threadCount; ++index) arguments.push_back({ &shared, index });
        auto threads = makeThreads(static_cast<size_t>(threadCount));
        std::vector<void*> raw;
        for (auto& argument : arguments) raw.push_back(&argument);
        startAll(threads, allocateThenFreeNeighbours, raw);
        joinAll(threads);
        EXPECT_EQ(shared.corrupt.load(), 0) << threadCount << " threads";
        EXPECT_EQ(joyeer_runtime_active_allocations(), before) << threadCount << " threads";
    }
}

struct ChurnArgument {
    int index;
    int rounds;
};

// Small blocks allocated and freed on the spot, hammering the same size classes from every thread.
void churn(void* raw) {
    auto* argument = static_cast<ChurnArgument*>(raw);
    for (int round = 0; round < argument->rounds; ++round) {
        void* blocks[16];
        for (int slot = 0; slot < 16; ++slot) {
            blocks[slot] = joyeer_alloc(static_cast<size_t>(16 + slot * 16));
            std::memset(blocks[slot], argument->index, static_cast<size_t>(16 + slot * 16));
        }
        for (int slot = 0; slot < 16; ++slot) joyeer_free(blocks[slot], static_cast<size_t>(16 + slot * 16));
    }
}

TEST_F(RuntimeThreadSafetyTest, ConcurrentAllocateAndFreeKeepsTheCounterBalanced) {
    constexpr int count = 8;
    std::vector<ChurnArgument> arguments;
    for (int index = 0; index < count; ++index) arguments.push_back({ index, 5000 });
    auto threads = makeThreads(count);
    std::vector<void*> raw;
    for (auto& argument : arguments) raw.push_back(&argument);
    startAll(threads, churn, raw);
    joinAll(threads);
    EXPECT_EQ(joyeer_runtime_active_allocations(), before);
}

// ---- Standard output ------------------------------------------------------------------

struct Capture {
    int saved = -1;
    std::string path;

    void begin() {
        std::fflush(stdout);
        joyeer_stdout_flush();
        path = std::string(std::tmpnam(nullptr));
        const int file = JOYEER_OPEN(path.c_str());
        ASSERT_GE(file, 0);
        saved = JOYEER_DUP(JOYEER_FILENO(stdout));
        JOYEER_DUP2(file, JOYEER_FILENO(stdout));
        JOYEER_CLOSE(file);
    }

    std::string end() {
        joyeer_stdout_flush();
        JOYEER_DUP2(saved, JOYEER_FILENO(stdout));
        JOYEER_CLOSE(saved);
        std::ifstream input(path, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        std::remove(path.c_str());
        return content;
    }
};

struct PrintArgument {
    int index;
    int lines;
};

void printLines(void* raw) {
    auto* argument = static_cast<PrintArgument*>(raw);
    // A 120-byte line that names its writer: a split would mix two writers' bytes.
    char line[121];
    std::memset(line, 'a' + argument->index, 120);
    for (int number = 0; number < argument->lines; ++number) {
        joyeer_print_string_abi(reinterpret_cast<const uint8_t*>(line), 120);
    }
}

TEST_F(RuntimeThreadSafetyTest, APrintIsAtomicUnderSeveralWriters) {
    constexpr int writers = 4;
    constexpr int lines = 2000;
    Capture capture;
    capture.begin();
    std::vector<PrintArgument> arguments;
    for (int index = 0; index < writers; ++index) arguments.push_back({ index, lines });
    auto threads = makeThreads(writers);
    std::vector<void*> raw;
    for (auto& argument : arguments) raw.push_back(&argument);
    startAll(threads, printLines, raw);
    joinAll(threads);
    const std::string output = capture.end();

    // Every line is one writer's 120 bytes and a newline, and every writer's lines are all there.
    ASSERT_EQ(output.size(), static_cast<size_t>(writers) * lines * 121);
    int counts[writers] = {};
    for (size_t position = 0; position < output.size(); position += 121) {
        const char first = output[position];
        ASSERT_GE(first, 'a');
        ASSERT_LT(first, 'a' + writers);
        for (size_t offset = 0; offset < 120; ++offset) ASSERT_EQ(output[position + offset], first);
        ASSERT_EQ(output[position + 120], '\n');
        ++counts[first - 'a'];
    }
    for (const int count : counts) EXPECT_EQ(count, lines);
}

void printBigAndSmall(void* raw) {
    // Longer than the 8 KiB buffer, so it bypasses it; it must still not be split.
    static const std::string big(20000, 'z');
    const int index = *static_cast<int*>(raw);
    for (int round = 0; round < 20; ++round) {
        if (index == 0) {
            joyeer_print_string_abi(reinterpret_cast<const uint8_t*>(big.data()), static_cast<int64_t>(big.size()));
        } else {
            joyeer_print_int(1234567);
        }
    }
}

TEST_F(RuntimeThreadSafetyTest, APrintLargerThanTheBufferIsNotSplit) {
    Capture capture;
    capture.begin();
    int zero = 0, one = 1, two = 2;
    auto threads = makeThreads(3);
    startAll(threads, printBigAndSmall, { &zero, &one, &two });
    joinAll(threads);
    const std::string output = capture.end();
    size_t position = 0;
    int bigLines = 0, smallLines = 0;
    while (position < output.size()) {
        const size_t newline = output.find('\n', position);
        ASSERT_NE(newline, std::string::npos);
        const std::string line = output.substr(position, newline - position);
        if (line == std::string(20000, 'z')) ++bigLines;
        else if (line == "1234567") ++smallLines;
        else FAIL() << "a split or mixed line of " << line.size() << " bytes";
        position = newline + 1;
    }
    EXPECT_EQ(bigLines, 20);
    EXPECT_EQ(smallLines, 40);
}

// ---- A process of its own: first use and panic -------------------------------------------------

void allocateAndPrintAtOnce(void* raw) {
    std::atomic<int>* start = static_cast<std::atomic<int>*>(raw);
    while (start->load() == 0) { /* every thread leaves the gate together */ }
    for (int round = 0; round < 200; ++round) {
        void* block = joyeer_alloc(64);
        joyeer_print_int(round);
        joyeer_free(block, 64);
    }
}

// The runtime's lazily created state is process wide, so the first-use race needs a fresh
// process: the death test re-runs this statement in one (the "threadsafe" style).
TEST(RuntimeThreadSafetyDeathTest, TwoThreadsRacingOnTheFirstUseBothSucceed) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
            {
                std::atomic<int> start { 0 };
                auto threads = makeThreads(4);
                for (auto& thread : threads) {
                    if (joyeer_thread_start_abi(&thread, allocateAndPrintAtOnce, &start) != 0) std::_Exit(2);
                }
                start.store(1);
                for (auto& thread : threads) {
                    if (joyeer_thread_join_abi(&thread) != 0) std::_Exit(3);
                }
                joyeer_stdout_flush();
                std::_Exit(joyeer_runtime_active_allocations() == 0 ? 0 : 4);
            },
            testing::ExitedWithCode(0), "");
}

void panicTogether(void* raw) {
    auto* gate = static_cast<std::atomic<int>*>(raw);
    gate->fetch_add(1);
    while (gate->load() < 3) { /* meet the other panickers */ }
    joyeer_panic("boom");
}

// Three threads trap at the same instant: one message, one exit with status 1, no matter who wins.
TEST(RuntimeThreadSafetyDeathTest, SimultaneousPanicsPrintOnceAndExitOnce) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    for (int round = 0; round < 5; ++round) {
        EXPECT_EXIT(
                {
                    static std::atomic<int> gate { 0 };
                    auto threads = makeThreads(3);
                    joyeer_print_int(42); // pending standard output the panic must flush
                    for (auto& thread : threads) {
                        if (joyeer_thread_start_abi(&thread, panicTogether, &gate) != 0) std::_Exit(2);
                    }
                    for (auto& thread : threads) joyeer_thread_join_abi(&thread); // never returns
                    std::_Exit(3);
                },
                testing::ExitedWithCode(1), "^Joyeer runtime error: boom\n$")
                << "round " << round;
    }
}

TEST(RuntimeThreadSafetyDeathTest, ANestedPanicInASingleThreadStillExits) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    // No thread was ever started, so a second panic cannot park: it exits, as it always did.
    EXPECT_EXIT(joyeer_panic("single"), testing::ExitedWithCode(1), "Joyeer runtime error: single");
}

} // namespace
