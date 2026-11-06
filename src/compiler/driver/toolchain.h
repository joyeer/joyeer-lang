#ifndef __joyeer_driver_toolchain_h__
#define __joyeer_driver_toolchain_h__

#include <filesystem>
#include <string>
#include <vector>

namespace joyeer {

// Directory that contains the running compiler executable, or empty if the
// platform cannot report it.
[[nodiscard]] std::filesystem::path executableDirectory();

// One standard-library module found under <root>/lib/<Name>/project.toml.
struct StandardModule {
    std::string name;
    std::vector<std::filesystem::path> sources;        // absolute
    std::vector<std::string> dependencies;
    bool nativeBindings = false;
    std::vector<std::filesystem::path> nativeLibraries; // absolute
};

// The standard modules that ship with this compiler, described by
// <root>/lib/index.toml. The compiler never searches PATH or the environment:
// the root is the executable's directory unless --sysroot overrides it.
class StandardLibrary {
public:
    enum class Status {
        absent,   // no index.toml: this toolchain has no standard modules
        ready,
        invalid,  // index.toml exists but is malformed or stamps another build
    };

    explicit StandardLibrary(std::filesystem::path libraryDirectory);

    // `problem` describes an invalid index; it is empty otherwise.
    Status load(std::string& problem);
    [[nodiscard]] bool contains(const std::string& name) const;
    // Read one module's project.toml. Returns false and sets `problem` on failure.
    [[nodiscard]] bool resolve(const std::string& name, StandardModule& module, std::string& problem) const;
    [[nodiscard]] const std::filesystem::path& directory() const { return libraryDirectory; }

    // <root>/lib for a root, where an empty root means the executable's directory.
    [[nodiscard]] static std::filesystem::path libraryDirectoryFor(const std::filesystem::path& sysroot);

private:
    std::filesystem::path libraryDirectory;
    std::vector<std::string> moduleNames;
};

// The build stamp the compiler expects in index.toml (empty fields are not checked).
struct ToolchainStamp {
    std::string version;
    std::string target;
    std::string configuration;
};
[[nodiscard]] ToolchainStamp expectedToolchainStamp();

} // namespace joyeer

#endif
