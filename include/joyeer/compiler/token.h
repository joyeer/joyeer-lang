#ifndef __joyeer_compiler_lexer_token_h__
#define __joyeer_compiler_lexer_token_h__

#include <string>
#include <string_view>
#include <unordered_set>
#include <unordered_map>
#include <memory>
#include <vector>
#include <cstdint>

enum TokenKind {
    // Legacy parser categories. The lexer emits the explicit terminal kinds
    // below; SyntaxParser accepts these categories during the migration.
  identifier,
  keyword,
  punctuation,
  operators,
  booleanLiteral,
  nilLiteral,
  floatLiteral,
  decimalLiteral,
    stringLiteral,
    byteLiteral,

    endOfFile,
    invalid,
    deferredKeyword,
    wildcard,

    kwFunc,
    kwClass,
    kwStruct,
    kwEnum,
    kwVar,
    kwLet,
    kwIf,
    kwElse,
    kwFor,
    kwWhile,
    kwImport,
    kwTry,
    kwIn,
    kwInit,
    kwSelf,
    kwReturn,
    kwFileImport,
    kwMatch,
    kwInout,
    kwBorrowing,
    kwConsuming,
    kwInitializing,
    kwConsume,

    leftCurly,
    rightCurly,
    leftParen,
    rightParen,
    leftSquare,
    rightSquare,
    colon,
    comma,
    dot,
    semicolon,
    atSign,
    hash,
    fatArrow,

    equal,
    notEqual,
    equalEqual,
    andAnd,
    orOr,
    question,
    bang,
    plus,
    minus,
    multiply,
    divide,
    percentage,
    less,
    lessEqual,
    greater,
    greaterEqual,
    ampersand
};

struct SourceSpan {
        uint32_t offset = 0;
        uint32_t length = 0;
};

struct Token {
public:
    using Ptr = std::shared_ptr<Token>;
    
public:
    TokenKind kind;
    std::string rawValue;
    union {
        int64_t intValue;
        double doubleValue;
        float floatValue;
        int opValue;
    };

    SourceSpan span;
    uint32_t lineNumber;
    uint32_t columnAt;
    bool startsLine = false;

    Token(TokenKind kind, const std::string& rawValue, size_t lineNumber, size_t columnAt);
    Token(TokenKind kind,
          const std::string& rawValue,
          SourceSpan span,
          size_t lineNumber,
          size_t columnAt,
          bool startsLine);

};

struct Keywords {
    static const std::string FUNC;
    static const std::string CLASS;
    static const std::string STRUCT;
    static const std::string ENUM;
    static const std::string VAR;
    static const std::string LET;
    static const std::string IF;
    static const std::string ELSE;
    static const std::string FOR;
    static const std::string WHILE;
    static const std::string IMPORT;
    static const std::string TRY;
    static const std::string IN;
    static const std::string INIT;
    static const std::string SELF;
    static const std::string RETURN;
    static const std::string FILEIMPORT;
    static const std::string MATCH;
    static const std::string INOUT;
    static const std::string BORROWING;
    static const std::string CONSUMING;
    static const std::string INITIALIZING;
    static const std::string CONSUME;
    
    static const std::unordered_set<std::string> map;
};

bool isKeyword(const std::string& keyword);
TokenKind keywordKind(std::string_view keyword);
bool isDeferredKeyword(std::string_view keyword);
bool isKeywordKind(TokenKind kind);
bool isPunctuationKind(TokenKind kind);
bool isOperatorKind(TokenKind kind);
bool tokenKindMatches(TokenKind actual, TokenKind expected);

struct Punctuations {
    static const std::string OPEN_CURLY_BRACKET;   // {
    static const std::string CLOSE_CURLY_BRACKET;  // }
    static const std::string OPEN_ROUND_BRACKET;   // (
    static const std::string CLOSE_ROUND_BRACKET;  // )
    static const std::string OPEN_SQUARE_BRACKET;  // [
    static const std::string CLOSE_SQUARE_BRACKET; // ]
    static const std::string COLON;                // :
    static const std::string COMMA;                // ,
    static const std::string DOT;                  // .
    static const std::string SEMICOLON;            // ;
    static const std::string FAT_ARROW;            // =>
};

enum OperatorPriority {
    high,
    low
};

struct Operators {
    static const std::string EQUALS;               // =
    static const std::string NOT_EQUALS;           // !=
    static const std::string EQUAL_EQUAL;          // ==
    static const std::string AND_AND;              // &&
    static const std::string OR_OR;                // ||
    static const std::string QUESTION;             // ?
    static const std::string POINT;                // !
    static const std::string PLUS;                 // +
    static const std::string MINUS;                // -
    static const std::string MULTIPLY;             // *
    static const std::string DIV;                  // /
    static const std::string PERCENTAGE;           // %
    static const std::string LESS;                 // <
    static const std::string LESS_EQ;              // <=
    static const std::string GREATER;              // >
    static const std::string GREATER_EQ;           // >=
    static const std::string AMPERSAND;             // &
    
    
    // get the operator's priority
    static OperatorPriority getPriority(const std::string& name);
    
    static const std::unordered_map<std::string, OperatorPriority> prioprityMap;
};

struct Literals {
  static const std::string TRUE;
  static const std::string FALSE;
  static const std::string NIL;
};

#endif
