#ifndef __joyeer_compiler_lexer_lexparser_h__
#define __joyeer_compiler_lexer_lexparser_h__

#include "joyeer/compiler/token.h"
#include "joyeer/compiler/context.h"

class LexParser {
public:
    explicit LexParser(const CompileContext::Ptr& context);

    // parse and tokenize the source file
    void parse(const SourceFile::Ptr& sourceFile);
    
private:
    void parseStringIdentifier();
    void parseStringLiteral();
    void parseOctalLiteral(std::string::const_iterator startAt);
    void parseNumberLiteral(std::string::const_iterator startAt);
    void pushOperator(std::string op, std::string::const_iterator startIterator);
    void parseOperator(std::string::const_iterator startIterator);
    void parsePunctuation(std::string::const_iterator startIterator);
    void parseCppComment();
    void parseCComment();

private:
    std::string::const_iterator iterator;
    std::string::const_iterator endIterator;

    size_t lineNumber = 0;
    std::string::const_iterator lineStartAtPosition;

    // Source files
    SourceFile::Ptr sourcefile;

    Diagnostics* diagnostics;
};


#endif
