#ifndef __joyeer_compiler_lexer_token_h__
#define __joyeer_compiler_lexer_token_h__

#include <string>
#include <unordered_set>
#include <unordered_map>
#include <memory>
#include <vector>

enum TokenKind {
  identifier,
  keyword,
  punctuation,
  operators,
  booleanLiteral,
  nilLiteral,
  floatLiteral,
  decimalLiteral,
  stringLiteral
};

struct Token {
public:
    using Ptr = std::shared_ptr<Token>;
    
public:
    TokenKind kind;
    std::string rawValue;
    union {
        int intValue;
        double doubleValue;
        float floatValue;
        int opValue;
    };

    uint16_t lineNumber;
    uint16_t columnAt;

    Token(TokenKind kind, const std::string& rawValue, size_t lineNumber, size_t columnAt);

};

struct Keywords {
    static const std::string FUNC;
    static const std::string CLASS;
    static const std::string STRUCT;
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
    
    static const std::unordered_set<std::string> map;
};

bool isKeyword(const std::string& keyword);

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
