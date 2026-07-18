#include "joyeer/compiler/lexparser.h"
#include "joyeer/compiler/sourcefile.h"
#include "joyeer/diagnostic/diagnostic.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

class LexerTest : public testing::Test {
protected:
    void lex(const std::string& text, LexerProfile profile = LexerProfile::jsonParserMvp) {
        diagnostics.errors.clear();
        source = std::make_shared<SourceFile>(text);
        LexParser lexer(&diagnostics, profile);
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

TEST(SourceFileTest, PreservesCrLfBytesFromDisk) {
    const auto unique = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
    const auto directory = std::filesystem::temp_directory_path();
    const auto path = directory / ("joyeer-sourcefile-crlf-" + unique + ".joyeer");
    {
        std::ofstream output(path, std::ios::binary);
        ASSERT_TRUE(output.good());
        output << "let first = 1\r\nlet second = 2\r\n";
    }

    const auto source = std::make_shared<SourceFile>(directory.string(), path.string());
    ASSERT_TRUE(source->loaded());
    EXPECT_EQ(source->content, "let first = 1\r\nlet second = 2\r\n");
    std::error_code error;
    std::filesystem::remove(path, error);
    EXPECT_FALSE(error);
}

TEST(SourceFileTest, RejectsDirectoriesAsSourceFiles) {
    const auto directory = std::filesystem::temp_directory_path();
    const auto source = std::make_shared<SourceFile>(directory, directory);

    EXPECT_FALSE(source->loaded());
    EXPECT_EQ(source->loadingError(), SourceFile::LoadError::notRegularFile);
    EXPECT_TRUE(source->content.empty());
}

TEST_F(LexerTest, EmptySourceProducesOneEof) {
    lex("");

    expectKinds({endOfFile});
    EXPECT_EQ(source->tokens[0]->span.offset, 0u);
    EXPECT_EQ(source->tokens[0]->span.length, 0u);
    EXPECT_TRUE(source->tokens[0]->startsLine);
    EXPECT_TRUE(diagnostics.errors.empty());
}

TEST_F(LexerTest, ClassifiesMvpKeywordsAndLiteralWords) {
    lex("func struct enum let var if else while match return inout borrowing consuming initializing consume true false nil");

    expectKinds({
        kwFunc, kwStruct, kwEnum, kwLet, kwVar, kwIf, kwElse, kwWhile,
        kwMatch, kwReturn, kwInout, kwBorrowing, kwConsuming, kwInitializing,
        kwConsume,
        booleanLiteral, booleanLiteral, nilLiteral,
        endOfFile
    });
    EXPECT_TRUE(diagnostics.errors.empty());
}

TEST_F(LexerTest, ClassifiesIdentifiersWildcardAndPunctuation) {
    lex("x _x matchValue b0 _ { } ( ) [ ] : , .");

    expectKinds({
        identifier, identifier, identifier, identifier, wildcard,
        leftCurly, rightCurly, leftParen, rightParen, leftSquare, rightSquare,
        colon, comma, dot, endOfFile
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

TEST_F(LexerTest, TracksEverySupportedNewlineForm) {
    lex(" \tlet\rvar\nfunc\r\n");

    expectKinds({kwLet, kwVar, kwFunc, endOfFile});
    EXPECT_EQ(source->tokens[0]->lineNumber, 0u);
    EXPECT_EQ(source->tokens[0]->columnAt, 2u);
    EXPECT_EQ(source->tokens[1]->lineNumber, 1u);
    EXPECT_EQ(source->tokens[1]->columnAt, 0u);
    EXPECT_EQ(source->tokens[2]->lineNumber, 2u);
    EXPECT_EQ(source->tokens[2]->columnAt, 0u);
    EXPECT_EQ(source->tokens[3]->lineNumber, 3u);
    EXPECT_EQ(source->tokens[3]->span.offset, 16u);
    EXPECT_EQ(source->tokens[3]->span.length, 0u);
    EXPECT_EQ(source->lineStarts, (std::vector<uint32_t> {0, 6, 10, 16}));
    for (const auto& token : source->tokens) {
        EXPECT_TRUE(token->startsLine);
    }
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

TEST_F(LexerTest, PreservesInt64WidthAndDiagnosesOverflow) {
    lex("9223372036854775807 9223372036854775808");

    expectKinds({decimalLiteral, decimalLiteral, endOfFile});
    EXPECT_EQ(source->tokens[0]->intValue, std::numeric_limits<int64_t>::max());
    ASSERT_EQ(diagnostics.errors.size(), 1u);
    EXPECT_EQ(diagnostics.errors[0].message, Diagnostics::errorIntegerLiteralOverflow);
}

TEST_F(LexerTest, RejectsUnsupportedNumbersAsWholeTokens) {
    lex("1.2 1e3 0xff 0b1 0o7 12abc 1_000");

    expectKinds({invalid, invalid, invalid, invalid, invalid, invalid, invalid, endOfFile});
    ASSERT_EQ(diagnostics.errors.size(), 7u);
    for (size_t index = 0; index < 5; ++index) {
        EXPECT_EQ(diagnostics.errors[index].message, Diagnostics::errorUnsupportedNumericLiteral);
    }
    EXPECT_EQ(diagnostics.errors[5].message, Diagnostics::errorInvalidNumericSuffix);
    EXPECT_EQ(diagnostics.errors[6].message, Diagnostics::errorInvalidNumericSuffix);
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

TEST_F(LexerTest, AcceptsEmptyUtf8AndCarriageReturnStrings) {
    lex("\"\" \"é\" \"\\r\"");

    expectKinds({stringLiteral, stringLiteral, stringLiteral, endOfFile});
    EXPECT_TRUE(source->tokens[0]->rawValue.empty());
    EXPECT_EQ(source->tokens[1]->rawValue, "é");
    EXPECT_EQ(source->tokens[2]->rawValue, std::string(1, '\r'));
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

TEST_F(LexerTest, ScansAllRemainingJsonBytesAndEscapes) {
    lex("b'}' b'[' b']' b':' b',' b' ' b'\\t' b'\\r' b'\\0' b'\\\''");

    expectKinds({
        byteLiteral, byteLiteral, byteLiteral, byteLiteral, byteLiteral,
        byteLiteral, byteLiteral, byteLiteral, byteLiteral, byteLiteral,
        endOfFile
    });
    const std::vector<int64_t> expected {
        '}', '[', ']', ':', ',', ' ', '\t', '\r', '\0', '\''
    };
    for (size_t index = 0; index < expected.size(); ++index) {
        EXPECT_EQ(source->tokens[index]->intValue, expected[index]);
    }
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
    lex("?? ?. || ! / % ; @ # 'x' += << >> 1.2 0xff");

    expectKinds({invalid, invalid, invalid, invalid, invalid, invalid,
                 invalid, invalid, invalid, invalid, invalid, invalid,
                 invalid, invalid, invalid, endOfFile});
    EXPECT_EQ(diagnostics.errors.size(), 15u);
}

TEST_F(LexerTest, RejectsCompoundAndShiftOperatorsAsWholeTokens) {
    lex("-= *= /= %= &= |= ^= ~= <<= >>= &&= ||=");

    expectKinds({
        invalid, invalid, invalid, invalid, invalid, invalid,
        invalid, invalid, invalid, invalid, invalid, invalid,
        endOfFile
    });
    ASSERT_EQ(diagnostics.errors.size(), 12u);

    const std::vector<std::string> expected {
        "-=", "*=", "/=", "%=", "&=", "|=", "^=", "~=",
        "<<=", ">>=", "&&=", "||="
    };
    for (size_t index = 0; index < expected.size(); ++index) {
        EXPECT_EQ(source->tokens[index]->rawValue, expected[index]);
        EXPECT_EQ(source->tokens[index]->span.length, expected[index].size());
    }
}

TEST_F(LexerTest, DiagnosesMalformedLiteralsAndUnknownCharacters) {
    lex("b'' b'ab' b'\\q' \"bad\\q\" `");

    expectKinds({invalid, invalid, invalid, invalid, invalid, endOfFile});
    EXPECT_EQ(diagnostics.errors.size(), 5u);
}

TEST_F(LexerTest, DiagnosesNulAndNonAsciiSourceBytes) {
    std::string bytes;
    bytes.push_back('\0');
    bytes.push_back(static_cast<char>(0xc3));
    bytes.push_back(static_cast<char>(0xa9));
    lex(bytes);

    expectKinds({invalid, invalid, invalid, endOfFile});
    ASSERT_EQ(diagnostics.errors.size(), 3u);
    for (const auto& error : diagnostics.errors) {
        EXPECT_EQ(error.level, ErrorLevel::failure);
    }
}

TEST_F(LexerTest, DiagnosesUnterminatedConstructsAtTheirStart) {
    lex("\"unterminated");
    expectKinds({invalid, endOfFile});
    ASSERT_EQ(diagnostics.errors.size(), 1u);
    EXPECT_EQ(diagnostics.errors[0].message, Diagnostics::errorUnterminatedStringLiteral);
    EXPECT_EQ(diagnostics.errors[0].lineAt, 0);
    EXPECT_EQ(diagnostics.errors[0].columnAt, 0);

    lex("b'a");
    expectKinds({invalid, endOfFile});
    ASSERT_EQ(diagnostics.errors.size(), 1u);
    EXPECT_EQ(diagnostics.errors[0].message, Diagnostics::errorUnterminatedByteLiteral);

    lex("/* unterminated");
    expectKinds({endOfFile});
    ASSERT_EQ(diagnostics.errors.size(), 1u);
    EXPECT_EQ(diagnostics.errors[0].message, Diagnostics::errorUnterminatedCComment);
}

TEST_F(LexerTest, LineCommentsMayEndAtEof) {
    lex("// comment without newline");

    expectKinds({endOfFile});
    EXPECT_EQ(source->tokens[0]->span.offset, source->content.size());
    EXPECT_TRUE(diagnostics.errors.empty());
}

TEST_F(LexerTest, RetokenizingDoesNotDuplicateTokens) {
    diagnostics.errors.clear();
    source = std::make_shared<SourceFile>("let x = 1");
        LexParser lexer(&diagnostics);

    lexer.parse(source);
    const size_t count = source->tokens.size();
    lexer.parse(source);

    EXPECT_EQ(source->tokens.size(), count);
    expectKinds({kwLet, identifier, equal, decimalLiteral, endOfFile});
    EXPECT_TRUE(diagnostics.errors.empty());
}

TEST_F(LexerTest, ArbitraryByteBuffersAlwaysEndWithOrderedSpansAndOneEof) {
    uint32_t state = 0x4a6f7965u;
    for (size_t sample = 0; sample < 64; ++sample) {
        std::string bytes;
        bytes.reserve(128);
        for (size_t index = 0; index < 128; ++index) {
            state = state * 1664525u + 1013904223u;
            bytes.push_back(static_cast<char>(state >> 24));
        }

        lex(bytes);
        ASSERT_FALSE(source->tokens.empty()) << "sample " << sample;

        uint32_t previousEnd = 0;
        size_t eofCount = 0;
        for (const auto& token : source->tokens) {
            EXPECT_GE(token->span.offset, previousEnd) << "sample " << sample;
            EXPECT_LE(static_cast<uint64_t>(token->span.offset) + token->span.length,
                      source->content.size()) << "sample " << sample;
            if (token->kind == endOfFile) {
                ++eofCount;
                EXPECT_EQ(token->span.length, 0u);
            } else {
                EXPECT_GT(token->span.length, 0u) << "sample " << sample;
            }
            previousEnd = token->span.offset + token->span.length;
        }

        EXPECT_EQ(eofCount, 1u) << "sample " << sample;
        EXPECT_EQ(source->tokens.back()->kind, endOfFile) << "sample " << sample;
        EXPECT_EQ(source->tokens.back()->span.offset, source->content.size())
                << "sample " << sample;
    }
}

TEST_F(LexerTest, LegacyProfileKeepsExistingKeywordAndOperatorSurface) {
    lex("class for in init self 09 / || !", LexerProfile::legacy);

    expectKinds({kwClass, kwFor, kwIn, kwInit, kwSelf, decimalLiteral,
                 divide, orOr, bang, endOfFile});
    ASSERT_EQ(diagnostics.errors.size(), 1u);
    EXPECT_EQ(diagnostics.errors[0].message, Diagnostics::errorOctalNumberFormat);
}
