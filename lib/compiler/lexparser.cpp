#include "joyeer/compiler/lexparser.h"
#include "joyeer/diagnostic/diagnostic.h"

#include <charconv>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <utility>

namespace {

bool isDigit(char value) {
    return value >= '0' && value <= '9';
}

bool isAscii(char value) {
    return static_cast<unsigned char>(value) <= 0x7f;
}

const char* lexerDiagnosticCode(const char* error) {
    if (std::strcmp(error, Diagnostics::errorOctalNumberFormat) == 0) {
        return "lexer.invalid-octal-number";
    }
    if (std::strcmp(error, Diagnostics::errorIntegerLiteralOverflow) == 0) {
        return "lexer.integer-literal-overflow";
    }
    if (std::strcmp(error, Diagnostics::errorUnterminatedCComment) == 0) {
        return "lexer.unterminated-comment";
    }
    if (std::strcmp(error, Diagnostics::errorUnterminatedStringLiteral) == 0) {
        return "lexer.unterminated-string";
    }
    if (std::strcmp(error, Diagnostics::errorInvalidStringEscape) == 0) {
        return "lexer.invalid-string-escape";
    }
    if (std::strcmp(error, Diagnostics::errorUnterminatedByteLiteral) == 0) {
        return "lexer.unterminated-byte-literal";
    }
    if (std::strcmp(error, Diagnostics::errorInvalidByteLiteral) == 0) {
        return "lexer.invalid-byte-literal";
    }
    if (std::strcmp(error, Diagnostics::errorInvalidByteEscape) == 0) {
        return "lexer.invalid-byte-escape";
    }
    if (std::strcmp(error, Diagnostics::errorUnsupportedNumericLiteral) == 0) {
        return "lexer.unsupported-numeric-literal";
    }
    if (std::strcmp(error, Diagnostics::errorInvalidNumericSuffix) == 0) {
        return "lexer.invalid-numeric-suffix";
    }
    if (std::strcmp(error, Diagnostics::errorInvalidSourceCharacter) == 0) {
        return "lexer.invalid-source-character";
    }
    if (std::strcmp(error, Diagnostics::errorUnsupportedSyntax) == 0) {
        return "lexer.unsupported-syntax";
    }
    return "lexer.invalid-source";
}

} // namespace

LexParser::LexParser(const CompileContext::Ptr& context, LexerProfile profile):
        diagnostics(context->diagnostics),
        profile(profile) {
}

void LexParser::parse(const SourceFile::Ptr& sourceFile) {
    sourcefile = sourceFile;
    sourcefile->tokens.clear();
    sourcefile->lineStarts.clear();
    sourcefile->lineStarts.push_back(0);

    position = 0;
    lineNumber = 0;
    lineStartOffset = 0;
    tokenLine = 0;
    tokenColumn = 0;
    nextTokenStartsLine = true;

    while (true) {
        skipTrivia();
        if (atEnd()) {
            break;
        }

        const size_t start = position;
        tokenLine = lineNumber;
        tokenColumn = position - lineStartOffset;
        const char value = peek();

        if (value == 'b' && peek(1) == '\'') {
            parseByteLiteral();
        } else if (isIdentifierHead(value)) {
            parseIdentifier();
        } else if (isDigit(value)) {
            parseNumberLiteral();
        } else if (value == '"') {
            parseStringLiteral();
        } else {
            parseOperatorOrPunctuation();
        }

        // Every scanner must consume input. Keep malformed-input recovery from
        // ever turning into an infinite loop.
        if (position == start) {
            advance();
            report(tokenLine, tokenColumn, Diagnostics::errorInvalidSourceCharacter,
                   sourcefile->content.substr(start, 1).c_str());
            emitInvalid(start);
        }
    }

    tokenLine = lineNumber;
    tokenColumn = position - lineStartOffset;
    emit(endOfFile, position);
}

bool LexParser::atEnd() const {
    return position >= sourcefile->content.size();
}

char LexParser::peek(size_t lookahead) const {
    const size_t target = position + lookahead;
    if (target >= sourcefile->content.size()) {
        return '\0';
    }
    return sourcefile->content[target];
}

char LexParser::advance() {
    if (atEnd()) {
        return '\0';
    }
    return sourcefile->content[position++];
}

bool LexParser::consumeIf(char expected) {
    if (peek() != expected) {
        return false;
    }
    ++position;
    return true;
}

