#include "joyeer/compiler/sourcefile.h"
#include <iostream>
#include <sstream>
#include <fstream>
#include <limits>
#include <utility>

namespace {

std::string pathUtf8(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return std::string(
            reinterpret_cast<const char*>(encoded.data()),
            encoded.size());
}

} // namespace

SourceFile::SourceFile(
        const std::filesystem::path& workingDirectory,
        const std::filesystem::path& path) {
    std::error_code error;
    auto modulePath = std::filesystem::relative(path, workingDirectory, error);
    if (error) modulePath = path.filename();
    pathInWorkingDirectory = pathUtf8(modulePath);
    location = path;
    open(path);
}

SourceFile::SourceFile(std::string sourceContent):
        content(std::move(sourceContent)),
        pathInWorkingDirectory("<memory>"),
        location("<memory>") {
}

void SourceFile::open(const std::filesystem::path& path) {
    content.clear();
    loadError = LoadError::none;
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        loadError = LoadError::notRegularFile;
        return;
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        loadError = LoadError::readFailure;
        return;
    }
    if (size > std::numeric_limits<uint32_t>::max()) {
        loadError = LoadError::sourceTooLarge;
        return;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        loadError = LoadError::readFailure;
        return;
    }
    content.resize(static_cast<size_t>(size));
    if (size != 0) {
        input.read(content.data(), static_cast<std::streamsize>(size));
        if (input.gcount() != static_cast<std::streamsize>(size)) {
            content.clear();
            loadError = LoadError::readFailure;
            return;
        }
    }
    char extra = 0;
    if (input.get(extra)) {
        content.clear();
        loadError = LoadError::readFailure;
    }
}
