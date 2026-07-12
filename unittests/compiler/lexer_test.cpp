#include "joyeer/compiler/lexparser.h"
#include "joyeer/compiler/sourcefile.h"
#include "joyeer/compiler/symtable.h"
#include "joyeer/diagnostic/diagnostic.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
#include <vector>

class LexerTest : public testing::Test {
protected:
    void lex(const std::string& text, LexerProfile profile = LexerProfile::jsonParserMvp) {
        diagnostics.errors.clear();
        source = std::make_shared<SourceFile>(text);
        auto context = std::make_shared<CompileContext>(
                &diagnostics,
                std::make_shared<SymbolTable>());
        LexParser lexer(context, profile);
        lexer.parse(source);
    }

    void expectKinds(std::initializer_list<TokenKind> expected) {
        ASSERT_EQ(source->tokens.size(), expected.size());
        size_t index = 0;
        for (const auto kind : expected) {
            EXPECT_EQ(source->tokens[index]->kind, kind) << "token index " << index;
            ++index;
        }
    }

    Diagnostics diagnostics;
    SourceFile::Ptr source;
};

TEST_F(LexerTest, EmptySourceProducesOneEof) {
    lex("");

    expectKinds({endOfFile});
    EXPECT_EQ(source->tokens[0]->span.offset, 0u);
    EXPECT_EQ(source->tokens[0]->span.length, 0u);
    EXPECT_TRUE(source->tokens[0]->startsLine);
    EXPECT_TRUE(diagnostics.errors.empty());
}

TEST_F(LexerTest, ClassifiesMvpKeywordsAndLiteralWords) {
    lex("func struct enum let var if else while match return inout true false nil");

    expectKinds({
        kwFunc, kwStruct, kwEnum, kwLet, kwVar, kwIf, kwElse, kwWhile,
        kwMatch, kwReturn, kwInout, booleanLiteral, booleanLiteral, nilLiteral,
        endOfFile
    });
    EXPECT_TRUE(diagnostics.errors.empty());
}

TEST_F(LexerTest, ReservesDeferredWordsButLeavesRemovedEffectsAsIdentifiers) {
    lex("for where public performs pure");

    expectKinds({deferredKeyword, deferredKeyword, deferredKeyword,
                 identifier, identifier, endOfFile});
    ASSERT_EQ(diagnostics.errors.size(), 3u);
}

TEST_F(LexerTest, UsesLongestMatchForMvpOperators) {
    lex("= == => != <= < >= > && & ? + - *");

    expectKinds({
        equal, equalEqual, fatArrow, notEqual, lessEqual, less,
        greaterEqual, greater, andAnd, ampersand, question,
        plus, minus, multiply, endOfFile
    });
    EXPECT_TRUE(diagnostics.errors.empty());
}

TEST_F(LexerTest, ScansDecimalIntegersWithoutImplicitOctal) {
    lex("0 00 077 42");

    expectKinds({decimalLiteral, decimalLiteral, decimalLiteral, decimalLiteral, endOfFile});
    EXPECT_EQ(source->tokens[0]->intValue, 0);
    EXPECT_EQ(source->tokens[1]->intValue, 0);
    EXPECT_EQ(source->tokens[2]->intValue, 77);
    EXPECT_EQ(source->tokens[3]->intValue, 42);
    EXPECT_TRUE(diagnostics.errors.empty());
}

TEST_F(LexerTest, DecodesTheFixedStringEscapeSet) {
    lex("\"a\\n\\t\\\"\\\\\\0\"");

    expectKinds({stringLiteral, endOfFile});
    const std::string expected {'a', '\n', '\t', '"', '\\', '\0'};
    EXPECT_EQ(source->tokens[0]->rawValue, expected);
    EXPECT_EQ(source->tokens[0]->span.offset, 0u);
    EXPECT_EQ(source->tokens[0]->span.length, 13u);
    EXPECT_TRUE(diagnostics.errors.empty());
}