void LexParser::skipTrivia() {
    while (!atEnd()) {
        switch (peek()) {
            case ' ':
            case '\t':
                ++position;
                continue;
            case '\n':
            case '\r':
                consumeNewline();
                continue;
            case '/':
                if (peek(1) == '/') {
                    position += 2;
                    parseLineComment();
                    continue;
                }
                if (peek(1) == '*') {
                    const size_t start = position;
                    const size_t startLine = lineNumber;
                    const size_t startColumn = position - lineStartOffset;
                    position += 2;
                    parseBlockComment(start, startLine, startColumn);
                    continue;
                }
                return;
            default:
                return;
        }
    }
}

void LexParser::consumeNewline() {
    if (peek() == '\r') {
        ++position;
        if (peek() == '\n') {
            ++position;
        }
    } else {
        ++position;
    }

    ++lineNumber;
    lineStartOffset = position;
    sourcefile->lineStarts.push_back(static_cast<uint32_t>(position));
    nextTokenStartsLine = true;
}

void LexParser::parseLineComment() {
    while (!atEnd() && peek() != '\n' && peek() != '\r') {
        ++position;
    }
}

void LexParser::parseBlockComment(size_t start, size_t startLine, size_t startColumn) {
    size_t depth = 1;
    while (!atEnd()) {
        if (peek() == '/' && peek(1) == '*') {
            position += 2;
            ++depth;
            continue;
        }
        if (peek() == '*' && peek(1) == '/') {
            position += 2;
            if (--depth == 0) {
                return;
            }
            continue;
        }
        if (peek() == '\n' || peek() == '\r') {
            consumeNewline();
            continue;
        }
        ++position;
    }

    report(startLine, startColumn, Diagnostics::errorUnterminatedCComment);
    (void)start;
}

void LexParser::parseIdentifier() {
    const size_t start = position;
    advance();
    while (isIdentifierTail(peek())) {
        advance();
    }

    const std::string value = sourcefile->content.substr(start, position - start);
    if (value == "_") {
        emit(wildcard, start, value);
        return;
    }
    if (value == Literals::NIL) {
        emit(nilLiteral, start, value);
        return;
    }
    if (value == Literals::TRUE || value == Literals::FALSE) {
        emit(booleanLiteral, start, value);
        return;
    }

    const TokenKind kind = keywordKind(value);
    if (profile == LexerProfile::jsonParserMvp) {
        if (kind != identifier && !isMvpKeyword(kind)) {
            report(tokenLine, tokenColumn, Diagnostics::errorUnsupportedSyntax, value.c_str());
            emit(deferredKeyword, start, value);
            return;
        }
        if (kind == identifier && isDeferredKeyword(value)) {
            report(tokenLine, tokenColumn, Diagnostics::errorUnsupportedSyntax, value.c_str());
            emit(deferredKeyword, start, value);
            return;
        }
    }

    emit(kind, start, value);
}

void LexParser::parseNumberLiteral() {
    const size_t start = position;
    while (isDigit(peek())) {
        advance();
    }

    bool unsupportedNumber = false;
    if ((peek() == '.' && isDigit(peek(1))) || peek() == 'e' || peek() == 'E') {
        unsupportedNumber = true;
        if (peek() == '.') {
            advance();
            while (isDigit(peek())) {
                advance();
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            advance();
            if (peek() == '+' || peek() == '-') {
                advance();
            }
            while (isDigit(peek())) {
                advance();
            }
        }
    } else if (position == start + 1 && sourcefile->content[start] == '0' &&
               (peek() == 'x' || peek() == 'X' || peek() == 'b' || peek() == 'B' ||
                peek() == 'o' || peek() == 'O')) {
        unsupportedNumber = true;
        advance();
        while (isIdentifierTail(peek())) {
            advance();
        }
    } else if (isIdentifierHead(peek())) {
        while (isIdentifierTail(peek())) {
            advance();
        }
        report(tokenLine, tokenColumn, Diagnostics::errorInvalidNumericSuffix);
        emitInvalid(start);
        return;
    }

    if (unsupportedNumber) {
        report(tokenLine, tokenColumn, Diagnostics::errorUnsupportedNumericLiteral);
        emitInvalid(start);
        return;
    }

    const std::string value = sourcefile->content.substr(start, position - start);
    if (profile == LexerProfile::legacy && value.size() > 1 && value.front() == '0' &&
        value.find_first_of("89") != std::string::npos) {
        report(tokenLine, tokenColumn, Diagnostics::errorOctalNumberFormat);
    }

    int64_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed, 10);
    if (result.ec == std::errc::result_out_of_range) {
        report(tokenLine, tokenColumn, Diagnostics::errorIntegerLiteralOverflow);
    }

    emit(decimalLiteral, start, value);
    sourcefile->tokens.back()->intValue = parsed;
}

