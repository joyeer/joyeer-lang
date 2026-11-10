#ifndef JOYEER_COMPILER_HOSTBUILTINS_H
#define JOYEER_COMPILER_HOSTBUILTINS_H

#include <array>
#include <span>
#include <string_view>

// The error and status enumerations that the `System` host operations return.
// They are prelude types: the compiler declares them so that user code and
// `System` agree on one definition, but it knows no host function. Every host
// operation is an ordinary Joyeer function in the `System` standard module.
namespace joyeer::hostbuiltins {

struct Enumeration {
    std::string_view name;
    std::span<const std::string_view> cases;
    // True when every case carries one Int payload (a platform error code or
    // a status value); false when the cases carry nothing.
    bool integerPayload;
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
inline constexpr std::array<std::string_view, 3> stdinErrors {
    "EndOfInput", "LineTooLong", "ReadFailed",
};
inline constexpr std::array<std::string_view, 1> conversionErrors {
    "OutOfRange",
};
inline constexpr std::array<std::string_view, 1> channelErrors {
    "Closed",
};
inline constexpr std::array<Enumeration, 8> enumerations {{
    { "FileSystemError", filesystemErrors, true },
    { "FileKind", fileKinds, false },
    { "ProcessStatus", processStatuses, true },
    { "ProcessError", processErrors, true },
    { "StderrError", stderrErrors, true },
    { "StdinError", stdinErrors, true },
    { "ConversionError", conversionErrors, false },
    { "ChannelClosed", channelErrors, false },
}};

}

#endif
