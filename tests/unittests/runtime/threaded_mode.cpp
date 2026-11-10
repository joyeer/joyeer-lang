// "Threaded mode" for the whole runtime test suite (decision 0017). The runtime takes its locks only
// while another thread exists, so a single-threaded test run never executes a locked path and
// cannot find a deadlock inside one (for example an exported function that takes a lock calling
// another that takes it again). With JOYEER_TEST_THREADED=1 this environment starts one thread that
// sleeps on a condition for the whole run, which keeps the unjoined-thread count above zero, so every
// existing test runs through the locks. CTest runs the suite once this way as `RuntimeTestsThreadedMode`.

#include "joyeer/runtime.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>

namespace {

struct ParkedThread {
    alignas(8) unsigned char thread[JOYEER_THREAD_BYTES];
    alignas(8) unsigned char mutex[JOYEER_LOCK_BYTES];
    alignas(8) unsigned char condition[JOYEER_LOCK_BYTES];
    bool release = false;
    bool running = false;
};

ParkedThread parked;

void sleepUntilReleased(void*) {
    joyeer_mutex_lock_abi(parked.mutex);
    while (!parked.release) joyeer_condition_wait_abi(parked.condition, parked.mutex);
    joyeer_mutex_unlock_abi(parked.mutex);
}

class ThreadedModeEnvironment : public testing::Environment {
public:
    void SetUp() override {
        const char* enabled = std::getenv("JOYEER_TEST_THREADED");
        if (enabled == nullptr || std::strcmp(enabled, "1") != 0) return;
        std::memset(&parked, 0, sizeof(parked));
        ASSERT_EQ(joyeer_mutex_init_abi(parked.mutex), 0);
        ASSERT_EQ(joyeer_condition_init_abi(parked.condition), 0);
        ASSERT_EQ(joyeer_thread_start_abi(parked.thread, sleepUntilReleased, nullptr), 0);
        parked.running = true;
    }

    void TearDown() override {
        if (!parked.running) return;
        joyeer_mutex_lock_abi(parked.mutex);
        parked.release = true;
        joyeer_condition_broadcast_abi(parked.condition);
        joyeer_mutex_unlock_abi(parked.mutex);
        joyeer_thread_join_abi(parked.thread);
        parked.running = false;
    }
};

const auto* const registered = testing::AddGlobalTestEnvironment(new ThreadedModeEnvironment);

} // namespace