void LexParser::parseStringLiteral() {
    const size_t start = position;
    advance(); // opening quote

    std::string decoded;
    bool invalidLiteral = false;
    while (!atEnd()) {
        const char value = peek();
        if (value == '"') {
            advance();
            if (invalidLiteral) {
                emitInvalid(start);
            } else {
                emit(stringLiteral, start, decoded);
            }
            return;
        }
        if (value == '\n' || value == '\r') {
            report(tokenLine, tokenColumn, Diagnostics::errorUnterminatedStringLiteral);
            emitInvalid(start);
            return;
        }
        if (value != '\\') {
            decoded.push_back(advance());
            continue;
        }

        advance(); // backslash
        if (atEnd()) {
            report(tokenLine, tokenColumn, Diagnostics::errorUnterminatedStringLiteral);
            emitInvalid(start);
            return;
        }

        const char escaped = advance();
        switch (escaped) {
            case 'n': decoded.push_back('\n'); break;
            case 't': decoded.push_back('\t'); break;
            case 'r': decoded.push_back('\r'); break;
            case '"': decoded.push_back('"'); break;
            case '\\': decoded.push_back('\\'); break;
            case '0': decoded.push_back('\0'); break;
            default:
                report(tokenLine, tokenColumn, Diagnostics::errorInvalidStringEscape);
                invalidLiteral = true;
                break;
        }
    }

    report(tokenLine, tokenColumn, Diagnostics::errorUnterminatedStringLiteral);
    emitInvalid(start);
}

void LexParser::parseByteLiteral() {
    const size_t start = position;
    position += 2; // b'

    bool valid = true;
    uint8_t decoded = 0;
    if (atEnd() || peek() == '\n' || peek() == '\r') {
        report(tokenLine, tokenColumn, Diagnostics::errorUnterminatedByteLiteral);
        emitInvalid(start);
        return;
    }

    if (peek() == '\'') {
        report(tokenLine, tokenColumn, Diagnostics::errorInvalidByteLiteral);
        advance();
        emitInvalid(start);
        return;
    }

    if (peek() == '\\') {
        advance();
        if (atEnd()) {
            report(tokenLine, tokenColumn, Diagnostics::errorUnterminatedByteLiteral);
            emitInvalid(start);
            return;
        }
        const char escaped = advance();
        switch (escaped) {
            case 'n': decoded = '\n'; break;
            case 't': decoded = '\t'; break;
            case 'r': decoded = '\r'; break;
            case '\'': decoded = '\''; break;
            case '"': decoded = '"'; break;
            case '\\': decoded = '\\'; break;
            case '0': decoded = '\0'; break;
            default:
                report(tokenLine, tokenColumn, Diagnostics::errorInvalidByteEscape);
                valid = false;
                break;
        }
    } else {
        const char value = advance();
        if (!isAscii(value)) {
            report(tokenLine, tokenColumn, Diagnostics::errorInvalidByteLiteral);
            valid = false;
        }
        decoded = static_cast<uint8_t>(value);
    }

    if (!consumeIf('\'')) {
        while (!atEnd() && peek() != '\'' && peek() != '\n' && peek() != '\r') {
            advance();
        }
        if (consumeIf('\'')) {
            report(tokenLine, tokenColumn, Diagnostics::errorInvalidByteLiteral);
        } else {
            report(tokenLine, tokenColumn, Diagnostics::errorUnterminatedByteLiteral);
        }
        valid = false;
    }

    if (!valid) {
        emitInvalid(start);
        return;
    }

    emit(byteLiteral, start, std::string(1, static_cast<char>(decoded)));
    sourcefile->tokens.back()->intValue = decoded;
}

