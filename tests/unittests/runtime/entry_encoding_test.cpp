#include "joyeer/native/runtime.h"

#include <gtest/gtest.h>

#include <string>

namespace {
std::string observedArgument;
}

extern "C" int joyeer_test_entry_wmain(int argc, wchar_t** argv);

extern "C" int joyeer_main_uses_arguments(void) {
    return 1;
}

extern "C" int64_t joyeer_main(const JoyeerArray* arguments) {
    if (arguments->count != 1) return 1;
    const auto* argument = static_cast<const JoyeerString*>(
            joyeer_array_at_abi(arguments->data, arguments->count, 0));
    observedArgument.assign(
            reinterpret_cast<const char*>(argument->data),
            static_cast<size_t>(argument->count));
    return 0;
}

TEST(NativeEntryEncodingDeathTest, RejectsUnpairedHighSurrogate) {
    wchar_t executable[] = L"entry-test";
    wchar_t invalid[] { 0xd800, 0 };
    wchar_t* arguments[] { executable, invalid };
    EXPECT_DEATH(
            static_cast<void>(joyeer_test_entry_wmain(2, arguments)),
            "invalid Windows command line argument encoding");
}

TEST(NativeEntryEncodingDeathTest, RejectsUnpairedLowSurrogate) {
    wchar_t executable[] = L"entry-test";
    wchar_t invalid[] { 0xdc00, 0 };
    wchar_t* arguments[] { executable, invalid };
    EXPECT_DEATH(
            static_cast<void>(joyeer_test_entry_wmain(2, arguments)),
            "invalid Windows command line argument encoding");
}

TEST(NativeEntryEncodingTest, PreservesSupplementaryUnicodeAndCleansStorage) {
    wchar_t executable[] = L"entry-test";
    wchar_t valid[] { 0xd83d, 0xde00, 0 };
    wchar_t* arguments[] { executable, valid };
    ASSERT_EQ(joyeer_runtime_active_allocations(), 0);
    EXPECT_EQ(joyeer_test_entry_wmain(2, arguments), 0);
    EXPECT_EQ(observedArgument, "\xf0\x9f\x98\x80");
    EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
}
