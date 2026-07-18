#ifndef __joyeer_compiler_sourcefile_h__
#define __joyeer_compiler_sourcefile_h__

#include "joyeer/compiler/token.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace joyeer::semantic {
class SemanticModel;
}

namespace joyeer::typing {
class TypeCheckedModel;
}

namespace joyeer::ir {
struct Module;
}

// SourceFile contains all information of source file in disk
class SourceFile {
public:
    using Ptr = std::shared_ptr<SourceFile>;

    enum class LoadError {
        none,
        notRegularFile,
        sourceTooLarge,
        readFailure,
    };
    
public:
        SourceFile(
            const std::filesystem::path& workingDirectory,
            const std::filesystem::path& path);
    explicit SourceFile(std::string sourceContent);
    
    // get .joyeer file's relative locationInParent against the working directory
    [[nodiscard]] std::string getLocation() const {
        return pathInWorkingDirectory;
    }
    
    // get .joyeer file's abstract locationInParent
    [[nodiscard]] std::string getAbstractLocation() const {
        return location.string();
    }

    [[nodiscard]] const std::filesystem::path& getAbstractPath() const {
        return location;
    }
    
    // get .joyeer's parentTypeSlot folder
    [[nodiscard]] std::string getParentFolder() const {
        return location.parent_path().string();
    }

    [[nodiscard]] bool loaded() const {
        return loadError == LoadError::none;
    }

    [[nodiscard]] LoadError loadingError() const {
        return loadError;
    }
    
    // file content
    std::string content;
    
    // lexer parsing result: token list
    std::vector<Token::Ptr> tokens;

    // UTF-8 byte offsets at which source lines begin. Always starts with 0.
    std::vector<uint32_t> lineStarts { 0 };

    // Syntax tree ownership and all name-resolution annotations live in the
    // semantic model.
    std::shared_ptr<joyeer::semantic::SemanticModel> semanticModel;

    // Exact declaration/expression types and type-directed reference
    // completions.
    std::shared_ptr<joyeer::typing::TypeCheckedModel> typeCheckedModel;

    // Backend-neutral high-level IR produced only after successful type
    // checking and structural verification.
    std::shared_ptr<joyeer::ir::Module> joyeerIR;

    // Textual LLVM IR emitted from verified Joyeer IR. Empty when LLVM
    // lowering did not run or failed.
    std::string llvmIR;
    bool llvmHasEntryPoint = false;
    
protected:
    // the path relative to the working directory
    std::string pathInWorkingDirectory;
    
    // file locationInParent
    std::filesystem::path location;

    LoadError loadError = LoadError::none;
    
    void open(const std::filesystem::path& path);
};

#endif