void LexParser::parseOperatorOrPunctuation() {
    const size_t start = position;
    const char value = advance();

    auto emitUnsupported = [this, start]() {
        const std::string text = sourcefile->content.substr(start, position - start);
        report(tokenLine, tokenColumn, Diagnostics::errorUnsupportedSyntax, text.c_str());
        emitInvalid(start, text);
    };

    auto emitMvpOperator = [this, start](TokenKind kind) {
        if (profile == LexerProfile::jsonParserMvp && !isMvpOperator(kind)) {
            const std::string text = sourcefile->content.substr(start, position - start);
            report(tokenLine, tokenColumn, Diagnostics::errorUnsupportedSyntax, text.c_str());
            emitInvalid(start, text);
        } else {
            emit(kind, start);
        }
    };

    switch (value) {
        case '{': emit(leftCurly, start); return;
        case '}': emit(rightCurly, start); return;
        case '(': emit(leftParen, start); return;
        case ')': emit(rightParen, start); return;
        case '[': emit(leftSquare, start); return;
        case ']': emit(rightSquare, start); return;
        case ':': emit(colon, start); return;
        case ',': emit(comma, start); return;
        case '.':
            if (profile == LexerProfile::jsonParserMvp && (peek() == '.' || isDigit(peek()))) {
                while (peek() == '.' || isDigit(peek())) {
                    advance();
                }
                report(tokenLine, tokenColumn, Diagnostics::errorUnsupportedSyntax,
                       sourcefile->content.substr(start, position - start).c_str());
                emitInvalid(start);
                return;
            }
            emit(dot, start);
            return;
        case ';':
            if (profile == LexerProfile::jsonParserMvp) {
                report(tokenLine, tokenColumn, Diagnostics::errorUnsupportedSyntax, ";");
                emitInvalid(start);
            } else {
                emit(semicolon, start);
            }
            return;
        case '@':
            if (profile == LexerProfile::jsonParserMvp) {
                report(tokenLine, tokenColumn, Diagnostics::errorUnsupportedSyntax, "@");
                emitInvalid(start);
            } else {
                emit(atSign, start);
            }
            return;
        case '#':
            if (profile == LexerProfile::jsonParserMvp) {
                report(tokenLine, tokenColumn, Diagnostics::errorUnsupportedSyntax, "#");
                emitInvalid(start);
            } else {
                emit(hash, start);
            }
            return;
        case '=':
            if (consumeIf('>')) { emit(fatArrow, start); return; }
            if (consumeIf('=')) { emit(equalEqual, start); return; }
            emit(equal, start);
            return;
        case '!':
            if (consumeIf('=')) { emit(notEqual, start); return; }
            emitMvpOperator(bang);
            return;
        case '<':
            if (profile == LexerProfile::jsonParserMvp && consumeIf('<')) {
                consumeIf('=');
                emitUnsupported();
                return;
            }
            if (consumeIf('=')) { emit(lessEqual, start); return; }
            emit(less, start);
            return;
        case '>':
            if (profile == LexerProfile::jsonParserMvp && consumeIf('>')) {
                consumeIf('=');
                emitUnsupported();
                return;
            }
            if (consumeIf('=')) { emit(greaterEqual, start); return; }
            emit(greater, start);
            return;
        case '&':
            if (consumeIf('&')) {
                if (profile == LexerProfile::jsonParserMvp && consumeIf('=')) {
                    emitUnsupported();
                } else {
                    emit(andAnd, start);
                }
                return;
            }
            if (profile == LexerProfile::jsonParserMvp && consumeIf('=')) {
                emitUnsupported();
                return;
            }
            emit(ampersand, start);
            return;
        case '|':
            if (consumeIf('|')) {
                if (profile == LexerProfile::jsonParserMvp) {
                    consumeIf('=');
                }
                emitMvpOperator(orOr);
            } else {
                if (profile == LexerProfile::jsonParserMvp) {
                    consumeIf('=');
                }
                emitUnsupported();
            }
            return;
        case '?':
            if (profile == LexerProfile::jsonParserMvp && (peek() == '?' || peek() == '.')) {
                advance();
                report(tokenLine, tokenColumn, Diagnostics::errorUnsupportedSyntax,
                       sourcefile->content.substr(start, position - start).c_str());
                emitInvalid(start);
                return;
            }
            emit(question, start);
            return;
        case '+':
            if (profile == LexerProfile::jsonParserMvp && consumeIf('=')) {
                report(tokenLine, tokenColumn, Diagnostics::errorUnsupportedSyntax, "+=");
                emitInvalid(start);
                return;
            }
            emit(plus, start);
            return;
        case '-':
            if (profile == LexerProfile::jsonParserMvp && consumeIf('=')) {
                report(tokenLine, tokenColumn, Diagnostics::errorUnsupportedSyntax, "-=");
                emitInvalid(start);
                return;
            }
            emit(minus, start);
            return;
        case '*':
            if (profile == LexerProfile::jsonParserMvp && consumeIf('=')) {
                report(tokenLine, tokenColumn, Diagnostics::errorUnsupportedSyntax, "*=");
                emitInvalid(start);
                return;
            }
            emit(multiply, start);
            return;
        case '/':
            if (profile == LexerProfile::jsonParserMvp && consumeIf('=')) {
                emitUnsupported();
                return;
            }
            emitMvpOperator(divide);
            return;
        case '%':
            if (profile == LexerProfile::jsonParserMvp && consumeIf('=')) {
                emitUnsupported();
                return;
            }
            emitMvpOperator(percentage);
            return;
        case '^':
        case '~':
            if (profile == LexerProfile::jsonParserMvp) {
                consumeIf('=');
            }
            emitUnsupported();
            return;
        case '\'':
            report(tokenLine, tokenColumn, Diagnostics::errorUnsupportedSyntax, "'");
            while (!atEnd() && peek() != '\'' && peek() != '\n' && peek() != '\r') {
                advance();
            }
            consumeIf('\'');
            emitInvalid(start);
            return;
        default:
            report(tokenLine, tokenColumn, Diagnostics::errorInvalidSourceCharacter,
                   sourcefile->content.substr(start, 1).c_str());
            emitInvalid(start);
            return;
    }
}

