#ifndef JOYEER_COMPILER_HOSTBUILTINS_H
#define JOYEER_COMPILER_HOSTBUILTINS_H

#include <array>
#include <span>
#include <string_view>

namespace joyeer::hostbuiltins {

enum class ValueKind {
    string,
    strings,
    integer,
    unit,
    fileKind,
    processStatus,
};

struct Enumeration {
    std::string_view name;
    std::span<const std::string_view> cases;
    bool integerPayload;
    int firstAbiTag;
};

inline constexpr std::array<std::string_view, 7> filesystemErrors {
    "InvalidPath", "NotFound", "PermissionDenied", "AlreadyExists",
    "NotDirectory", "IsDirectory", "Other",
};
inline constexpr std::array<std::string_view, 4> fileKinds {
    "File", "Directory", "Symlink", "Other",
};
inline constexpr std::array<std::string_view, 2> processStatuses {
    "Exited", "Signaled",
};
inline constexpr std::array<std::string_view, 5> processErrors {
    "InvalidInput", "NotFound", "PermissionDenied", "LaunchFailed", "WaitFailed",
};
inline constexpr std::array<std::string_view, 1> stderrErrors {
    "WriteFailed",
};
inline constexpr std::array<Enumeration, 5> enumerations {{
    { "FileSystemError", filesystemErrors, true, 1 },
    { "FileKind", fileKinds, false, 0 },
    { "ProcessStatus", processStatuses, true, 0 },
    { "ProcessError", processErrors, true, 1 },
    { "StderrError", stderrErrors, true, 1 },
}};

struct Parameter {
    std::string_view name;
    ValueKind kind;
};

inline constexpr std::array<Parameter, 1> pathParameters {{
    { "path", ValueKind::string },
}};
inline constexpr std::array<Parameter, 2> readPrefixParameters {{
    { "path", ValueKind::string }, { "maximumBytes", ValueKind::integer },
}};
inline constexpr std::array<Parameter, 2> writeParameters {{
    { "path", ValueKind::string }, { "contents", ValueKind::string },
}};
inline constexpr std::array<Parameter, 1> stderrParameters {{
    { "contents", ValueKind::string },
}};
inline constexpr std::array<Parameter, 2> joinParameters {{
    { "base", ValueKind::string }, { "path", ValueKind::string },
}};
inline constexpr std::array<Parameter, 3> processParameters {{
    { "executable", ValueKind::string },
    { "arguments", ValueKind::strings },
    { "workingDirectory", ValueKind::string },
}};

struct Function {
    std::string_view name;
    std::string_view runtimeName;
    ValueKind result;
    std::string_view error;
    std::span<const Parameter> parameters;
};

inline constexpr std::array<Function, 11> functions {{
    { "readFileUtf8", "joyeer_fs_read_file_abi", ValueKind::string,
      "FileSystemError", pathParameters },
        { "readFilePrefix", "joyeer_fs_read_file_prefix_abi", ValueKind::string,
            "FileSystemError", readPrefixParameters },
    { "writeFileNew", "joyeer_fs_write_file_new_abi", ValueKind::unit,
      "FileSystemError", writeParameters },
    { "createDirectory", "joyeer_fs_create_directory_abi", ValueKind::unit,
      "FileSystemError", pathParameters },
    { "listDirectory", "joyeer_fs_list_directory_abi", ValueKind::strings,
      "FileSystemError", pathParameters },
    { "fileKind", "joyeer_fs_file_kind_abi", ValueKind::fileKind,
      "FileSystemError", pathParameters },
    { "removeFile", "joyeer_fs_remove_file_abi", ValueKind::unit,
      "FileSystemError", pathParameters },
    { "removeDirectory", "joyeer_fs_remove_directory_abi", ValueKind::unit,
      "FileSystemError", pathParameters },
    { "joinPath", "joyeer_fs_join_path_abi", ValueKind::string,
      "FileSystemError", joinParameters },
    { "runProcess", "joyeer_run_process_abi", ValueKind::processStatus,
      "ProcessError", processParameters },
        { "writeStderr", "joyeer_write_stderr_abi", ValueKind::unit,
            "StderrError", stderrParameters },
}};

inline const Function* findFunction(std::string_view name) {
    for (const auto& function : functions) {
        if (function.name == name) return &function;
    }
    return nullptr;
}

inline const Enumeration* findEnumeration(std::string_view name) {
    for (const auto& enumeration : enumerations) {
        if (enumeration.name == name) return &enumeration;
    }
    return nullptr;
}

}

#endif
