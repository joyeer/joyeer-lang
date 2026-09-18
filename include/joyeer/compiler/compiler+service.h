#ifndef __joyeer_compiler_compiler_service_h__
#define __joyeer_compiler_compiler_service_h__

#include "joyeer/compiler/options.h"
#include "joyeer/compiler/sourcefile.h"
#include "joyeer/diagnostic/diagnostic.h"

#include <filesystem>
#include <unordered_map>

class CompilerService {
public:
    explicit CompilerService(Diagnostics* diagnostics, joyeer::CompileOptions options);

    void compile(const std::filesystem::path& inputFile);

    [[nodiscard]] const SourceFile::Ptr& getLastCompiledSourceFile() const {
        return lastCompiledSourceFile;
    }

private:
    void compile(const SourceFile::Ptr& sourcefile);

    SourceFile::Ptr findSourceFile(
            const std::filesystem::path& path,
            const std::filesystem::path& relativeFolder = {});

    joyeer::CompileOptions options;
    std::unordered_map<std::filesystem::path, SourceFile::Ptr> sourceFiles;
    SourceFile::Ptr lastCompiledSourceFile;

    Diagnostics* diagnostics;
};
#endif
