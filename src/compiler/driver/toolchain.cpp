#include "toolchain.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#ifndef JOYEER_TOOLCHAIN_VERSION
#define JOYEER_TOOLCHAIN_VERSION ""
#endif
#ifndef JOYEER_TOOLCHAIN_TARGET
#define JOYEER_TOOLCHAIN_TARGET ""
#endif
#ifndef JOYEER_TOOLCHAIN_CONFIG
#define JOYEER_TOOLCHAIN_CONFIG ""
#endif

namespace joyeer {

std::filesystem::path executableDirectory() {
#if defined(_WIN32)
    std::wstring executablePath(32768, L'\0');
    const auto length = GetModuleFileNameW(
            nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
    if (length > 0 && length < executablePath.size()) {
        executablePath.resize(length);
        return std::filesystem::path(executablePath).parent_path();
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    static_cast<void>(_NSGetExecutablePath(nullptr, &size));
    std::vector<char> executablePath(size);
    if (size != 0 && _NSGetExecutablePath(executablePath.data(), &size) == 0) {
        std::error_code error;
        const auto resolved = std::filesystem::weakly_canonical(executablePath.data(), error);
        return (error ? std::filesystem::path(executablePath.data()) : resolved).parent_path();
    }
#elif defined(__linux__)
    std::vector<char> executablePath(4096);
    while (true) {
        const auto length = readlink("/proc/self/exe", executablePath.data(), executablePath.size());
        if (length < 0) break;
        if (static_cast<size_t>(length) < executablePath.size()) {
            return std::filesystem::path(
                    std::string(executablePath.data(), static_cast<size_t>(length))).parent_path();
        }
        executablePath.resize(executablePath.size() * 2);
    }
#endif
    return {};
}

ToolchainStamp expectedToolchainStamp() {
    return {JOYEER_TOOLCHAIN_VERSION, JOYEER_TOOLCHAIN_TARGET, JOYEER_TOOLCHAIN_CONFIG};
}

namespace {

// A value of the small TOML subset used by index.toml and module manifests:
// strings, booleans, integers, and (possibly multi-line) arrays of strings.
struct TomlValue {
    std::string text;
    std::vector<std::string> items;
    bool isArray = false;
};

using TomlDocument = std::map<std::string, TomlValue>; // "section.key" or "key"

class TomlReader {
public:
    explicit TomlReader(const std::string& text) : text(text) {}

    bool parse(TomlDocument& document, std::string& problem) {
        std::string section;
        while (true) {
            skipBlank();
            if (position >= text.size()) return true;
            if (text[position] == '[') {
                ++position;
                const auto end = text.find(']', position);
                if (end == std::string::npos) return fail(problem, "unterminated table header");
                section = trim(text.substr(position, end - position));
                position = end + 1;
                skipLine();
                continue;
            }
            const auto equals = text.find('=', position);
            const auto newline = text.find('\n', position);
            if (equals == std::string::npos || (newline != std::string::npos && newline < equals)) {
                return fail(problem, "expected key = value");
            }
            const auto key = trim(text.substr(position, equals - position));
            position = equals + 1;
            if (key.empty()) return fail(problem, "empty key");
            TomlValue value;
            if (!parseValue(value, problem)) return false;
            document[section.empty() ? key : section + "." + key] = std::move(value);
            skipLine();
        }
    }

private:
    static std::string trim(const std::string& value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return {};
        return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    }

    bool fail(std::string& problem, const std::string& message) const {
        problem = message;
        return false;
    }

    void skipBlank() {
        while (position < text.size()) {
            const char c = text[position];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++position;
            } else if (c == '#') {
                while (position < text.size() && text[position] != '\n') ++position;
            } else {
                break;
            }
        }
    }

    void skipSpaces() {
        while (position < text.size() && (text[position] == ' ' || text[position] == '\t')) ++position;
    }

    void skipLine() {
        skipSpaces();
        if (position < text.size() && text[position] == '#') {
            while (position < text.size() && text[position] != '\n') ++position;
        }
    }

    bool parseString(std::string& result, std::string& problem) {
        const char quote = text[position++];
        const auto end = text.find(quote, position);
        if (end == std::string::npos) return fail(problem, "unterminated string");
        result = text.substr(position, end - position);
        if (result.find('\n') != std::string::npos) return fail(problem, "string spans lines");
        position = end + 1;
        return true;
    }

    bool parseValue(TomlValue& value, std::string& problem) {
        skipSpaces();
        if (position >= text.size()) return fail(problem, "missing value");
        const char c = text[position];
        if (c == '\'' || c == '"') return parseString(value.text, problem);
        if (c == '[') {
            ++position;
            value.isArray = true;
            while (true) {
                skipBlank();
                if (position >= text.size()) return fail(problem, "unterminated array");
                if (text[position] == ']') {
                    ++position;
                    return true;
                }
                if (text[position] != '\'' && text[position] != '"') {
                    return fail(problem, "arrays may contain only strings");
                }
                std::string item;
                if (!parseString(item, problem)) return false;
                value.items.push_back(std::move(item));
                skipBlank();
                if (position < text.size() && text[position] == ',') ++position;
            }
        }
        const auto start = position;
        while (position < text.size() && text[position] != '\n' && text[position] != '#') ++position;
        value.text = trim(text.substr(start, position - start));
        if (value.text.empty()) return fail(problem, "missing value");
        return true;
    }

    const std::string& text;
    size_t position = 0;
};

bool readDocument(const std::filesystem::path& path, TomlDocument& document, std::string& problem) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        problem = "cannot read '" + path.generic_string() + "'";
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    const auto text = buffer.str();
    std::string message;
    if (!TomlReader(text).parse(document, message)) {
        problem = "'" + path.generic_string() + "': " + message;
        return false;
    }
    return true;
}

// Relative, forward-only path: no absolute paths, no parent traversal.
bool isContainedRelativePath(const std::string& value) {
    if (value.empty()) return false;
    const std::filesystem::path path = std::filesystem::u8path(value);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
    return std::none_of(path.begin(), path.end(), [](const auto& part) { return part == ".."; });
}

} // namespace

StandardLibrary::StandardLibrary(std::filesystem::path libraryDirectory)
        : libraryDirectory(std::move(libraryDirectory)) {}

std::filesystem::path StandardLibrary::libraryDirectoryFor(const std::filesystem::path& sysroot) {
    const auto root = sysroot.empty() ? executableDirectory() : sysroot;
    if (root.empty()) return {};
    return root / "lib";
}

StandardLibrary::Status StandardLibrary::load(std::string& problem) {
    problem.clear();
    moduleNames.clear();
    if (libraryDirectory.empty()) return Status::absent;
    const auto index = libraryDirectory / "index.toml";
    std::error_code error;
    if (!std::filesystem::is_regular_file(index, error)) return Status::absent;

    TomlDocument document;
    if (!readDocument(index, document, problem)) return Status::invalid;
    const auto text = [&](const char* key) {
        const auto found = document.find(key);
        return found == document.end() ? std::string() : found->second.text;
    };
    if (text("schema-version") != "1") {
        problem = "unsupported index.toml schema-version";
        return Status::invalid;
    }
    const auto expected = expectedToolchainStamp();
    const struct { const char* key; const std::string& wanted; } checks[] = {
        {"compiler-version", expected.version},
        {"target", expected.target},
        {"configuration", expected.configuration},
    };
    for (const auto& check : checks) {
        if (!check.wanted.empty() && text(check.key) != check.wanted) {
            problem = std::string("library ") + check.key + " is '" + text(check.key) +
                      "' but this compiler is '" + check.wanted + "'";
            return Status::invalid;
        }
    }
    const auto modules = document.find("modules");
    if (modules == document.end() || !modules->second.isArray) {
        problem = "index.toml must list modules as an array of strings";
        return Status::invalid;
    }
    moduleNames = modules->second.items;
    return Status::ready;
}

bool StandardLibrary::contains(const std::string& name) const {
    return std::find(moduleNames.begin(), moduleNames.end(), name) != moduleNames.end();
}

bool StandardLibrary::resolve(const std::string& name, StandardModule& module, std::string& problem) const {
    module = StandardModule {};
    module.name = name;
    if (!contains(name)) {
        problem = "module '" + name + "' is not part of this toolchain";
        return false;
    }
    const auto directory = libraryDirectory / std::filesystem::u8path(name);
    TomlDocument document;
    if (!readDocument(directory / "project.toml", document, problem)) return false;
    const auto declared = document.find("module.name");
    if (declared == document.end() || declared->second.text != name) {
        problem = "standard module manifest for '" + name + "' declares a different name";
        return false;
    }
    const auto sources = document.find("module.sources");
    if (sources == document.end() || !sources->second.isArray || sources->second.items.empty()) {
        problem = "standard module '" + name + "' has no sources";
        return false;
    }
    for (const auto& source : sources->second.items) {
        if (!isContainedRelativePath(source)) {
            problem = "standard module '" + name + "' has an unsafe source path '" + source + "'";
            return false;
        }
        module.sources.push_back(directory / std::filesystem::u8path(source));
    }
    if (const auto dependencies = document.find("module.dependencies"); dependencies != document.end()) {
        module.dependencies = dependencies->second.items;
    }
    if (const auto bindings = document.find("module.native-bindings"); bindings != document.end()) {
        module.nativeBindings = bindings->second.text == "true";
    }
    if (const auto libraries = document.find("module.native-libraries"); libraries != document.end()) {
        for (const auto& library : libraries->second.items) {
            if (!isContainedRelativePath(library)) {
                problem = "standard module '" + name + "' has an unsafe native library path '" + library + "'";
                return false;
            }
            module.nativeLibraries.push_back(directory / std::filesystem::u8path(library));
        }
    }
    return true;
}

} // namespace joyeer
