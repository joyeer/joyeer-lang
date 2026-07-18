#ifndef __joyeer_compiler_lexer_lexparser_h__
#define __joyeer_compiler_lexer_lexparser_h__

#include "joyeer/compiler/sourcefile.h"
#include "joyeer/diagnostic/diagnostic.h"

enum class LexerProfile {
    legacy,
    jsonParserMvp
};

class LexParser {
public:
    explicit LexParser(Diagnostics* diagnostics,
                       LexerProfile profile = LexerProfile::jsonParserMvp);

    // Tokenize the source file. Existing output and cursor state are reset.
    void parse(const SourceFile::Ptr& sourceFile);
    
private:
    [[nodiscard]] bool atEnd() const;
    [[nodiscard]] char peek(size_t lookahead = 0) const;
    char advance();
    bool consumeIf(char expected);

    void skipTrivia();
    void consumeNewline();
    void parseLineComment();
    void parseBlockComment(size_t start, size_t startLine, size_t startColumn);

    void parseIdentifier();
    void parseNumberLiteral();
    void parseStringLiteral();
    void parseByteLiteral();
    void parseOperatorOrPunctuation();

    void emit(TokenKind kind, size_t start, std::string rawValue = {});
    void emitInvalid(size_t start, std::string rawValue = {});
    void report(size_t line, size_t column, const char* error, ...);

    [[nodiscard]] bool isIdentifierHead(char value) const;
    [[nodiscard]] bool isIdentifierTail(char value) const;
    [[nodiscard]] bool isMvpKeyword(TokenKind kind) const;
    [[nodiscard]] bool isMvpOperator(TokenKind kind) const;

private:
    size_t position = 0;
    size_t lineNumber = 0;
    size_t lineStartOffset = 0;
    size_t tokenLine = 0;
    size_t tokenColumn = 0;
    bool nextTokenStartsLine = true;

    // Source files
    SourceFile::Ptr sourcefile;

    Diagnostics* diagnostics;
    LexerProfile profile;
};


#endif
