#ifndef __joyeer_native_filesystem_h__
#define __joyeer_native_filesystem_h__

#include "joyeer/native/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

// Stable Result error tags. On failure, errorCode contains the originating
// errno or Windows error (a native invalid-input code for rejected paths).
enum JoyeerFileSystemErrorKind {
    JOYEER_FS_ERROR_NONE = 0,
    JOYEER_FS_ERROR_INVALID_PATH = 1,
    JOYEER_FS_ERROR_NOT_FOUND = 2,
    JOYEER_FS_ERROR_PERMISSION_DENIED = 3,
    JOYEER_FS_ERROR_ALREADY_EXISTS = 4,
    JOYEER_FS_ERROR_NOT_DIRECTORY = 5,
    JOYEER_FS_ERROR_IS_DIRECTORY = 6,
    JOYEER_FS_ERROR_OTHER = 7,
};

// Stable FileKind payload tags; classification does not follow the leaf link.
enum JoyeerFileKind {
    JOYEER_FS_KIND_FILE = 0,
    JOYEER_FS_KIND_DIRECTORY = 1,
    JOYEER_FS_KIND_SYMLINK = 2,
    JOYEER_FS_KIND_OTHER = 3,
};

// All inputs are flat UTF-8 byte/count pairs, not nul-terminated strings.
// Paths must be nonempty, contain no embedded nul, and decode as strict UTF-8;
// relative paths resolve against the process working directory. Return values
// are JoyeerFileSystemErrorKind, with errorCode set to zero on success.
// Output structs are initialized empty on failure. Successful String and
// Array outputs are runtime-owned and must be destroyed using the matching
// joyeer_*_destroy_abi function; array elements are owned JoyeerStrings.
// Contents are arbitrary bytes, including nul and non-UTF-8 sequences.
// No C structs are passed or returned by value across the compiler ABI.

// Reads the complete file, following ordinary OS path resolution.
int32_t joyeer_fs_read_file_abi(
    JoyeerString* result, int64_t* errorCode,
    const uint8_t* pathData, int64_t pathCount);
// Creates a new file exclusively; never truncates or replaces an existing one.
int32_t joyeer_fs_write_file_new_abi(
    int64_t* errorCode, const uint8_t* pathData, int64_t pathCount,
    const uint8_t* contentsData, int64_t contentsCount);
// Creates one directory only; existing entries and missing parents fail.
int32_t joyeer_fs_create_directory_abi(
    int64_t* errorCode, const uint8_t* pathData, int64_t pathCount);
// Lists owned entry names, excluding "." and "..", in unspecified order.
// Enumeration may follow a directory symlink; non-UTF-8 names cause failure.
int32_t joyeer_fs_list_directory_abi(
    JoyeerArray* result, int64_t* errorCode,
    const uint8_t* pathData, int64_t pathCount);
// Classifies the leaf itself rather than following a symlink.
int32_t joyeer_fs_file_kind_abi(
    int32_t* result, int64_t* errorCode,
    const uint8_t* pathData, int64_t pathCount);
// Removes a file or leaf symlink, never its target; rejects real directories.
int32_t joyeer_fs_remove_file_abi(
    int64_t* errorCode, const uint8_t* pathData, int64_t pathCount);
// Removes only an empty real directory; does not remove a directory symlink.
int32_t joyeer_fs_remove_directory_abi(
    int64_t* errorCode, const uint8_t* pathData, int64_t pathCount);
// Lexical join: both paths must be nonempty. An absolute right-hand path
// replaces the base. On Windows a drive-relative right-hand path (C:foo)
// also replaces the base but remains drive-relative; joining it to an
// unrelated base would change its meaning. No normalization, canonicalization,
// or confinement is implied.
int32_t joyeer_fs_join_path_abi(
    JoyeerString* result, int64_t* errorCode,
    const uint8_t* baseData, int64_t baseCount,
    const uint8_t* pathData, int64_t pathCount);

#ifdef __cplusplus
}
#endif

#endif
