#include "joyeer/compiler/lexparser.h"
#include "joyeer/diagnostic/diagnostic.h"

LexParser::LexParser(CompileContext::Ptr context) {
    this->diagnostics = context->diagnostics;
}

void LexParser::parse(const SourceFile::Ptr& sourceFile) {
    
    this->sourcefile = sourceFile;
    iterator = sourceFile->content.begin();
    endIterator = sourceFile->content.end();
    
    while (iterator != endIterator) {
        switch (*iterator ++) {
            case '\u0000':
            case '\u0009':
            case '\u000B':
            case '\u000C':
            case '\u0020':
                break;
            case '\u000A':
                lineNumber ++;
                lineStartAtPosition = (iterator - 1);
                break;
            case '\u000D':
                lineNumber ++;
                lineStartAtPosition = iterator - 1;
                if(iterator != endIterator && *iterator == '\u000A') {
                    iterator ++;
                }
                break;
            case 'a': case 'b': case 'c': case 'd': case 'e': case 'f': case 'g': case 'h': case 'i': case 'j': case 'k': case 'l': case 'm': case 'n': case 'o': case 'p': case 'q': case 'r': case 's': case 't': case 'u': case 'v': case 'w': case 'x': case 'y': case 'z':
            case 'A': case 'B': case 'C': case 'D': case 'E': case 'F': case 'G': case 'H': case 'I': case 'J': case 'K': case 'L': case 'M': case 'N': case 'O': case 'P': case 'Q': case 'R': case 'S': case 'T': case 'U': case 'V': case 'W': case 'X': case 'Y': case 'Z':
            case '_':
                parseStringIdentifier();
                break;
            case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
                parseNumberLiteral(iterator - 1);
                break;
            case '0':
                if (iterator == endIterator) {
                    auto token = std::make_shared<Token>(TokenKind::decimalLiteral, "0", lineNumber, iterator - lineStartAtPosition);
                    token->intValue = 0;
                    sourceFile->tokens.push_back(token);
                    break;
                }
                switch (*iterator) {
                    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7':
                        parseOctalLiteral(iterator);
                        break;
                    case '8': case '9':
                        diagnostics->reportError(ErrorLevel::failure, (int)lineNumber, (int)(iterator - lineStartAtPosition), Diagnostics::errorOctalNumberFormat);
                        break;
                    default:
                        auto token = std::make_shared<Token>(TokenKind::decimalLiteral, "0", lineNumber, iterator - lineStartAtPosition);
                        token->intValue = 0;
                        sourceFile->tokens.push_back(token);
                        break;
                }
                
                break;
            case '/':
                if (iterator != endIterator && *iterator == '/') {
                    parseCppComment();
                    continue;
                } if (iterator != endIterator && *iterator == '*') {
                    iterator ++;
                    parseCComment();
                    continue;
                }
                parseOperator(iterator - 1);
                break;
            case '=':
                if (iterator != endIterator) {
                    if (*iterator == '=') {
                        iterator ++;
                        pushOperator(Operators::EQUAL_EQUAL, iterator);
                        continue;
                    }
                }
                parseOperator(iterator - 1);
                break;
            case '-':
                parseOperator(iterator - 1);
                break;
            case '+':
                parseOperator(iterator - 1);
                break;
            case '!':
                if(iterator != endIterator) {
                    if(*iterator == '=') {
                        iterator ++;
                        pushOperator(Operators::NOT_EQUALS, iterator);
                        continue;
                    }
                }
                parseOperator(iterator - 1);
                break;
            case '*':
                parseOperator(iterator - 1);
                break;
            case '%':
                parseOperator(iterator - 1);
                break;
            case '<':
                if(iterator != endIterator) {
                    if(*iterator == '=') {
                        iterator ++;
                        pushOperator(Operators::LESS_EQ, iterator);
                        continue;
                    }
                }
                parseOperator(iterator - 1);
                break;
            case '>':
                if(iterator != endIterator) {
                    if(*iterator == '=') {
                        iterator ++;
                        pushOperator(Operators::GREATER_EQ, iterator);
                        continue;
                    }
                }
                parseOperator(iterator - 1);
                break;
            case '&':
                if (iterator != endIterator) {
                    if (*iterator == '&') {
                        iterator ++;
                        pushOperator(Operators::AND_AND, iterator);
                        continue;
                    }
                }
                parseOperator(iterator - 1);
                break;
            case '|':
                parseOperator(iterator - 1);
                break;
            case '^':
                parseOperator(iterator - 1);
                break;
            case '~':
                parseOperator(iterator - 1);
                break;
            case '?':
                parseOperator(iterator - 1);
                break;
            case '(':
                parsePunctuation(iterator - 1);
                break;
            case ')':
                parsePunctuation(iterator - 1);
                break;
            case '{':
                parsePunctuation(iterator - 1);
                break;
            case '}':
                parsePunctuation(iterator - 1);
                break;
            case '[':
                parsePunctuation(iterator - 1);
                break;
            case ']':
                parsePunctuation(iterator - 1);
                break;
            case '.':
                parsePunctuation(iterator - 1);
                break;
            case ',':
                parsePunctuation(iterator - 1);
                break;
            case ':':
                parsePunctuation(iterator - 1);
                break;
            case '@':
                parsePunctuation(iterator - 1);
                break;
            case '#':
                parsePunctuation(iterator - 1);
                break;
            case ';':
                parsePunctuation(iterator - 1);
                break;
            case '\"':
                parseStringLiteral();
                break;
            default:
                break;
            }
  }
}

