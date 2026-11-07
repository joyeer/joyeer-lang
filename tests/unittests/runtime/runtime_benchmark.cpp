// Micro-benchmarks for the hot runtime operations, written against the exported
// ABI only so the same program measures any provider of the runtime (the C
// implementation, later the Joyeer one; docs/plan/runtime-in-joyeer.md).
//
// This is not a test and is not registered with CTest. Run it from a Release
// build with `cmake --build <dir> --target runtime-benchmark`. Numbers are for
// comparing two providers on the same machine, not for absolute claims.

#include "runtime/runtime.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

volatile int64_t sink = 0;

struct Result {
    std::string name;
    double nanosecondsPerOperation;
};

// Best of several repetitions: the minimum is the least disturbed by the machine.
template <typename Body>
Result measure(const std::string& name, int64_t operations, Body body) {
    constexpr int repetitions = 7;
    double best = 1e300;
    for (int repetition = 0; repetition < repetitions; ++repetition) {
        const auto start = Clock::now();
        body();
        const auto elapsed = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
        best = std::min(best, elapsed / static_cast<double>(operations));
    }
    return { name, best };
}

std::string patterned(size_t count, char last) {
    std::string text(count, 'x');
    for (size_t index = 0; index < count; ++index) text[index] = static_cast<char>('a' + index % 23);
    if (count != 0) text[count - 1] = last;
    return text;
}

const uint8_t* bytes(const std::string& text) {
    return reinterpret_cast<const uint8_t*>(text.data());
}

int64_t count(const std::string& text) {
    return static_cast<int64_t>(text.size());
}

void cloneInt(void*, const void*) {}

struct IntEntry {
    int64_t key;
    int64_t value;
};

}

int main(int argc, char** argv) {
    // `--quick` runs one tenth of the work, to check that the program still runs.
    const bool quick = argc > 1 && std::strcmp(argv[1], "--quick") == 0;
    const int64_t scale = quick ? 1 : 10;
    std::vector<Result> results;

    for (const size_t size : { size_t { 16 }, size_t { 1024 } }) {
        const std::string left = patterned(size, 'x');
        const std::string same = patterned(size, 'x');
        const std::string differs = patterned(size, 'y');
        const int64_t operations = 200000 * scale;
        const std::string suffix = " (" + std::to_string(size) + " B)";

        results.push_back(measure("string equal, equal" + suffix, operations, [&] {
            int64_t total = 0;
            for (int64_t index = 0; index < operations; ++index) {
                total += joyeer_string_equal_abi(bytes(left), count(left), bytes(same), count(same));
            }
            sink = total;
        }));
        results.push_back(measure("string compare, last byte differs" + suffix, operations, [&] {
            int64_t total = 0;
            for (int64_t index = 0; index < operations; ++index) {
                total += joyeer_string_compare_abi(bytes(left), count(left), bytes(differs), count(differs));
            }
            sink = total;
        }));
        results.push_back(measure("string concat + destroy" + suffix, operations, [&] {
            for (int64_t index = 0; index < operations; ++index) {
                JoyeerString joined {};
                joyeer_string_concat_abi(&joined, bytes(left), count(left), bytes(same), count(same));
                sink = joined.count;
                joyeer_string_destroy_abi(&joined);
            }
        }));
        results.push_back(measure("string clone + destroy" + suffix, operations, [&] {
            for (int64_t index = 0; index < operations; ++index) {
                JoyeerString copy {};
                joyeer_string_clone_abi(&copy, bytes(left), count(left));
                sink = copy.count;
                joyeer_string_destroy_abi(&copy);
            }
        }));
    }

    {
        const int64_t elements = 100000 * scale;
        results.push_back(measure("array append, grow from empty (Int)", elements, [&] {
            JoyeerArray array {};
            joyeer_array_create_owned_abi(&array, nullptr, 0, sizeof(int64_t), nullptr, nullptr);
            for (int64_t value = 0; value < elements; ++value) {
                joyeer_array_append_owned_abi(&array, &value);
            }
            sink = array.count;
            joyeer_array_destroy_abi(&array);
        }));
        results.push_back(measure("array clone + destroy, 1000 elements with callbacks", 1000 * 200, [&] {
            std::vector<int64_t> source(1000, 7);
            JoyeerArray array {};
            joyeer_array_create_owned_abi(&array, source.data(), 1000, sizeof(int64_t), cloneInt, [](void*) {});
            for (int pass = 0; pass < 200; ++pass) {
                JoyeerArray copy {};
                joyeer_array_clone_abi(&copy, array.data, array.count);
                sink = copy.count;
                joyeer_array_destroy_abi(&copy);
            }
            joyeer_array_destroy_abi(&array);
        }));
    }

    {
        const int64_t entries = 1000;
        const int64_t lookups = 200 * scale;
        results.push_back(measure("dictionary set, 1000 distinct Int keys (linear)", entries, [&] {
            JoyeerDictionary dictionary {};
            joyeer_dictionary_create_owned_abi(
                    &dictionary, nullptr, 0, sizeof(int64_t), sizeof(int64_t), sizeof(IntEntry),
                    offsetof(IntEntry, value), JOYEER_DICTIONARY_KEY_INT, nullptr, nullptr, nullptr, nullptr);
            for (int64_t key = 0; key < entries; ++key) {
                int64_t mutableKey = key;
                joyeer_dictionary_set_owned_abi(&dictionary, &mutableKey, &key);
            }
            sink = dictionary.count;
            joyeer_dictionary_destroy_abi(&dictionary);
        }));

        JoyeerDictionary dictionary {};
        joyeer_dictionary_create_owned_abi(
                &dictionary, nullptr, 0, sizeof(int64_t), sizeof(int64_t), sizeof(IntEntry),
                offsetof(IntEntry, value), JOYEER_DICTIONARY_KEY_INT, nullptr, nullptr, nullptr, nullptr);
        for (int64_t key = 0; key < entries; ++key) {
            int64_t mutableKey = key;
            joyeer_dictionary_set_owned_abi(&dictionary, &mutableKey, &key);
        }
        results.push_back(measure("dictionary find, last of 1000 Int keys", lookups, [&] {
            const int64_t key = entries - 1;
            int64_t total = 0;
            for (int64_t index = 0; index < lookups; ++index) {
                total += *static_cast<const int64_t*>(joyeer_dictionary_find_abi(
                        dictionary.data, dictionary.count, &key, sizeof(key), JOYEER_DICTIONARY_KEY_INT));
            }
            sink = total;
        }));
        joyeer_dictionary_destroy_abi(&dictionary);
    }

    std::printf("%-58s %14s\n", "benchmark", "ns per op");
    for (const auto& result : results) {
        std::printf("%-58s %14.2f\n", result.name.c_str(), result.nanosecondsPerOperation);
    }
    if (joyeer_runtime_active_allocations() != 0) {
        std::fprintf(stderr, "leaked %lld allocation(s)\n",
                static_cast<long long>(joyeer_runtime_active_allocations()));
        return 1;
    }
    return 0;
}
