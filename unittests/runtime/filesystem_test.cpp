#include "joyeer/native/filesystem.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace {

struct Bytes {
    std::string owned;
    const uint8_t* data;
    int64_t count;

    explicit Bytes(const std::string& text)
        : owned(text),
          data(reinterpret_cast<const uint8_t*>(owned.data())),
          count(static_cast<int64_t>(owned.size())) {}
    Bytes(const Bytes&) = delete;
    Bytes(Bytes&&) = delete;
};

Bytes bytes(const std::string& text) {
    return Bytes(text);
}

std::string utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return { reinterpret_cast<const char*>(value.data()), value.size() };
}

std::string text(const JoyeerString& value) {
    if (value.count == 0) return {};
    return { reinterpret_cast<const char*>(value.data),
             static_cast<size_t>(value.count) };
}

class FileSystemTest : public testing::Test {
protected:
    void SetUp() override {
        static std::atomic<unsigned> serial { 0 };
        root_ = std::filesystem::temp_directory_path() /
                ("joyeer-fs-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 "-" + std::to_string(serial++));
        ASSERT_TRUE(std::filesystem::create_directory(root_));
    }

    void TearDown() override {
        for (auto it = paths_.rbegin(); it != paths_.rend(); ++it) {
            std::error_code error;
            std::filesystem::remove(*it, error);
            EXPECT_FALSE(error) << it->string() << ": " << error.message();
        }
        std::error_code error;
        std::filesystem::remove(root_, error);
        EXPECT_FALSE(error) << error.message();
        EXPECT_EQ(joyeer_runtime_active_allocations(), 0);
    }

    std::filesystem::path track(const std::filesystem::path& path) {
        paths_.push_back(path);
        return path;
    }

    std::filesystem::path root_;
    std::vector<std::filesystem::path> paths_;
};

TEST_F(FileSystemTest, WritesReadsBinaryAndPreservesExistingFiles) {
    const auto file = track(root_ / std::filesystem::path(u8"space ☃.bin"));
    const auto path = bytes(utf8(file));
    const std::string contents("one\0two\xff", 8);
    const auto input = bytes(contents);
    int64_t code = -1;
    EXPECT_EQ(joyeer_fs_write_file_new_abi(
            &code, path.data, path.count, input.data, input.count), JOYEER_FS_ERROR_NONE);
    EXPECT_EQ(code, 0);
    const auto replacement = bytes(std::string("replacement"));
    EXPECT_EQ(joyeer_fs_write_file_new_abi(&code, path.data, path.count,
            replacement.data, replacement.count), JOYEER_FS_ERROR_ALREADY_EXISTS);
#ifdef _WIN32
    EXPECT_EQ(code, ERROR_FILE_EXISTS);
#else
    EXPECT_EQ(code, EEXIST);
#endif

    JoyeerString output {};
    EXPECT_EQ(joyeer_fs_read_file_abi(&output, &code, path.data, path.count),
            JOYEER_FS_ERROR_NONE);
    EXPECT_EQ(code, 0);
    EXPECT_EQ(text(output), contents);
    joyeer_string_destroy_abi(&output);
    const auto directory = bytes(utf8(root_));
    JoyeerArray names {};
    EXPECT_EQ(joyeer_fs_list_directory_abi(
            &names, &code, directory.data, directory.count), JOYEER_FS_ERROR_NONE);
    ASSERT_EQ(names.count, 1);
    EXPECT_EQ(text(static_cast<const JoyeerString*>(names.data)[0]),
            utf8(file.filename()));
    joyeer_array_destroy_abi(&names);
}

TEST_F(FileSystemTest, ReadsPrefixesAtZeroPartialExactAndPastEofLimits) {
    const auto file = track(root_ / std::filesystem::path(u8"prefix ☃ 😀.bin"));
    const auto path = bytes(utf8(file));
    const std::string contents("one\0two\xff", 8);
    const auto input = bytes(contents);
    int64_t code = -1;
    ASSERT_EQ(joyeer_fs_write_file_new_abi(
            &code, path.data, path.count, input.data, input.count), JOYEER_FS_ERROR_NONE);
    const auto baseline = joyeer_runtime_active_allocations();
    for (const auto maximum : std::array<int64_t, 7> {
            0, 1, 4, 8, 9, 65537, (std::numeric_limits<int64_t>::max)(),
         }) {
        SCOPED_TRACE(maximum);
        JoyeerString output {};
        code = -1;
        EXPECT_EQ(joyeer_fs_read_file_prefix_abi(
                &output, &code, path.data, path.count, maximum), JOYEER_FS_ERROR_NONE);
        EXPECT_EQ(code, 0);
            EXPECT_EQ(output.count, (std::min)(maximum, input.count));
        EXPECT_EQ(text(output), contents.substr(0, static_cast<size_t>(maximum)));
        EXPECT_EQ(joyeer_runtime_active_allocations(), baseline + 1);
        joyeer_string_destroy_abi(&output);
        EXPECT_EQ(output.data, nullptr);
        EXPECT_EQ(output.count, 0);
        EXPECT_EQ(joyeer_runtime_active_allocations(), baseline);
    }

    const auto emptyFile = track(root_ / "empty");
    const auto emptyPath = bytes(utf8(emptyFile));
    ASSERT_EQ(joyeer_fs_write_file_new_abi(
            &code, emptyPath.data, emptyPath.count, nullptr, 0), JOYEER_FS_ERROR_NONE);
    JoyeerString empty {};
    EXPECT_EQ(joyeer_fs_read_file_prefix_abi(
            &empty, &code, emptyPath.data, emptyPath.count, 65537), JOYEER_FS_ERROR_NONE);
    EXPECT_EQ(code, 0);
    EXPECT_EQ(empty.count, 0);
    EXPECT_EQ(joyeer_runtime_active_allocations(), baseline + 1);
    joyeer_string_destroy_abi(&empty);
    EXPECT_EQ(joyeer_runtime_active_allocations(), baseline);
}

TEST_F(FileSystemTest, PreservesBinaryPrefixesEndingInsideUtf8) {
    const auto file = track(root_ / "utf8-cut.bin");
    const auto path = bytes(utf8(file));
    const std::string contents("a\0\xff\xe2\x98\x83z", 7);
    const auto input = bytes(contents);
    int64_t code = -1;
    ASSERT_EQ(joyeer_fs_write_file_new_abi(
            &code, path.data, path.count, input.data, input.count), JOYEER_FS_ERROR_NONE);
    const auto baseline = joyeer_runtime_active_allocations();
    for (const auto maximum : std::array<int64_t, 3> { 3, 4, 5 }) {
        SCOPED_TRACE(maximum);
        JoyeerString output {};
        EXPECT_EQ(joyeer_fs_read_file_prefix_abi(
                &output, &code, path.data, path.count, maximum), JOYEER_FS_ERROR_NONE);
        EXPECT_EQ(code, 0);
        EXPECT_EQ(output.count, maximum);
        EXPECT_EQ(text(output), contents.substr(0, static_cast<size_t>(maximum)));
        EXPECT_EQ(joyeer_runtime_active_allocations(), baseline + 1);
        joyeer_string_destroy_abi(&output);
        EXPECT_EQ(joyeer_runtime_active_allocations(), baseline);
    }
}

TEST_F(FileSystemTest, CapsGrowthAndReturnsTheManifestSentinelByte) {
    const auto file = track(root_ / "large.bin");
    const auto path = bytes(utf8(file));
    std::string contents(131072, 'x');
    contents[65536] = '\xff';
    const auto input = bytes(contents);
    int64_t code = -1;
    ASSERT_EQ(joyeer_fs_write_file_new_abi(
            &code, path.data, path.count, input.data, input.count), JOYEER_FS_ERROR_NONE);
    const auto baseline = joyeer_runtime_active_allocations();
    for (const auto maximum : std::array<int64_t, 4> { 4095, 4096, 4097, 65537 }) {
        SCOPED_TRACE(maximum);
        JoyeerString output {};
        EXPECT_EQ(joyeer_fs_read_file_prefix_abi(
                &output, &code, path.data, path.count, maximum), JOYEER_FS_ERROR_NONE);
        EXPECT_EQ(code, 0);
        EXPECT_EQ(output.count, maximum);
        EXPECT_EQ(text(output), contents.substr(0, static_cast<size_t>(maximum)));
        EXPECT_EQ(joyeer_runtime_active_allocations(), baseline + 1);
        joyeer_string_destroy_abi(&output);
        EXPECT_EQ(joyeer_runtime_active_allocations(), baseline);
    }
    JoyeerString whole {};
    EXPECT_EQ(joyeer_fs_read_file_abi(&whole, &code, path.data, path.count),
            JOYEER_FS_ERROR_NONE);
    EXPECT_EQ(code, 0);
    EXPECT_EQ(whole.count, input.count);
    EXPECT_EQ(text(whole), contents);
    joyeer_string_destroy_abi(&whole);
    EXPECT_EQ(joyeer_runtime_active_allocations(), baseline);

        std::filesystem::resize_file(file, 65536);
        JoyeerString exactManifest {};
        EXPECT_EQ(joyeer_fs_read_file_prefix_abi(
            &exactManifest, &code, path.data, path.count, 65537), JOYEER_FS_ERROR_NONE);
        EXPECT_EQ(code, 0);
        EXPECT_EQ(exactManifest.count, 65536);
        EXPECT_EQ(text(exactManifest), contents.substr(0, 65536));
        joyeer_string_destroy_abi(&exactManifest);
        EXPECT_EQ(joyeer_runtime_active_allocations(), baseline);
}

TEST_F(FileSystemTest, RejectsNegativePrefixLimitsAsOtherWithoutAllocations) {
    const auto missing = bytes(utf8(root_ / "absent"));
    const auto baseline = joyeer_runtime_active_allocations();
    for (const auto maximum : std::array<int64_t, 2> {
            -1, (std::numeric_limits<int64_t>::min)(),
         }) {
        SCOPED_TRACE(maximum);
        JoyeerString output {};
        int64_t code = 0;
        EXPECT_EQ(joyeer_fs_read_file_prefix_abi(
                &output, &code, missing.data, missing.count, maximum),
                JOYEER_FS_ERROR_OTHER);
#ifdef _WIN32
        EXPECT_EQ(code, ERROR_INVALID_PARAMETER);
#else
        EXPECT_EQ(code, EINVAL);
#endif
        EXPECT_EQ(output.data, nullptr);
        EXPECT_EQ(output.count, 0);
        EXPECT_EQ(joyeer_runtime_active_allocations(), baseline);
        joyeer_string_destroy_abi(&output);
        EXPECT_EQ(joyeer_runtime_active_allocations(), baseline);
    }
}

TEST_F(FileSystemTest, ZeroPrefixStillValidatesAndOpensItsPath) {
    const auto missing = bytes(utf8(root_ / "absent"));
    JoyeerString output {};
    int64_t code = 0;
    const auto baseline = joyeer_runtime_active_allocations();
    EXPECT_EQ(joyeer_fs_read_file_prefix_abi(
            &output, &code, missing.data, missing.count, 0), JOYEER_FS_ERROR_NOT_FOUND);
#ifdef _WIN32
    EXPECT_EQ(code, ERROR_FILE_NOT_FOUND);
#else
    EXPECT_EQ(code, ENOENT);
#endif
    const auto nul = bytes(std::string("bad\0path", 8));
    EXPECT_EQ(joyeer_fs_read_file_prefix_abi(
            &output, &code, nul.data, nul.count, 0), JOYEER_FS_ERROR_INVALID_PATH);
#ifdef _WIN32
    EXPECT_EQ(code, ERROR_INVALID_PARAMETER);
#else
    EXPECT_EQ(code, EINVAL);
#endif
    const auto malformed = bytes(std::string("\xc0\xaf", 2));
    EXPECT_EQ(joyeer_fs_read_file_prefix_abi(
            &output, &code, malformed.data, malformed.count, 0), JOYEER_FS_ERROR_INVALID_PATH);
#ifdef _WIN32
    EXPECT_EQ(code, ERROR_NO_UNICODE_TRANSLATION);
#else
    EXPECT_EQ(code, EILSEQ);
#endif
    EXPECT_EQ(joyeer_fs_read_file_prefix_abi(
            &output, &code, nullptr, 0, 0), JOYEER_FS_ERROR_INVALID_PATH);
#ifdef _WIN32
    EXPECT_EQ(code, ERROR_INVALID_PARAMETER);
#else
    EXPECT_EQ(code, EINVAL);
#endif
    EXPECT_EQ(output.data, nullptr);
    EXPECT_EQ(output.count, 0);
    EXPECT_EQ(joyeer_runtime_active_allocations(), baseline);
}

TEST_F(FileSystemTest, PrefixFailureDoesNotPublishOrLeakPartialStorage) {
    const auto directory = bytes(utf8(root_));
    JoyeerString output {};
    int64_t code = 0;
    const auto baseline = joyeer_runtime_active_allocations();
    EXPECT_EQ(joyeer_fs_read_file_prefix_abi(
            &output, &code, directory.data, directory.count, 65537),
            JOYEER_FS_ERROR_IS_DIRECTORY);
#ifdef _WIN32
    EXPECT_EQ(code, ERROR_ACCESS_DENIED);
#else
    EXPECT_EQ(code, EISDIR);
#endif
    EXPECT_EQ(output.data, nullptr);
    EXPECT_EQ(output.count, 0);
    EXPECT_EQ(joyeer_runtime_active_allocations(), baseline);
}

TEST_F(FileSystemTest, ValidatesPathsAndReportsPlatformErrors) {
    const auto missing = bytes(utf8(root_ / "absent"));
    int64_t code = 0;
    JoyeerString output {};
    EXPECT_EQ(joyeer_fs_read_file_abi(&output, &code, missing.data, missing.count),
            JOYEER_FS_ERROR_NOT_FOUND);
#ifdef _WIN32
    EXPECT_EQ(code, ERROR_FILE_NOT_FOUND);
#else
    EXPECT_EQ(code, ENOENT);
#endif
    EXPECT_EQ(output.data, nullptr);

    const std::string invalid("bad\0path", 8);
    const auto nul = bytes(invalid);
    int32_t kind = -1;
    EXPECT_EQ(joyeer_fs_file_kind_abi(
            &kind, &code, nul.data, nul.count),
            JOYEER_FS_ERROR_INVALID_PATH);
#ifdef _WIN32
    EXPECT_EQ(code, ERROR_INVALID_PARAMETER);
#else
    EXPECT_EQ(code, EINVAL);
#endif
    const std::string malformed("\xc0\xaf", 2);
    const auto bad = bytes(malformed);
    EXPECT_EQ(joyeer_fs_file_kind_abi(&kind, &code, bad.data, bad.count),
            JOYEER_FS_ERROR_INVALID_PATH);
#ifdef _WIN32
    EXPECT_EQ(code, ERROR_NO_UNICODE_TRANSLATION);
#else
    EXPECT_EQ(code, EILSEQ);
#endif
    EXPECT_EQ(joyeer_fs_create_directory_abi(&code, nullptr, 0),
            JOYEER_FS_ERROR_INVALID_PATH);
#ifdef _WIN32
    EXPECT_EQ(code, ERROR_INVALID_PARAMETER);
#else
    EXPECT_EQ(code, EINVAL);
#endif
}

TEST_F(FileSystemTest, CreatesListsKindsAndRemovesOneLevelAtATime) {
    const auto directory = track(root_ / "child");
    const auto folder = bytes(utf8(directory));
    int64_t code = 0;
    EXPECT_EQ(joyeer_fs_create_directory_abi(&code, folder.data, folder.count),
            JOYEER_FS_ERROR_NONE);
    EXPECT_EQ(joyeer_fs_create_directory_abi(&code, folder.data, folder.count),
            JOYEER_FS_ERROR_ALREADY_EXISTS);
#ifdef _WIN32
    EXPECT_EQ(code, ERROR_ALREADY_EXISTS);
#else
    EXPECT_EQ(code, EEXIST);
#endif
    JoyeerArray empty {};
    EXPECT_EQ(joyeer_fs_list_directory_abi(&empty, &code, folder.data, folder.count),
            JOYEER_FS_ERROR_NONE);
    EXPECT_EQ(empty.count, 0);
    joyeer_array_destroy_abi(&empty);
    const auto missingParent = bytes(utf8(root_ / "missing" / "child"));
    EXPECT_EQ(joyeer_fs_create_directory_abi(
            &code, missingParent.data, missingParent.count), JOYEER_FS_ERROR_NOT_FOUND);

    const auto file = track(directory / "item.txt");
    const auto pathname = bytes(utf8(file));
    EXPECT_EQ(joyeer_fs_write_file_new_abi(&code, pathname.data, pathname.count,
            nullptr, 0), JOYEER_FS_ERROR_NONE);
    JoyeerString emptyFile {};
    EXPECT_EQ(joyeer_fs_read_file_abi(
            &emptyFile, &code, pathname.data, pathname.count), JOYEER_FS_ERROR_NONE);
    EXPECT_EQ(emptyFile.count, 0);
    joyeer_string_destroy_abi(&emptyFile);
    int32_t kind = -1;
    EXPECT_EQ(joyeer_fs_file_kind_abi(&kind, &code, folder.data, folder.count),
            JOYEER_FS_ERROR_NONE);
    EXPECT_EQ(kind, JOYEER_FS_KIND_DIRECTORY);
    EXPECT_EQ(joyeer_fs_file_kind_abi(&kind, &code, pathname.data, pathname.count),
            JOYEER_FS_ERROR_NONE);
    EXPECT_EQ(kind, JOYEER_FS_KIND_FILE);

    JoyeerArray names {};
    EXPECT_EQ(joyeer_fs_list_directory_abi(&names, &code, folder.data, folder.count),
            JOYEER_FS_ERROR_NONE);
    ASSERT_EQ(names.count, 1);
    EXPECT_EQ(text(static_cast<const JoyeerString*>(names.data)[0]), "item.txt");
    joyeer_array_destroy_abi(&names);
    EXPECT_EQ(joyeer_fs_list_directory_abi(&names, &code, pathname.data, pathname.count),
            JOYEER_FS_ERROR_NOT_DIRECTORY);
    EXPECT_EQ(names.data, nullptr);
    EXPECT_EQ(joyeer_fs_remove_file_abi(&code, folder.data, folder.count),
            JOYEER_FS_ERROR_IS_DIRECTORY);
    EXPECT_EQ(joyeer_fs_remove_directory_abi(&code, pathname.data, pathname.count),
            JOYEER_FS_ERROR_NOT_DIRECTORY);
    EXPECT_NE(joyeer_fs_remove_directory_abi(&code, folder.data, folder.count),
            JOYEER_FS_ERROR_NONE);
    EXPECT_EQ(joyeer_fs_remove_file_abi(&code, pathname.data, pathname.count),
            JOYEER_FS_ERROR_NONE);
    EXPECT_EQ(joyeer_fs_remove_directory_abi(&code, folder.data, folder.count),
            JOYEER_FS_ERROR_NONE);
}

TEST_F(FileSystemTest, JoinsLexicallyAndRejectsInvalidText) {
    int64_t code = -1;
    JoyeerString joined {};
    const auto base = bytes(std::string("one/.."));
    const auto leaf = bytes(std::string("two"));
    EXPECT_EQ(joyeer_fs_join_path_abi(&joined, &code, base.data, base.count,
            leaf.data, leaf.count), JOYEER_FS_ERROR_NONE);
#ifdef _WIN32
    EXPECT_EQ(text(joined), "one/..\\two");
#else
    EXPECT_EQ(text(joined), "one/../two");
#endif
    joyeer_string_destroy_abi(&joined);
    const auto absolute = bytes(utf8(root_));
    EXPECT_EQ(joyeer_fs_join_path_abi(&joined, &code, base.data, base.count,
            absolute.data, absolute.count), JOYEER_FS_ERROR_NONE);
    EXPECT_EQ(text(joined), utf8(root_));
    joyeer_string_destroy_abi(&joined);
    const std::string badPath("a\0b", 3);
    const auto invalid = bytes(badPath);
    EXPECT_EQ(joyeer_fs_join_path_abi(&joined, &code, base.data, base.count,
            invalid.data, invalid.count), JOYEER_FS_ERROR_INVALID_PATH);
    EXPECT_NE(code, 0);
    EXPECT_EQ(joyeer_fs_join_path_abi(&joined, &code, base.data, base.count,
            nullptr, 0), JOYEER_FS_ERROR_INVALID_PATH);
    EXPECT_NE(code, 0);
    EXPECT_EQ(joyeer_fs_join_path_abi(&joined, &code, nullptr, 0,
            leaf.data, leaf.count), JOYEER_FS_ERROR_INVALID_PATH);
    EXPECT_NE(code, 0);
#ifdef _WIN32
    const auto driveRelative = bytes(std::string("C:child"));
    EXPECT_EQ(joyeer_fs_join_path_abi(&joined, &code, base.data, base.count,
            driveRelative.data, driveRelative.count), JOYEER_FS_ERROR_NONE);
    EXPECT_EQ(text(joined), "C:child");
    joyeer_string_destroy_abi(&joined);
#endif
}

#ifdef _WIN32
TEST_F(FileSystemTest, PreservesBareDriveCurrentDirectory) {
    const auto drive = bytes(utf8(root_.root_name()));
    if (drive.count != 2 || drive.owned[1] != ':') {
        GTEST_SKIP() << "temporary directory is not on a drive-letter volume";
    }
    struct RestoreDirectory {
        std::filesystem::path previous;
        ~RestoreDirectory() {
            std::error_code error;
            std::filesystem::current_path(previous, error);
            EXPECT_FALSE(error) << error.message();
        }
    } restore { std::filesystem::current_path() };
    std::filesystem::current_path(root_);
    {
        std::ofstream marker(track(root_ / "drive-marker"));
        ASSERT_TRUE(marker.good());
        marker << "marker";
    }

    int64_t code = -1;
    JoyeerString joined {};
    const auto leaf = bytes(std::string("child"));
    EXPECT_EQ(joyeer_fs_join_path_abi(
            &joined, &code, drive.data, drive.count, leaf.data, leaf.count),
            JOYEER_FS_ERROR_NONE);
    EXPECT_EQ(code, 0);
    EXPECT_EQ(text(joined), drive.owned + "child");
    joyeer_string_destroy_abi(&joined);

    JoyeerArray names {};
    EXPECT_EQ(joyeer_fs_list_directory_abi(
            &names, &code, drive.data, drive.count), JOYEER_FS_ERROR_NONE);
    EXPECT_EQ(code, 0);
    EXPECT_EQ(names.count, 1);
    if (names.count == 1) {
        EXPECT_EQ(text(static_cast<const JoyeerString*>(names.data)[0]), "drive-marker");
    }
    joyeer_array_destroy_abi(&names);
}
#endif

TEST_F(FileSystemTest, RemovesLinkWithoutTouchingItsTarget) {
    const auto target = track(root_ / "target.txt");
    const auto link = track(root_ / "link.txt");
    {
        std::ofstream output(target);
        ASSERT_TRUE(output.is_open());
        output << "still here";
    }
    std::error_code error;
    std::filesystem::create_symlink(target, link, error);
    if (error) GTEST_SKIP() << "symlink creation unavailable: " << error.message();
    const auto path = bytes(utf8(link));
    int64_t code = 0;
    int32_t kind = -1;
    EXPECT_EQ(joyeer_fs_file_kind_abi(&kind, &code, path.data, path.count),
            JOYEER_FS_ERROR_NONE);
    EXPECT_EQ(kind, JOYEER_FS_KIND_SYMLINK);
    EXPECT_EQ(joyeer_fs_remove_directory_abi(&code, path.data, path.count),
            JOYEER_FS_ERROR_NOT_DIRECTORY);
    EXPECT_TRUE(std::filesystem::exists(target));
    JoyeerString followed {};
    EXPECT_EQ(joyeer_fs_read_file_abi(&followed, &code, path.data, path.count),
            JOYEER_FS_ERROR_NONE);
    EXPECT_EQ(text(followed), "still here");
    joyeer_string_destroy_abi(&followed);
        EXPECT_EQ(joyeer_fs_read_file_prefix_abi(
            &followed, &code, path.data, path.count, 5), JOYEER_FS_ERROR_NONE);
        EXPECT_EQ(code, 0);
        EXPECT_EQ(text(followed), "still");
        joyeer_string_destroy_abi(&followed);
    EXPECT_EQ(joyeer_fs_remove_file_abi(&code, path.data, path.count),
            JOYEER_FS_ERROR_NONE);
    EXPECT_TRUE(std::filesystem::exists(target));
}

TEST_F(FileSystemTest, RemovesDirectoryLinkOnlyThroughRemoveFile) {
    const auto target = track(root_ / "target-dir");
    ASSERT_TRUE(std::filesystem::create_directory(target));
    const auto child = track(target / "untouched");
    {
        std::ofstream output(child);
        ASSERT_TRUE(output.is_open());
        output << "safe";
    }
    const auto link = track(root_ / "dir-link");
    std::error_code error;
    std::filesystem::create_directory_symlink(target, link, error);
    if (error) GTEST_SKIP() << "directory symlink unavailable: " << error.message();
    const auto path = bytes(utf8(link));
    int64_t code = 0;
    int32_t kind = -1;
    EXPECT_EQ(joyeer_fs_file_kind_abi(&kind, &code, path.data, path.count),
            JOYEER_FS_ERROR_NONE);
    EXPECT_EQ(kind, JOYEER_FS_KIND_SYMLINK);
    EXPECT_EQ(joyeer_fs_remove_directory_abi(&code, path.data, path.count),
            JOYEER_FS_ERROR_NOT_DIRECTORY);
    EXPECT_TRUE(std::filesystem::exists(child));
    EXPECT_EQ(joyeer_fs_remove_file_abi(&code, path.data, path.count),
            JOYEER_FS_ERROR_NONE);
    EXPECT_TRUE(std::filesystem::exists(child));
}

TEST_F(FileSystemTest, ClassifiesDanglingLinkWithoutFollowingIt) {
    const auto link = track(root_ / "dangling");
    std::error_code error;
    std::filesystem::create_symlink(root_ / "not-created", link, error);
    if (error) GTEST_SKIP() << "symlink creation unavailable: " << error.message();
    const auto path = bytes(utf8(link));
    int64_t code = 0;
    int32_t kind = -1;
    EXPECT_EQ(joyeer_fs_file_kind_abi(&kind, &code, path.data, path.count),
            JOYEER_FS_ERROR_NONE);
    EXPECT_EQ(kind, JOYEER_FS_KIND_SYMLINK);
    EXPECT_EQ(joyeer_fs_remove_file_abi(&code, path.data, path.count),
            JOYEER_FS_ERROR_NONE);
    EXPECT_FALSE(std::filesystem::exists(root_ / "not-created"));
}

#ifndef _WIN32
TEST_F(FileSystemTest, ClassifiesNonRegularEntryAsOther) {
    const auto pipe = track(root_ / "pipe");
    ASSERT_EQ(mkfifo(pipe.c_str(), 0600), 0);
    const auto path = bytes(utf8(pipe));
    int32_t kind = -1;
    int64_t code = -1;
    EXPECT_EQ(joyeer_fs_file_kind_abi(&kind, &code, path.data, path.count),
            JOYEER_FS_ERROR_NONE);
    EXPECT_EQ(kind, JOYEER_FS_KIND_OTHER);
    EXPECT_EQ(code, 0);
}

TEST_F(FileSystemTest, RejectsNonUtf8DirectoryEntriesWithoutLeakingPartialResults) {
    const auto good = track(root_ / "valid");
    const auto bad = track(root_ / std::filesystem::path(std::string("bad\xff", 4)));
    {
        std::ofstream first(good), second(bad);
        ASSERT_TRUE(first.is_open());
        ASSERT_TRUE(second.is_open());
    }
    const auto path = bytes(utf8(root_));
    JoyeerArray names {};
    int64_t code = 0;
    EXPECT_EQ(joyeer_fs_list_directory_abi(&names, &code, path.data, path.count),
            JOYEER_FS_ERROR_INVALID_PATH);
    EXPECT_EQ(code, EILSEQ);
    EXPECT_EQ(names.data, nullptr);
}
#endif

} // namespace