void LexParser::parseOctalLiteral(std::string::const_iterator startAt) {
    while (iterator < endIterator) {
        switch (*iterator) {
            case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7':
                iterator ++;
                break;
            case '8': case '9':
                diagnostics->reportError(ErrorLevel::failure, (int)lineNumber, (int)(startAt - lineStartAtPosition), Diagnostics::errorOctalNumberFormat);
                goto label;
            default:
                goto label;
        }
    }
    
label:
    std::string number(startAt, iterator);
    auto token = std::make_shared<Token>(TokenKind::decimalLiteral, number, lineNumber, startAt - lineStartAtPosition);
    sourcefile->tokens.push_back(token);
}

void LexParser::parseNumberLiteral(std::string::const_iterator startAt) {

  bool isFloatingLiteral = false;
  while (iterator != endIterator) {
    switch (*iterator ++)
    {
    case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9': case '0':
      break;
    default:
      iterator --;
      goto break_label_1;
    }
  }

  break_label_1:

  bool hasFraction = false;
  if(*iterator == '.') {
    iterator ++ ;
    std::string::const_iterator fractionStartIterator = iterator;
    while(iterator != endIterator ) {
      switch (*iterator ++)
      {
      case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9': case '0':
        hasFraction = true;
        isFloatingLiteral = true;
        break;
      default:
        iterator --;
        break;
      }
    }
    if(!hasFraction) {
      iterator --;
    } 
  }

    std::string identifier(startAt, iterator);
    auto token = std::make_shared<Token>(TokenKind::decimalLiteral, identifier, lineNumber, iterator - startAt);
    token->intValue = std::stoi(identifier);
    sourcefile->tokens.push_back(token);
}

void LexParser::parseOperator(std::string::const_iterator startIterator) {
  std::string operators(startIterator, startIterator + 1);
  sourcefile->tokens.push_back(
    std::make_shared<Token>(TokenKind::operators, operators, lineNumber, iterator - startIterator)
  );
}

void LexParser::pushOperator(std::string op, std::string::const_iterator startIterator) {
    sourcefile->tokens.push_back(std::make_shared<Token>(TokenKind::operators, op, lineNumber, iterator - startIterator));
}

void LexParser::parsePunctuation(std::string::const_iterator startIterator) {
  std::string punctuation(startIterator, startIterator + 1);
  sourcefile->tokens.push_back(
          std::make_shared<Token>(TokenKind::punctuation, punctuation, lineNumber, iterator - startIterator)
  );
}

void LexParser::parseStringIdentifier() {
  std::string::const_iterator startAt = iterator - 1;
  while(iterator != endIterator) {
    switch (*iterator ++) {
    case 'a': case 'b': case 'c': case 'd': case 'e': case 'f': case 'g': case 'h': case 'i': case 'j': case 'k': case 'l': case 'm': case 'n': case 'o': case 'p': case 'q': case 'r': case 's': case 't': case 'u': case 'v': case 'w': case 'x': case 'y': case 'z':
    case 'A': case 'B': case 'C': case 'D': case 'E': case 'F': case 'G': case 'H': case 'I': case 'J': case 'K': case 'L': case 'M': case 'N': case 'O': case 'P': case 'Q': case 'R': case 'S': case 'T': case 'U': case 'V': case 'W': case 'X': case 'Y': case 'Z':
    case '_':
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
      break; 
    default:
      iterator --;
      goto exit_label;
    }
  }

  exit_label:
  std::string identifier(startAt, iterator);
  
    std::shared_ptr<Token> token;
    if(isKeyword(identifier)) {
        token = std::make_shared<Token>(TokenKind::keyword, identifier, lineNumber, iterator - startAt);
    } else if(identifier == Literals::NIL) {
        token = std::make_shared<Token>(TokenKind::nilLiteral, identifier, lineNumber, iterator - startAt);
    } else if(identifier == Literals::TRUE || identifier == Literals::FALSE) {
        token = std::make_shared<Token>(TokenKind::booleanLiteral, identifier, lineNumber, iterator - startAt);
    } else {
        token = std::make_shared<Token>(TokenKind::identifier, identifier, lineNumber, iterator - startAt);
    }
    sourcefile->tokens.push_back(token);
}

void LexParser::parseStringLiteral() {
    auto startAt = iterator;
    bool end = false;
    while(iterator != endIterator) {
        if(*iterator == '\\') {
            iterator ++;
            if(iterator == endIterator) {
                diagnostics->reportError(ErrorLevel::failure, (int)lineNumber, (int)(iterator - lineStartAtPosition), Diagnostics::errorUnterminatedStringLiteral);
                return;
            }
        } else if (*iterator == '\"') {
            iterator ++ ;
            end = true;
            break;
        }
        iterator ++;
    }

    if(!end) {
        diagnostics->reportError(ErrorLevel::failure, (int)lineNumber, (int)(iterator - lineStartAtPosition), Diagnostics::errorUnterminatedStringLiteral);
        return;
    }
    
    const std::string identifier(startAt, iterator - 1);
    auto stringLiteral = std::make_shared<Token>(TokenKind::stringLiteral, identifier, lineNumber, iterator - startAt);
    sourcefile->tokens.push_back(stringLiteral);
}

void LexParser::parseCppComment() {
    while(iterator != endIterator && *iterator != '\n') {
        iterator ++;
    }
}

void LexParser::parseCComment() {


    bool found = false;
    while(iterator != endIterator && (iterator + 1) != endIterator) {
        if(*iterator == '*' && *(iterator + 1) == '/') {
            found = true;
            iterator += 2;
            break;
        }

        iterator ++;
    }

    if(!found) {
        diagnostics->reportError(ErrorLevel::failure, (int)lineNumber, 0, Diagnostics::errorUnterminatedCComment);
    }

}
