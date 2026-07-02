#include "joyeer/compiler/token.h"
#include <unordered_map>
#include <utility>

Token::Token(TokenKind kind, const std::string& rawValue, size_t lineNumber, size_t columnAt):
  kind(kind),
  rawValue(rawValue),
  lineNumber(lineNumber),
  columnAt(columnAt) {

}

std::unordered_set<std::string> initKeywordMap();

const std::string Keywords::CLASS = "class";
const std::string Keywords::FUNC = "func";
const std::string Keywords::STRUCT = "struct";
const std::string Keywords::VAR = "var";
const std::string Keywords::LET = "let";
const std::string Keywords::IF = "if";
const std::string Keywords::ELSE = "else";
const std::string Keywords::FOR = "for";
const std::string Keywords::WHILE = "while";
const std::string Keywords::IMPORT = "import";
const std::string Keywords::TRY = "try";
const std::string Keywords::IN = "in";
const std::string Keywords::INIT = "init";
const std::string Keywords::SELF = "self";
const std::string Keywords::RETURN = "return";
const std::string Keywords::FILEIMPORT = "fileimport";

const std::unordered_set<std::string> Keywords::map = initKeywordMap();

std::unordered_set<std::string> initKeywordMap() {
    std::unordered_set<std::string> map;
    map.insert(Keywords::CLASS);
    map.insert(Keywords::FUNC);
    map.insert(Keywords::STRUCT);
    map.insert(Keywords::VAR);
    map.insert(Keywords::LET);
    map.insert(Keywords::IF);
    map.insert(Keywords::ELSE);
    map.insert(Keywords::FOR);
    map.insert(Keywords::WHILE);
    map.insert(Keywords::IMPORT);
    map.insert(Keywords::TRY);
    map.insert(Keywords::IN);
    map.insert(Keywords::INIT);
    map.insert(Keywords::SELF);
    map.insert(Keywords::RETURN);
    map.insert(Keywords::FILEIMPORT);

    return map;
}

bool isKeyword(const std::string& keyword) {
  return Keywords::map.find(keyword) != Keywords::map.end();
}

/// Punctuation
const std::string Punctuations::OPEN_CURLY_BRACKET = "{";
const std::string Punctuations::CLOSE_CURLY_BRACKET = "}";
const std::string Punctuations::OPEN_ROUND_BRACKET = "(";
const std::string Punctuations::CLOSE_ROUND_BRACKET = ")";
const std::string Punctuations::OPEN_SQUARE_BRACKET = "[";
const std::string Punctuations::CLOSE_SQUARE_BRACKET = "]";
const std::string Punctuations::COLON = ":";
const std::string Punctuations::COMMA = ",";
const std::string Punctuations::DOT = ".";


// Operators
const std::string Operators::EQUALS = "=";
const std::string Operators::NOT_EQUALS = "!=";
const std::string Operators::EQUAL_EQUAL = "==";
const std::string Operators::AND_AND = "&&";
const std::string Operators::OR_OR = "||";
const std::string Operators::QUESTION = "?";
const std::string Operators::POINT = "!";
const std::string Operators::PLUS = "+";
const std::string Operators::MINUS = "-";
const std::string Operators::MULTIPLY = "*";
const std::string Operators::DIV = "/";
const std::string Operators::PERCENTAGE = "%";
const std::string Operators::GREATER = ">";
const std::string Operators::LESS = "<";
const std::string Operators::LESS_EQ = "<=";
const std::string Operators::GREATER_EQ = ">=";

std::unordered_map<std::string, OperatorPriority> initOperatorPriorities() {
    std::unordered_map<std::string, OperatorPriority> result {
        { Operators::EQUALS, OperatorPriority::high },
        { Operators::NOT_EQUALS, OperatorPriority::high },
        { Operators::EQUAL_EQUAL, OperatorPriority::high },
        { Operators::AND_AND,     OperatorPriority::low },
        { Operators::OR_OR,       OperatorPriority::low },
        { Operators::QUESTION,    OperatorPriority::high },
        { Operators::POINT,       OperatorPriority::high },
        { Operators::PLUS,        OperatorPriority::low },
        { Operators::MINUS,       OperatorPriority::low },
        { Operators::MULTIPLY,    OperatorPriority::high },
        { Operators::DIV,         OperatorPriority::high },
        { Operators::PERCENTAGE,  OperatorPriority::high },
        { Operators::GREATER,     OperatorPriority::high },
        { Operators::GREATER_EQ,  OperatorPriority::high },
        { Operators::LESS,        OperatorPriority::high },
        { Operators::LESS_EQ,     OperatorPriority::high }
    };
    return result;
}

const std::unordered_map<std::string, OperatorPriority> Operators::prioprityMap = initOperatorPriorities();

OperatorPriority Operators::getPriority(const std::string& op) {
    return prioprityMap.find(op)->second;
}


// Literals
const std::string Literals::FALSE = "false";
const std::string Literals::TRUE = "true";
const std::string Literals::NIL = "nil";