TEST_F(LexerTest, ScansJsonByteLiterals) {
    lex(R"(b'{' b'\n' b'\\' b'"')");

    expectKinds({byteLiteral, byteLiteral, byteLiteral, byteLiteral, endOfFile});
    EXPECT_EQ(source->tokens[0]->intValue, static_cast<int64_t>('{'));
    EXPECT_EQ(source->tokens[1]->intValue, static_cast<int64_t>('\n'));
    EXPECT_EQ(source->tokens[2]->intValue, static_cast<int64_t>('\\'));
    EXPECT_EQ(source->tokens[3]->intValue, static_cast<int64_t>('"'));
    EXPECT_TRUE(diagnostics.errors.empty());
}

TEST_F(LexerTest, TokenizesTheJsonParserMvpSurface) {
    lex(R"JOYEER(
enum JsonValue {
    Null,
    Bool(Bool),
    Number(Int),
    Str(String),
}

enum JsonError { UnexpectedEof }

struct Parser {
    var input: String
    var pos: Int
}

func peek(p: Parser): UInt8? {
    if p.pos >= p.input.count { return nil }
    return p.input[p.pos]
}

func parseValue(p: inout Parser): Result<JsonValue, JsonError> {
    let c = match peek(p: p) {
        .Some(value) => value,
        .None => return .Err(.UnexpectedEof),
    }
    &p.pos = p.pos + 1
    return match c {
        b'{' => .Null,
        b't' => .Bool(true),
        _ => .Number(0),
    }
}

var sample = "{\"n\":42}"
)JOYEER");

    EXPECT_TRUE(diagnostics.errors.empty());
    EXPECT_EQ(source->tokens.back()->kind, endOfFile);

    bool sawEnum = false;
    bool sawMatch = false;
    bool sawByte = false;
    bool sawFatArrow = false;
    bool sawInoutMarker = false;
    for (const auto& token : source->tokens) {
        sawEnum = sawEnum || token->kind == kwEnum;
        sawMatch = sawMatch || token->kind == kwMatch;
        sawByte = sawByte || token->kind == byteLiteral;
        sawFatArrow = sawFatArrow || token->kind == fatArrow;
        sawInoutMarker = sawInoutMarker || token->kind == ampersand;
    }
    EXPECT_TRUE(sawEnum);
    EXPECT_TRUE(sawMatch);
    EXPECT_TRUE(sawByte);
    EXPECT_TRUE(sawFatArrow);
    EXPECT_TRUE(sawInoutMarker);
}

TEST_F(LexerTest, TracksSpansLinesAndCommentTrivia) {
    lex("let x = 1\r\n/* outer\n /* inner */ */\nvar y = 2");

    ASSERT_GE(source->tokens.size(), 9u);
    EXPECT_EQ(source->tokens[0]->kind, kwLet);
    EXPECT_EQ(source->tokens[0]->span.offset, 0u);
    EXPECT_EQ(source->tokens[0]->lineNumber, 0u);
    EXPECT_EQ(source->tokens[0]->columnAt, 0u);
    EXPECT_TRUE(source->tokens[0]->startsLine);

    const auto varToken = source->tokens[4];
    EXPECT_EQ(varToken->kind, kwVar);
    EXPECT_EQ(varToken->lineNumber, 3u);
    EXPECT_EQ(varToken->columnAt, 0u);
    EXPECT_TRUE(varToken->startsLine);
    EXPECT_EQ(source->lineStarts.size(), 4u);
    EXPECT_TRUE(diagnostics.errors.empty());
}

TEST_F(LexerTest, RejectsDeferredMvpSyntaxAsSingleInvalidTokens) {
    lex("?? ?. || ! / % ; += 1.2 0xff");

    expectKinds({invalid, invalid, invalid, invalid, invalid, invalid,
                 invalid, invalid, invalid, invalid, endOfFile});
    EXPECT_EQ(diagnostics.errors.size(), 10u);
}

TEST_F(LexerTest, DiagnosesMalformedLiteralsAndUnknownCharacters) {
    lex("b'' b'ab' b'\\q' \"bad\\q\" `");

    expectKinds({invalid, invalid, invalid, invalid, invalid, endOfFile});
    EXPECT_EQ(diagnostics.errors.size(), 5u);
}

TEST_F(LexerTest, RetokenizingDoesNotDuplicateTokens) {
    diagnostics.errors.clear();
    source = std::make_shared<SourceFile>("let x = 1");
    auto context = std::make_shared<CompileContext>(
            &diagnostics,
            std::make_shared<SymbolTable>());
    LexParser lexer(context, LexerProfile::jsonParserMvp);

    lexer.parse(source);
    const size_t count = source->tokens.size();
    lexer.parse(source);

    EXPECT_EQ(source->tokens.size(), count);
    expectKinds({kwLet, identifier, equal, decimalLiteral, endOfFile});
    EXPECT_TRUE(diagnostics.errors.empty());
}

TEST_F(LexerTest, LegacyProfileKeepsExistingKeywordAndOperatorSurface) {
    lex("class for in init self 09 / || !", LexerProfile::legacy);

    expectKinds({kwClass, kwFor, kwIn, kwInit, kwSelf, decimalLiteral,
                 divide, orOr, bang, endOfFile});
    ASSERT_EQ(diagnostics.errors.size(), 1u);
    EXPECT_EQ(diagnostics.errors[0].message, Diagnostics::errorOctalNumberFormat);
}
