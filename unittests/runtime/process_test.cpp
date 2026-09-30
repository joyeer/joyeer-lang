#include "joyeer/native/process.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#endif

#ifndef JOYEER_PROCESS_TEST_CHILD_PATH
#error JOYEER_PROCESS_TEST_CHILD_PATH must name the compiled child executable
#endif

namespace {

JoyeerString asString(const std::string& value) {
    return { reinterpret_cast<const uint8_t*>(value.data()),
             static_cast<int64_t>(value.size()) };
}

struct Outcome {
    int32_t error;
    int32_t kind;
    int64_t status;
    int64_t nativeError;
};

Outcome run(const std::string& executable, const std::vector<std::string>& arguments,
            const std::string& directory) {
    std::vector<JoyeerString> borrowed;
    borrowed.reserve(arguments.size());
    for (const auto& arg : arguments) borrowed.push_back(asString(arg));
    Outcome result { -1, -1, -1, -1 };
    result.error = joyeer_run_process_abi(
            &result.kind, &result.status, &result.nativeError,
            asString(executable).data, static_cast<int64_t>(executable.size()),
            borrowed.data(), static_cast<int64_t>(borrowed.size()),
            asString(directory).data, static_cast<int64_t>(directory.size()));
    return result;
}

std::string utf8Path(const std::filesystem::path& path) {
    auto bytes = path.u8string();
    return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
}

const std::filesystem::path childPath = JOYEER_PROCESS_TEST_CHILD_PATH;

std::string streamMask() {
#ifdef _WIN32
    const DWORD streams[] = {
        STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE
    };
    std::string mask;
    for (DWORD which : streams) {
        HANDLE handle = GetStdHandle(which);
        if (!handle || handle == INVALID_HANDLE_VALUE) {
            mask += '0';
            continue;
        }
        SetLastError(NO_ERROR);
        DWORD type = GetFileType(handle);
        mask += (type != FILE_TYPE_UNKNOWN || GetLastError() == NO_ERROR) ? '1' : '0';
    }
    return mask;
#else
    std::string mask;
    for (int fd = 0; fd < 3; ++fd) mask += fcntl(fd, F_GETFD) == -1 ? '0' : '1';
    return mask;
#endif
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("joyeer-process-" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }
    ~TemporaryDirectory() { std::filesystem::remove_all(path); }
    std::filesystem::path path;
};

class TestEnvironment {
public:
    TestEnvironment(const char* key, const std::string& value) : key(key) {
#ifdef _WIN32
        char* current = nullptr;
        size_t size = 0;
        EXPECT_EQ(_dupenv_s(&current, &size, key), 0);
#else
        const char* current = std::getenv(key);
#endif
        if (current) {
            previous = current;
            wasSet = true;
        }
#ifdef _WIN32
        std::free(current);
        EXPECT_EQ(_putenv_s(key, value.c_str()), 0);
#else
        EXPECT_EQ(setenv(key, value.c_str(), 1), 0);
#endif
    }
    ~TestEnvironment() {
#ifdef _WIN32
        EXPECT_EQ(_putenv_s(key, wasSet ? previous.c_str() : ""), 0);
#else
        if (wasSet) {
            EXPECT_EQ(setenv(key, previous.c_str(), 1), 0);
        } else {
            EXPECT_EQ(unsetenv(key), 0);
        }
#endif
    }
private:
    const char* key;
    bool wasSet = false;
    std::string previous;
};

class RedirectedStreams {
public:
    explicit RedirectedStreams(const std::filesystem::path& directory) {
#ifdef _WIN32
        originalOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        originalError = GetStdHandle(STD_ERROR_HANDLE);
        output = CreateFileW((directory / "stdout.txt").c_str(), GENERIC_WRITE,
                             FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, NULL);
        error = CreateFileW((directory / "stderr.txt").c_str(), GENERIC_WRITE,
                            FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
        if (output == INVALID_HANDLE_VALUE || error == INVALID_HANDLE_VALUE) return;
        outputChanged = SetStdHandle(STD_OUTPUT_HANDLE, output) != 0;
        errorChanged = SetStdHandle(STD_ERROR_HANDLE, error) != 0;
        ready = outputChanged && errorChanged;
#else
        originalOutput = dup(STDOUT_FILENO);
        originalError = dup(STDERR_FILENO);
        if (originalOutput < 0 || originalError < 0) return;
        output = open((directory / "stdout.txt").c_str(),
                      O_WRONLY | O_CREAT | O_TRUNC, 0600);
        error = open((directory / "stderr.txt").c_str(),
                     O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (output < 0 || error < 0) return;
        outputChanged = dup2(output, STDOUT_FILENO) >= 0;
        errorChanged = dup2(error, STDERR_FILENO) >= 0;
        ready = outputChanged && errorChanged;
#endif
    }
    ~RedirectedStreams() {
#ifdef _WIN32
        if (outputChanged) SetStdHandle(STD_OUTPUT_HANDLE, originalOutput);
        if (errorChanged) SetStdHandle(STD_ERROR_HANDLE, originalError);
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        if (error != INVALID_HANDLE_VALUE) CloseHandle(error);
#else
        if (outputChanged) dup2(originalOutput, STDOUT_FILENO);
        if (errorChanged) dup2(originalError, STDERR_FILENO);
        if (originalOutput >= 0) close(originalOutput);
        if (originalError >= 0) close(originalError);
        if (output >= 0) close(output);
        if (error >= 0) close(error);
#endif
    }
    bool ready = false;
private:
    bool outputChanged = false;
    bool errorChanged = false;
#ifdef _WIN32
    HANDLE originalOutput = INVALID_HANDLE_VALUE;
    HANDLE originalError = INVALID_HANDLE_VALUE;
    HANDLE output = INVALID_HANDLE_VALUE;
    HANDLE error = INVALID_HANDLE_VALUE;
#else
    int originalOutput = -1;
    int originalError = -1;
    int output = -1;
    int error = -1;
#endif
};

std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

std::vector<std::string> readRecords(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::vector<std::string> records;
    while (true) {
        unsigned char length[4];
        file.read(reinterpret_cast<char*>(length), 4);
        if (file.gcount() == 0) break;
        if (file.gcount() != 4) return {};
        uint32_t size = static_cast<uint32_t>(length[0]) |
                        (static_cast<uint32_t>(length[1]) << 8) |
                        (static_cast<uint32_t>(length[2]) << 16) |
                        (static_cast<uint32_t>(length[3]) << 24);
        std::string record(size, '\0');
        if (!file.read(record.data(), size)) return {};
        records.push_back(std::move(record));
    }
    return records;
}

TEST(ProcessTest, PassesExactArgumentsEnvironmentStreamsAndChildDirectory) {
    TemporaryDirectory temp;
    const auto originalDirectory = std::filesystem::current_path();
    const auto childDirectory = temp.path / std::filesystem::path(u8"child-\u65e5");
    std::filesystem::create_directories(childDirectory);
    const auto output = temp.path / "arguments.bin";
    const std::vector<std::string> payload {
        "", "white space", "hello \xE6\x97\xA5", "a\"b", "\\",
        "trailing\\\\", "slashes\\\\\"quote", "&|<>^%$!;*?()",
    };
    TestEnvironment environment("JOYEER_PROCESS_TEST_ENV", "present");
    std::vector<std::string> args { "record", utf8Path(output) };
    args.insert(args.end(), payload.begin(), payload.end());
    const auto result = run(utf8Path(childPath), args, utf8Path(childDirectory));
    ASSERT_EQ(result.error, JOYEER_PROCESS_ERROR_NONE) << result.nativeError;
    EXPECT_EQ(result.nativeError, 0);
    EXPECT_EQ(result.kind, JOYEER_PROCESS_STATUS_EXITED);
    EXPECT_EQ(result.status, 0);
    EXPECT_EQ(std::filesystem::current_path(), originalDirectory);
    std::vector<std::string> expected { utf8Path(childDirectory), "present",
                                        streamMask() };
    expected.insert(expected.end(), payload.begin(), payload.end());
    EXPECT_EQ(readRecords(output), expected);
}

TEST(ProcessTest, ResolvesRelativeExecutableBeforeChangingChildDirectory) {
    TemporaryDirectory temp;
    const auto cwd = std::filesystem::current_path();
    const auto relativeChild = std::filesystem::relative(childPath, cwd);
    const auto relativeDirectory = std::filesystem::relative(temp.path, cwd);
    const auto output = temp.path / "relative.bin";
    const auto result = run(utf8Path(relativeChild),
                            { "record", utf8Path(output), "relative" },
                            utf8Path(relativeDirectory));
    ASSERT_EQ(result.error, JOYEER_PROCESS_ERROR_NONE) << result.nativeError;
    EXPECT_EQ(result.status, 0);
    EXPECT_EQ(std::filesystem::current_path(), cwd);
    ASSERT_EQ(readRecords(output).size(), 4);
    EXPECT_EQ(readRecords(output)[0], utf8Path(temp.path));
    EXPECT_EQ(readRecords(output)[3], "relative");
}

TEST(ProcessTest, ReportsLaunchFailuresWithoutInterpretingShellOrSearchingPath) {
    TemporaryDirectory temp;
    const auto cwd = utf8Path(temp.path);
    const auto missing = run(utf8Path(temp.path / "absent-executable"),
                             {}, cwd);
    EXPECT_EQ(missing.error, JOYEER_PROCESS_ERROR_NOT_FOUND);
    EXPECT_NE(missing.nativeError, 0);
    const auto badDirectory = run(utf8Path(childPath), {},
                                  utf8Path(temp.path / "absent-directory"));
    EXPECT_EQ(badDirectory.error, JOYEER_PROCESS_ERROR_NOT_FOUND);
    EXPECT_NE(badDirectory.nativeError, 0);
    const auto onPath = temp.path /
        (std::string("joyeer-on-path")
#ifdef _WIN32
         + ".exe"
#endif
        );
    ASSERT_TRUE(std::filesystem::copy_file(childPath, onPath));
#ifndef _WIN32
    std::filesystem::permissions(onPath, std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::add);
#endif
    TestEnvironment pathEnvironment("PATH", cwd);
    const auto absolute = run(utf8Path(onPath), { "exit7" }, cwd);
    ASSERT_EQ(absolute.error, JOYEER_PROCESS_ERROR_NONE) << absolute.nativeError;
    EXPECT_EQ(absolute.status, 7);
    const auto nameOnly = run(utf8Path(onPath.filename()), { "exit7" }, cwd);
    EXPECT_EQ(nameOnly.error, JOYEER_PROCESS_ERROR_NOT_FOUND);
    const auto shell = run("echo hello", {}, cwd);
    EXPECT_EQ(shell.error, JOYEER_PROCESS_ERROR_NOT_FOUND);
#ifdef _WIN32
    const auto batchFile = temp.path / "script.cmd";
    { std::ofstream file(batchFile); file << "@echo off\r\nexit /b 0\r\n"; }
    const auto batch = run(utf8Path(batchFile), {}, cwd);
    EXPECT_EQ(batch.error, JOYEER_PROCESS_ERROR_LAUNCH_FAILED);
    EXPECT_NE(batch.nativeError, 0);
#else
    const auto nonExecutable = temp.path / "no-permission";
    { std::ofstream file(nonExecutable); file << "not an executable"; }
    const auto denied = run(utf8Path(nonExecutable), {}, cwd);
    EXPECT_EQ(denied.error, JOYEER_PROCESS_ERROR_PERMISSION_DENIED);
    EXPECT_NE(denied.nativeError, 0);
#endif
}

TEST(ProcessTest, InheritsWorkingOutputAndErrorStreams) {
    TemporaryDirectory temp;
    {
        RedirectedStreams streams(temp.path);
        ASSERT_TRUE(streams.ready);
        const auto result = run(utf8Path(childPath), { "streams" },
                                utf8Path(temp.path));
        ASSERT_EQ(result.error, JOYEER_PROCESS_ERROR_NONE) << result.nativeError;
        EXPECT_EQ(result.kind, JOYEER_PROCESS_STATUS_EXITED);
        EXPECT_EQ(result.status, 0);
    }
    EXPECT_EQ(readFile(temp.path / "stdout.txt"), "joyeer-child-stdout");
    EXPECT_EQ(readFile(temp.path / "stderr.txt"), "joyeer-child-stderr");
}

TEST(ProcessTest, RejectsMalformedOrEmptyInput) {
    TemporaryDirectory temp;
    const auto cwd = utf8Path(temp.path);
    const auto child = utf8Path(childPath);
    const std::string invalidUtf8("\xC0\xAF", 2);
    for (const auto& executable : { std::string {}, invalidUtf8,
                                    std::string("a\0b", 3) }) {
        auto result = run(executable, {}, cwd);
        EXPECT_EQ(result.error, JOYEER_PROCESS_ERROR_INVALID_INPUT);
        EXPECT_NE(result.nativeError, 0);
    }
    for (const auto& directory : { std::string {}, invalidUtf8,
                                   std::string("a\0b", 3) }) {
        auto result = run(child, {}, directory);
        EXPECT_EQ(result.error, JOYEER_PROCESS_ERROR_INVALID_INPUT);
        EXPECT_NE(result.nativeError, 0);
    }
    for (const auto& arg : { invalidUtf8, std::string("a\0b", 3) }) {
        auto result = run(child, { arg }, cwd);
        EXPECT_EQ(result.error, JOYEER_PROCESS_ERROR_INVALID_INPUT);
    }
    int32_t kind = -1;
    int64_t code = -1;
    int64_t error = -1;
    EXPECT_EQ(joyeer_run_process_abi(&kind, &code, &error,
              asString(child).data, child.size(), nullptr, 1,
              asString(cwd).data, cwd.size()), JOYEER_PROCESS_ERROR_INVALID_INPUT);
    EXPECT_NE(error, 0);
}

TEST(ProcessTest, ReturnsNonzeroCompletionAsStatus) {
    TemporaryDirectory temp;
#ifdef _WIN32
    const auto code = int64_t { 0xF1234567ULL };
    const auto decimal = "4045620583";
#else
    const auto code = int64_t { 73 };
    const auto decimal = "73";
#endif
    const auto result = run(utf8Path(childPath), { "exit", decimal },
                            utf8Path(temp.path));
    EXPECT_EQ(result.error, JOYEER_PROCESS_ERROR_NONE);
    EXPECT_EQ(result.kind, JOYEER_PROCESS_STATUS_EXITED);
    EXPECT_EQ(result.status, code);
    EXPECT_EQ(result.nativeError, 0);
}

#ifndef _WIN32
TEST(ProcessTest, ReportsChildTerminationBySignal) {
    TemporaryDirectory temp;
    const auto result = run(utf8Path(childPath), { "signal", "unused" },
                            utf8Path(temp.path));
    EXPECT_EQ(result.error, JOYEER_PROCESS_ERROR_NONE);
    EXPECT_EQ(result.kind, JOYEER_PROCESS_STATUS_SIGNALED);
    EXPECT_EQ(result.status, SIGTERM);
}
#endif

} // namespace
