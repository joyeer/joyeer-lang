#ifndef __joyeer_compiler_compiler_service_h__
#define __joyeer_compiler_compiler_service_h__

#include "options.h"
#include "compiler/frontend/sourcefile.h"
#include "compiler/frontend/syntax.h"
#include "compiler/frontend/nameresolution.h"
#include "compiler/diagnostics/diagnostic.h"

#include <filesystem>
#include <unordered_map>

class CompilerService {
public:
    explicit CompilerService(Diagnostics* diagnostics, joyeer::CompileOptions options);

    void compile(const std::filesystem::path& inputFile = {});

    [[nodiscard]] const SourceFile::Ptr& getLastCompiledSourceFile() const {
        return lastCompiledSourceFile;
    }

private:
    joyeer::syntax::SourceFileSyntax::Ptr parse(const SourceFile::Ptr& sourcefile);
    void compileModules();
    void compile(const std::vector<joyeer::semantic::ModuleInput>& modules);
    bool protectInputs(const std::vector<std::filesystem::path>& inputs);

    SourceFile::Ptr findSourceFile(
            const std::filesystem::path& path,
            const std::filesystem::path& relativeFolder = {});

    joyeer::CompileOptions options;
    std::unordered_map<std::filesystem::path, SourceFile::Ptr> sourceFiles;
    SourceFile::Ptr lastCompiledSourceFile;
    std::vector<SourceFile::Ptr> compilationSources;

    Diagnostics* diagnostics;
};
#endif