void LexParser::emit(TokenKind kind, size_t start, std::string rawValue) {
    if (rawValue.empty() && position > start &&
        kind != stringLiteral && kind != byteLiteral) {
        rawValue = sourcefile->content.substr(start, position - start);
    }

    const auto token = std::make_shared<Token>(
            kind,
            rawValue,
            SourceSpan { static_cast<uint32_t>(start), static_cast<uint32_t>(position - start) },
            tokenLine,
            tokenColumn,
            nextTokenStartsLine);
    sourcefile->tokens.push_back(token);
    nextTokenStartsLine = false;
}

void LexParser::emitInvalid(size_t start, std::string rawValue) {
    emit(invalid, start, std::move(rawValue));
}

void LexParser::report(size_t line, size_t column, const char* error, ...) {
    char message[2048] = {};
    va_list args;
    va_start(args, error);
    std::vsnprintf(message, sizeof(message), error, args);
    va_end(args);
    const auto offset = line < sourcefile->lineStarts.size()
            ? static_cast<size_t>(sourcefile->lineStarts[line]) + column
            : position;
    const auto length = position > offset ? position - offset : size_t { 1 };
    if (profile == LexerProfile::jsonParserMvp) {
        diagnostics->reportSourceDiagnostic(
                ErrorLevel::failure,
                lexerDiagnosticCode(error),
                sourcefile->getLocation(),
                sourcefile->content,
                sourcefile->lineStarts,
                static_cast<uint32_t>(offset),
                static_cast<uint32_t>(length),
                message);
    } else {
        diagnostics->reportError(ErrorLevel::failure,
                                 static_cast<int>(line),
                                 static_cast<int>(column),
                                 "%s",
                                 message);
    }
}

bool LexParser::isIdentifierHead(char value) const {
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') ||
           value == '_';
}

bool LexParser::isIdentifierTail(char value) const {
    return isIdentifierHead(value) || isDigit(value);
}

bool LexParser::isMvpKeyword(TokenKind kind) const {
    switch (kind) {
        case kwFunc:
        case kwStruct:
        case kwEnum:
        case kwVar:
        case kwLet:
        case kwIf:
        case kwElse:
        case kwWhile:
        case kwReturn:
        case kwMatch:
        case kwInout:
            return true;
        default:
            return false;
    }
}

bool LexParser::isMvpOperator(TokenKind kind) const {
    switch (kind) {
        case equal:
        case notEqual:
        case equalEqual:
        case andAnd:
        case question:
        case plus:
        case minus:
        case multiply:
        case less:
        case lessEqual:
        case greater:
        case greaterEqual:
        case ampersand:
            return true;
        default:
            return false;
    }
}
