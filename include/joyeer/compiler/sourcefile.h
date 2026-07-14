#ifndef __joyeer_compiler_sourcefile_h__
#define __joyeer_compiler_sourcefile_h__

#include "joyeer/runtime/arguments.h"
#include "joyeer/compiler/node.h"

namespace joyeer::semantic {
class SemanticModel;
}

namespace joyeer::typing {
class TypeCheckedModel;
}

// SourceFile contains all information of source file in disk
class SourceFile {
public:
    using Ptr = std::shared_ptr<SourceFile>;
    
public:
    SourceFile(const std::string& workingDirectory, const std::string& path);
    explicit SourceFile(std::string sourceContent);
    
    // get .joyeer file's relative locationInParent against the working directory
    [[nodiscard]] std::string getLocation() const {
        return pathInWorkingDirectory;
    }
    
    // get .joyeer file's abstract locationInParent
    [[nodiscard]] std::string getAbstractLocation() const {
        return location.string();
    }
    
    // get .joyeer's parentTypeSlot folder
    [[nodiscard]] std::string getParentFolder() const {
        return location.parent_path().string();
    }
    
    // file content
    std::string content;
    
    // lexer parsing result: token list
    std::vector<Token::Ptr> tokens;

    // UTF-8 byte offsets at which source lines begin. Always starts with 0.
    std::vector<uint32_t> lineStarts { 0 };

    // v0.1 syntax tree ownership and all name-resolution annotations live in
    // the semantic model. The legacy pipeline does not populate this field.
    std::shared_ptr<joyeer::semantic::SemanticModel> semanticModel;

    // Exact v0.1 declaration/expression types and type-directed reference
    // completions. The legacy TypeGen/VM pipeline does not use this model.
    std::shared_ptr<joyeer::typing::TypeCheckedModel> typeCheckedModel;

    ModuleClass* moduleClass;
    
protected:
    // the path relative to the working directory
    std::string pathInWorkingDirectory;
    
    // file locationInParent
    std::filesystem::path location;
    
    void open(const std::string& path);
};

#endif
