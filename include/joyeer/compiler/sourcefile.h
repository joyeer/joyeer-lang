#ifndef __joyeer_compiler_sourcefile_h__
#define __joyeer_compiler_sourcefile_h__

#include "joyeer/runtime/arguments.h"
#include "joyeer/compiler/node.h"

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

    ModuleClass* moduleClass;
    
protected:
    // the path relative to the working directory
    std::string pathInWorkingDirectory;
    
    // file locationInParent
    std::filesystem::path location;
    
    void open(const std::string& path);
};

#endif
