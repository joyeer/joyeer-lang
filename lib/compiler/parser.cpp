#include "joyeer/compiler/parser.h"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <utility>

namespace joyeer::parser {

namespace {

uint32_t spanEnd(SourceSpan span) {
    return span.offset + span.length;
}

SourceSpan coveringSpan(SourceSpan first, SourceSpan last) {
    const uint32_t end = std::max(spanEnd(first), spanEnd(last));
    return SourceSpan { first.offset, end - first.offset };
}

std::string quotedToken(const Token::Ptr& token) {
    if (token == nullptr || token->kind == endOfFile) {
        return "end of file";
    }
    return "'" + token->rawValue + "'";
}

std::optional<std::string> canonicalTokenSpelling(TokenKind kind) {
    switch (kind) {
        case leftCurly: return "{";
        case rightCurly: return "}";
        case leftParen: return "(";
        case rightParen: return ")";
        case leftSquare: return "[";
        case rightSquare: return "]";
        case colon: return ":";
        case comma: return ",";
        case dot: return ".";
        case fatArrow: return "=>";
        case question: return "?";
        case greater: return ">";
        default: return std::nullopt;
    }
}

} // namespace

const char* diagnosticName(DiagnosticId id) {
    switch (id) {
        case DiagnosticId::expectedDeclaration: return "parser.expected-declaration";
        case DiagnosticId::expectedIdentifier: return "parser.expected-identifier";
        case DiagnosticId::expectedType: return "parser.expected-type";
        case DiagnosticId::expectedExpression: return "parser.expected-expression";
        case DiagnosticId::expectedPattern: return "parser.expected-pattern";
        case DiagnosticId::expectedBlock: return "parser.expected-block";
        case DiagnosticId::expectedToken: return "parser.expected-token";
        case DiagnosticId::unexpectedToken: return "parser.unexpected-token";
        case DiagnosticId::missingComma: return "parser.missing-comma";
        case DiagnosticId::missingListElement: return "parser.missing-list-element";
        case DiagnosticId::invalidAssignmentTarget: return "parser.invalid-assignment-target";
        case DiagnosticId::chainedComparison: return "parser.chained-comparison";
        case DiagnosticId::sameLineItems: return "parser.same-line-items";
        case DiagnosticId::unexpectedEndOfFile: return "parser.unexpected-eof";
        case DiagnosticId::unsupportedSyntax: return "parser.unsupported-syntax";
    }
    return "parser.unknown";
}

std::string dump(const std::vector<Diagnostic>& diagnostics) {
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << diagnosticName(diagnostic.id)
            << '@' << diagnostic.span.offset << ':' << diagnostic.span.length
            << ' ' << diagnostic.message << '\n';
    }
    return out.str();
}

TokenCursor::TokenCursor(const std::vector<Token::Ptr>& tokens): tokens(tokens) {
    uint32_t offset = 0;
    uint32_t line = 0;
    uint32_t column = 0;
    if (!tokens.empty()) {
        const auto& last = tokens.back();
        offset = spanEnd(last->span);
        line = last->lineNumber;
        column = last->columnAt + last->span.length;
    }
    fallbackEof = std::make_shared<Token>(
            endOfFile,
            "",
            SourceSpan {offset, 0},
            line,
            column,
            true);
}

const Token::Ptr& TokenCursor::peek(size_t offset) const {
    if (index >= tokens.size() || offset >= tokens.size() - index) {
        return fallbackEof;
    }
    return tokens[index + offset];
}

const Token::Ptr& TokenCursor::previous() const {
    if (index == 0 || tokens.empty()) {
        return fallbackEof;
    }
    return tokens[std::min(index, tokens.size()) - 1];
}

const Token::Ptr& TokenCursor::tokenAt(size_t position) const {
    if (position >= tokens.size()) {
        return fallbackEof;
    }
    return tokens[position];
}

bool TokenCursor::at(TokenKind kind, size_t offset) const {
    return peek(offset)->kind == kind;
}

Token::Ptr TokenCursor::eat(TokenKind kind) {
    if (!at(kind)) {
        return nullptr;
    }
    return advance();
}

Token::Ptr TokenCursor::advance() {
    const auto token = peek();
    if (!atEnd() && index < tokens.size()) {
        ++index;
    }
    return token;
}

bool TokenCursor::atEnd() const {
    return peek()->kind == endOfFile;
}

size_t TokenCursor::position() const {
    return index;
}

TokenCursor::Checkpoint TokenCursor::checkpoint() const {
    return index;
}

void TokenCursor::rewind(Checkpoint checkpoint) {
    index = std::min(checkpoint, tokens.size());
}

Parser::Parser(const std::vector<Token::Ptr>& tokens): cursor(tokens) {
}

ParseResult Parser::parse() {
    const size_t start = cursor.position();
    std::vector<syntax::NodePtr> items;

    while (!cursor.atEnd()) {
        const size_t before = cursor.position();
        auto item = parseTopLevelItem();
        if (item != nullptr) {
            items.push_back(std::move(item));
        }
        if (cursor.position() == before) {
            report(DiagnosticId::unexpectedToken,
                   tokenSpan(cursor.peek()),
                   "unexpected token " + quotedToken(cursor.peek()) + " at file scope");
            cursor.advance();
        }
        enforceItemBoundary(true);
    }

    SourceSpan span = spanFrom(start);
    if (items.empty()) {
        span = insertionSpan();
    }
    auto root = std::make_shared<syntax::SourceFileSyntax>(span, std::move(items));
    return ParseResult { std::move(root), std::move(diagnostics) };
}

syntax::NodePtr Parser::parseTopLevelItem() {
    if (cursor.at(invalid) || cursor.at(deferredKeyword)) {
        const auto token = cursor.advance();
        return std::make_shared<syntax::ErrorDeclSyntax>(tokenSpan(token));
    }

    switch (cursor.peek()->kind) {
        case kwLet:
        case kwVar:
            return parseBindingDecl(false);
        case kwFunc:
            return parseFunctionDecl();
        case kwStruct:
            return parseStructDecl();
        case kwEnum:
            return parseEnumDecl();
        default:
            reportExpected(DiagnosticId::expectedDeclaration, "a top-level declaration");
            {
                const size_t start = cursor.position();
                if (!cursor.atEnd()) cursor.advance();
                synchronizeTopLevel();
                return std::make_shared<syntax::ErrorDeclSyntax>(spanFrom(start));
            }
    }
}

syntax::BindingDeclSyntax::Ptr Parser::parseBindingDecl(bool requireType) {
    const size_t start = cursor.position();
    auto keyword = cursor.advance();
    auto name = expect(identifier, "a binding name");

    syntax::TypePtr annotation;
    if (cursor.eat(colon) != nullptr) {
        annotation = parseType();
    } else if (requireType) {
        reportExpected(DiagnosticId::expectedToken, "':' and a stored-field type");
        addExpectedTokenFixIt(colon);
        annotation = std::make_shared<syntax::ErrorTypeSyntax>(insertionSpan());
    }

    syntax::ExprPtr initializer;
    if (cursor.eat(equal) != nullptr) {
        initializer = parseExpression();
        if (initializer == nullptr) {
            reportExpected(DiagnosticId::expectedExpression, "an initializer expression");
            initializer = std::make_shared<syntax::ErrorExprSyntax>(insertionSpan());
        }
    }

    return std::make_shared<syntax::BindingDeclSyntax>(
            spanFrom(start),
            std::move(keyword),
            std::move(name),
            std::move(annotation),
            std::move(initializer));
}

syntax::FunctionDeclSyntax::Ptr Parser::parseFunctionDecl() {
    const size_t start = cursor.position();
    cursor.advance();
    auto name = expect(identifier, "a function name");
    std::vector<syntax::ParameterDeclSyntax::Ptr> parameters;
    if (cursor.eat(leftParen) == nullptr) {
        reportExpected(DiagnosticId::expectedToken, "'(' to begin the parameter clause");
        addExpectedTokenFixIt(leftParen);
    } else {
        if (!cursor.at(rightParen) && !cursor.atEnd()) {
            while (true) {
                const size_t before = cursor.position();
                parameters.push_back(parseParameter());
                if (cursor.position() == before) {
                    cursor.advance();
                }

                if (cursor.eat(comma) != nullptr) {
                    if (cursor.at(rightParen)) break;
                    continue;
                }
                if (cursor.at(rightParen) || cursor.atEnd()) break;

                                reportMissingComma("expected ',' between function parameters");
                synchronizeList(rightParen);
                if (cursor.eat(comma) != nullptr) {
                    if (cursor.at(rightParen)) break;
                    continue;
                }
                break;
            }
        }
        expect(rightParen, "')'");
    }

    syntax::TypePtr returnType;
    if (cursor.eat(colon) != nullptr) {
        returnType = parseType();
    }

    auto body = parseBlock();
    return std::make_shared<syntax::FunctionDeclSyntax>(
            spanFrom(start),
            std::move(name),
            std::move(parameters),
            std::move(returnType),
            std::move(body));
}

syntax::ParameterDeclSyntax::Ptr Parser::parseParameter() {
    const size_t start = cursor.position();
    auto label = expect(identifier, "a parameter label");
    auto name = label;
    if (cursor.at(identifier, 0) && cursor.at(colon, 1)) {
        name = cursor.advance();
    }
    expect(colon, "':' after the parameter name");
    Token::Ptr accessKeyword;
    if (cursor.at(kwInout) || cursor.at(kwBorrowing) ||
        cursor.at(kwConsuming) || cursor.at(kwInitializing)) {
        accessKeyword = cursor.advance();
    }
    auto type = parseType();
    return std::make_shared<syntax::ParameterDeclSyntax>(
            spanFrom(start),
            std::move(label),
            std::move(name),
            std::move(accessKeyword),
            std::move(type));
}

syntax::StructDeclSyntax::Ptr Parser::parseStructDecl() {
    const size_t start = cursor.position();
    cursor.advance();
    auto name = expect(identifier, "a struct name");

    if (cursor.eat(leftCurly) == nullptr) {
        reportExpected(DiagnosticId::expectedBlock, "'{' to begin the struct body");
        return std::make_shared<syntax::StructDeclSyntax>(
                spanFrom(start), std::move(name), std::vector<syntax::StructFieldDeclSyntax::Ptr> {});
    }

    std::vector<syntax::StructFieldDeclSyntax::Ptr> fields;
    while (!cursor.at(rightCurly) && !cursor.atEnd()) {
        if (cursor.peek()->startsLine &&
            (cursor.at(kwFunc) || cursor.at(kwStruct) || cursor.at(kwEnum))) {
            break;
        }
        const size_t before = cursor.position();
        if (cursor.at(kwLet) || cursor.at(kwVar)) {
            fields.push_back(parseStructField());
        } else if (cursor.at(invalid) || cursor.at(deferredKeyword)) {
            cursor.advance();
        } else {
            report(DiagnosticId::unsupportedSyntax,
                   tokenSpan(cursor.peek()),
                   "only stored 'let' and 'var' fields are supported in a Parser MVP struct");
            cursor.advance();
            synchronizeBlock();
        }
        if (cursor.position() == before) cursor.advance();
        enforceItemBoundary(false);
    }
    expect(rightCurly, "'}' to close the struct body");

    return std::make_shared<syntax::StructDeclSyntax>(
            spanFrom(start), std::move(name), std::move(fields));
}

syntax::StructFieldDeclSyntax::Ptr Parser::parseStructField() {
    const size_t start = cursor.position();
    auto keyword = cursor.advance();
    auto name = expect(identifier, "a stored-field name");
    expect(colon, "':' and a stored-field type");
    auto type = parseType();

    syntax::ExprPtr initializer;
    if (cursor.eat(equal) != nullptr) {
        initializer = parseExpression();
        if (initializer == nullptr) {
            reportExpected(DiagnosticId::expectedExpression, "a stored-field initializer");
            initializer = std::make_shared<syntax::ErrorExprSyntax>(insertionSpan());
        }
    }

    return std::make_shared<syntax::StructFieldDeclSyntax>(
            spanFrom(start),
            std::move(keyword),
            std::move(name),
            std::move(type),
            std::move(initializer));
}

syntax::EnumDeclSyntax::Ptr Parser::parseEnumDecl() {
    const size_t start = cursor.position();
    cursor.advance();
    auto name = expect(identifier, "an enum name");
    if (cursor.eat(leftCurly) == nullptr) {
        reportExpected(DiagnosticId::expectedBlock, "'{' to begin the enum body");
        return std::make_shared<syntax::EnumDeclSyntax>(
                spanFrom(start), std::move(name), std::vector<syntax::EnumCaseDeclSyntax::Ptr> {});
    }

    std::vector<syntax::EnumCaseDeclSyntax::Ptr> cases;
    if (cursor.at(rightCurly)) {
        report(DiagnosticId::missingListElement,
               insertionSpan(),
               "an enum must declare at least one case");
    }

    while (!cursor.at(rightCurly) && !cursor.atEnd()) {
        if (cursor.peek()->startsLine && isTopLevelStart(cursor.peek())) {
            break;
        }
        const size_t before = cursor.position();
        if (cursor.at(identifier)) {
            cases.push_back(parseEnumCaseDecl());
        } else if (cursor.at(invalid) || cursor.at(deferredKeyword)) {
            cursor.advance();
        } else {
            reportExpected(DiagnosticId::expectedIdentifier, "an enum case name");
            cursor.advance();
            synchronizeList(rightCurly);
        }
        if (cursor.position() == before) cursor.advance();

        if (cursor.eat(comma) != nullptr) {
            if (cursor.at(rightCurly)) break;
            continue;
        }
        if (cursor.at(rightCurly) || cursor.atEnd()) break;

        reportMissingComma("expected ',' between enum cases");
        if (!cursor.at(identifier) || !cursor.peek()->startsLine) {
            synchronizeList(rightCurly);
            cursor.eat(comma);
        }
    }
    expect(rightCurly, "'}' to close the enum body");

    return std::make_shared<syntax::EnumDeclSyntax>(
            spanFrom(start), std::move(name), std::move(cases));
}

syntax::EnumCaseDeclSyntax::Ptr Parser::parseEnumCaseDecl() {
    const size_t start = cursor.position();
    auto name = cursor.advance();
    const bool hasPayloadClause = cursor.eat(leftParen) != nullptr;
    std::vector<syntax::AssociatedTypeSyntax::Ptr> associatedTypes;

    if (hasPayloadClause) {
        if (cursor.at(rightParen)) {
            report(DiagnosticId::missingListElement,
                   insertionSpan(),
                   "an enum payload clause cannot be empty");
        } else {
            while (!cursor.at(rightParen) && !cursor.atEnd()) {
                const size_t before = cursor.position();
                associatedTypes.push_back(parseAssociatedType());
                if (cursor.position() == before) cursor.advance();

                if (cursor.eat(comma) != nullptr) {
                    if (cursor.at(rightParen)) break;
                    continue;
                }
                if (cursor.at(rightParen) || cursor.atEnd()) break;
                                reportMissingComma("expected ',' between enum associated types");
                synchronizeList(rightParen);
                if (cursor.eat(comma) == nullptr) break;
            }
        }
        expect(rightParen, "')' to close the enum payload");
    }

    return std::make_shared<syntax::EnumCaseDeclSyntax>(
            spanFrom(start),
            std::move(name),
            hasPayloadClause,
            std::move(associatedTypes));
}

syntax::AssociatedTypeSyntax::Ptr Parser::parseAssociatedType() {
    const size_t start = cursor.position();
    Token::Ptr label;
    if (cursor.at(identifier) && cursor.at(colon, 1)) {
        label = cursor.advance();
        cursor.advance();
    }
    auto type = parseType();
    return std::make_shared<syntax::AssociatedTypeSyntax>(
            spanFrom(start), std::move(label), std::move(type));
}

syntax::TypePtr Parser::parseType() {
    const size_t start = cursor.position();
    auto type = parseTypePrimary();
    if (type == nullptr) {
        reportExpected(DiagnosticId::expectedType, "a type");
        if (!cursor.at(comma) && !cursor.at(colon) &&
            !cursor.at(rightParen) && !cursor.at(rightSquare) &&
            !cursor.at(greater) && !cursor.at(leftCurly) &&
            !cursor.at(equal) && !cursor.atEnd()) {
            cursor.advance();
        }
        return std::make_shared<syntax::ErrorTypeSyntax>(spanFrom(start));
    }

    if (cursor.eat(question) != nullptr) {
        type = std::make_shared<syntax::OptionalTypeSyntax>(spanFrom(start), std::move(type));
    }
    return type;
}

syntax::TypePtr Parser::parseTypePrimary() {
    const size_t start = cursor.position();
    if (cursor.at(identifier)) {
        auto name = cursor.advance();
        std::vector<syntax::TypePtr> arguments;
        if (cursor.eat(less) != nullptr) {
            if (cursor.at(greater)) {
                report(DiagnosticId::missingListElement,
                       insertionSpan(),
                       "a generic argument list cannot be empty");
            } else {
                while (!cursor.at(greater) && !cursor.atEnd()) {
                    const size_t before = cursor.position();
                    arguments.push_back(parseType());
                    if (cursor.position() == before) cursor.advance();
                    if (cursor.eat(comma) != nullptr) {
                        if (cursor.at(greater)) break;
                        continue;
                    }
                    if (cursor.at(greater) || cursor.atEnd()) break;
                      reportMissingComma("expected ',' between generic type arguments");
                    synchronizeList(greater);
                    if (cursor.eat(comma) == nullptr) break;
                }
            }
            expect(greater, "'>' to close generic type arguments");
        }
        return std::make_shared<syntax::NominalTypeSyntax>(
                spanFrom(start), std::move(name), std::move(arguments));
    }

    if (cursor.eat(leftSquare) != nullptr) {
        auto first = parseType();
        if (cursor.eat(colon) != nullptr) {
            auto second = parseType();
            expect(rightSquare, "']' to close a dictionary type");
            return std::make_shared<syntax::DictionaryTypeSyntax>(
                    spanFrom(start), std::move(first), std::move(second));
        }
        expect(rightSquare, "']' to close an array type");
        return std::make_shared<syntax::ArrayTypeSyntax>(spanFrom(start), std::move(first));
    }

    return nullptr;
}

syntax::BlockExprSyntax::Ptr Parser::parseBlock() {
    const size_t start = cursor.position();
    if (cursor.eat(leftCurly) == nullptr) {
        reportExpected(DiagnosticId::expectedBlock, "a '{ ... }' block");
        return std::make_shared<syntax::BlockExprSyntax>(
                insertionSpan(), std::vector<syntax::NodePtr> {});
    }

    std::vector<syntax::NodePtr> items;
    while (!cursor.at(rightCurly) && !cursor.atEnd()) {
        if (cursor.peek()->startsLine &&
            (cursor.at(kwFunc) || cursor.at(kwStruct) || cursor.at(kwEnum))) {
            break;
        }
        const size_t before = cursor.position();
        auto item = parseBlockItem();
        if (item != nullptr) items.push_back(std::move(item));
        if (cursor.position() == before) {
            report(DiagnosticId::unexpectedToken,
                   tokenSpan(cursor.peek()),
                   "unexpected token " + quotedToken(cursor.peek()) + " in block");
            cursor.advance();
        }
        enforceItemBoundary(false);
    }
    expect(rightCurly, "'}' to close the block");
    return std::make_shared<syntax::BlockExprSyntax>(spanFrom(start), std::move(items));
}

syntax::NodePtr Parser::parseBlockItem() {
    if (cursor.at(invalid) || cursor.at(deferredKeyword)) {
        const auto token = cursor.advance();
        return std::make_shared<syntax::ErrorExprSyntax>(tokenSpan(token));
    }
    if (cursor.at(kwLet) || cursor.at(kwVar)) {
        return parseBindingDecl(false);
    }
    if (cursor.at(kwWhile)) {
        return parseWhileStmt();
    }
    if (isExpressionStart(cursor.peek())) {
        return parseExpression();
    }

    reportExpected(DiagnosticId::expectedExpression, "a block item");
    const size_t start = cursor.position();
    if (!cursor.atEnd() && !cursor.at(rightCurly)) cursor.advance();
    return std::make_shared<syntax::ErrorExprSyntax>(spanFrom(start));
}

syntax::WhileStmtSyntax::Ptr Parser::parseWhileStmt() {
    const size_t start = cursor.position();
    cursor.advance();
    auto condition = parseExpression();
    if (condition == nullptr) {
        reportExpected(DiagnosticId::expectedExpression, "a while condition");
        condition = std::make_shared<syntax::ErrorExprSyntax>(insertionSpan());
    }
    auto body = parseBlock();
    return std::make_shared<syntax::WhileStmtSyntax>(
            spanFrom(start), std::move(condition), std::move(body));
}

syntax::ExprPtr Parser::parseExpression() {
    if (cursor.at(kwReturn)) {
        return parseReturnExpr();
    }
    return parsePrecedence(1);
}

syntax::ExprPtr Parser::parsePrecedence(int minimumBindingPower) {
    auto left = parsePrefixExpr();
    if (left == nullptr) return nullptr;

    int consumedNonAssociativePower = 0;
    while (!cursor.atEnd() && !cursor.peek()->startsLine && isInfix(cursor.peek()->kind)) {
        const InfixInfo info = infixInfo(cursor.peek()->kind);
        if (info.bindingPower < minimumBindingPower) break;

        if (info.nonAssociative && consumedNonAssociativePower == info.bindingPower) {
            report(DiagnosticId::chainedComparison,
                   tokenSpan(cursor.peek()),
                   "comparison and equality operators do not chain");
        }
        if (info.nonAssociative) consumedNonAssociativePower = info.bindingPower;

        auto op = cursor.advance();
        syntax::ExprPtr right;
        if (info.assignment) {
            right = parseExpression();
        } else {
            const int rightBindingPower = info.rightAssociative
                    ? info.bindingPower
                    : info.bindingPower + 1;
            right = parsePrecedence(rightBindingPower);
        }
        if (right == nullptr) {
            reportExpected(DiagnosticId::expectedExpression,
                           "an expression after " + quotedToken(op));
            right = std::make_shared<syntax::ErrorExprSyntax>(insertionSpan());
        }

        const SourceSpan span = coveringSpan(left->span, right->span);
        if (info.assignment) {
            if (!isAssignmentTarget(left)) {
                report(DiagnosticId::invalidAssignmentTarget,
                       left->span,
                       "the left side of assignment is not a writable syntax path");
            }
            left = std::make_shared<syntax::AssignmentExprSyntax>(
                    span, std::move(op), std::move(left), std::move(right));
        } else {
            left = std::make_shared<syntax::BinaryExprSyntax>(
                    span, std::move(op), std::move(left), std::move(right));
        }
    }
    return left;
}

syntax::ExprPtr Parser::parsePrefixExpr() {
    const size_t start = cursor.position();
    if (cursor.at(minus)) {
        auto op = cursor.advance();
        auto operand = parsePrefixExpr();
        if (operand == nullptr) {
            reportExpected(DiagnosticId::expectedExpression, "an operand after '-'");
            operand = std::make_shared<syntax::ErrorExprSyntax>(insertionSpan());
        }
        return std::make_shared<syntax::PrefixExprSyntax>(
                spanFrom(start), std::move(op), std::move(operand));
    }
    if (cursor.at(ampersand) || cursor.at(kwConsume)) {
        auto marker = cursor.advance();
        auto operand = parsePostfixExpr();
        if (operand == nullptr) {
                reportExpected(
                    DiagnosticId::expectedExpression,
                    marker->kind == kwConsume
                        ? "an owned expression after 'consume'"
                        : "an access path after '&'");
            operand = std::make_shared<syntax::ErrorExprSyntax>(insertionSpan());
        }
        return std::make_shared<syntax::AccessExprSyntax>(
                spanFrom(start), std::move(marker), std::move(operand));
    }
    return parsePostfixExpr();
}

syntax::ExprPtr Parser::parsePostfixExpr() {
    auto expression = parsePrimaryExpr();
    if (expression == nullptr) return nullptr;

    while (!cursor.atEnd() && !cursor.peek()->startsLine) {
        if (cursor.eat(dot) != nullptr) {
            auto member = expect(identifier, "a member name after '.'");
            expression = std::make_shared<syntax::MemberExprSyntax>(
                    coveringSpan(expression->span, tokenSpan(member)),
                    std::move(expression),
                    std::move(member));
            continue;
        }

        if (cursor.at(leftParen)) {
            auto arguments = parseArgumentClause();
            expression = std::make_shared<syntax::CallExprSyntax>(
                    coveringSpan(expression->span, tokenSpan(cursor.previous())),
                    std::move(expression),
                    std::move(arguments));
            continue;
        }

        if (cursor.eat(leftSquare) != nullptr) {
            auto index = parseExpression();
            if (index == nullptr) {
                reportExpected(DiagnosticId::expectedExpression, "a subscript index");
                index = std::make_shared<syntax::ErrorExprSyntax>(insertionSpan());
            }
            auto close = expect(rightSquare, "']' to close the subscript");
            expression = std::make_shared<syntax::SubscriptExprSyntax>(
                    coveringSpan(expression->span, tokenSpan(close)),
                    std::move(expression),
                    std::move(index));
            continue;
        }
        break;
    }
    return expression;
}

syntax::ExprPtr Parser::parsePrimaryExpr() {
    const size_t start = cursor.position();
    if (isLiteral(cursor.peek()->kind)) {
        auto literal = cursor.advance();
        return std::make_shared<syntax::LiteralExprSyntax>(tokenSpan(literal), std::move(literal));
    }
    if (cursor.at(identifier)) {
        auto name = cursor.advance();
        return std::make_shared<syntax::NameExprSyntax>(tokenSpan(name), std::move(name));
    }
    if (cursor.at(leftParen)) return parseParenthesizedExpr();
    if (cursor.at(leftSquare)) return parseArrayOrDictionaryExpr();
    if (cursor.at(kwIf)) return parseIfExpr();
    if (cursor.at(kwMatch)) return parseMatchExpr();

    if (cursor.eat(dot) != nullptr) {
        auto name = expect(identifier, "an enum case name after '.'");
        bool hasPayloadClause = false;
        std::vector<syntax::CallArgumentSyntax::Ptr> arguments;
        if (cursor.at(leftParen)) {
            hasPayloadClause = true;
            arguments = parseArgumentClause();
            if (arguments.empty()) {
                report(DiagnosticId::missingListElement,
                       tokenSpan(cursor.previous()),
                       "a contextual enum payload clause cannot be empty");
            }
        }
        return std::make_shared<syntax::ContextualCaseExprSyntax>(
                spanFrom(start),
                std::move(name),
                hasPayloadClause,
                std::move(arguments));
    }

    return nullptr;
}

syntax::ExprPtr Parser::parseParenthesizedExpr() {
    const size_t start = cursor.position();
    cursor.advance();
    auto expression = parseExpression();
    if (expression == nullptr) {
        reportExpected(DiagnosticId::expectedExpression, "an expression inside parentheses");
        expression = std::make_shared<syntax::ErrorExprSyntax>(insertionSpan());
    }
    expect(rightParen, "')' to close the parenthesized expression");
    return std::make_shared<syntax::ParenthesizedExprSyntax>(
            spanFrom(start), std::move(expression));
}

syntax::ExprPtr Parser::parseArrayOrDictionaryExpr() {
    const size_t start = cursor.position();
    cursor.advance();

    if (cursor.eat(colon) != nullptr) {
        expect(rightSquare, "']' to close the empty dictionary");
        return std::make_shared<syntax::DictionaryExprSyntax>(
                spanFrom(start), std::vector<syntax::DictionaryEntrySyntax::Ptr> {});
    }
    if (cursor.eat(rightSquare) != nullptr) {
        return std::make_shared<syntax::ArrayExprSyntax>(
                spanFrom(start), std::vector<syntax::ExprPtr> {});
    }

    auto first = parseExpression();
    if (first == nullptr) {
        reportExpected(DiagnosticId::expectedExpression, "an array element or dictionary key");
        first = std::make_shared<syntax::ErrorExprSyntax>(insertionSpan());
    }

    if (cursor.eat(colon) != nullptr) {
        std::vector<syntax::DictionaryEntrySyntax::Ptr> entries;
        auto value = parseExpression();
        if (value == nullptr) {
            reportExpected(DiagnosticId::expectedExpression, "a dictionary value");
            value = std::make_shared<syntax::ErrorExprSyntax>(insertionSpan());
        }
        entries.push_back(std::make_shared<syntax::DictionaryEntrySyntax>(
                coveringSpan(first->span, value->span), std::move(first), std::move(value)));

        while (cursor.eat(comma) != nullptr) {
            if (cursor.at(rightSquare)) break;
            auto key = parseExpression();
            if (key == nullptr) {
                reportExpected(DiagnosticId::expectedExpression, "a dictionary key");
                key = std::make_shared<syntax::ErrorExprSyntax>(insertionSpan());
            }
            expect(colon, "':' between a dictionary key and value");
            auto itemValue = parseExpression();
            if (itemValue == nullptr) {
                reportExpected(DiagnosticId::expectedExpression, "a dictionary value");
                itemValue = std::make_shared<syntax::ErrorExprSyntax>(insertionSpan());
            }
            entries.push_back(std::make_shared<syntax::DictionaryEntrySyntax>(
                    coveringSpan(key->span, itemValue->span),
                    std::move(key),
                    std::move(itemValue)));
        }
        expect(rightSquare, "']' to close the dictionary literal");
        return std::make_shared<syntax::DictionaryExprSyntax>(
                spanFrom(start), std::move(entries));
    }

    std::vector<syntax::ExprPtr> elements;
    elements.push_back(std::move(first));
    while (cursor.eat(comma) != nullptr) {
        if (cursor.at(rightSquare)) break;
        auto element = parseExpression();
        if (element == nullptr) {
            reportExpected(DiagnosticId::expectedExpression, "an array element");
            element = std::make_shared<syntax::ErrorExprSyntax>(insertionSpan());
        }
        if (cursor.at(colon)) {
            report(DiagnosticId::unexpectedToken,
                   tokenSpan(cursor.peek()),
                   "array and dictionary entries cannot be mixed in one literal");
            cursor.advance();
            parseExpression();
        }
        elements.push_back(std::move(element));
    }
    expect(rightSquare, "']' to close the array literal");
    return std::make_shared<syntax::ArrayExprSyntax>(spanFrom(start), std::move(elements));
}

syntax::IfExprSyntax::Ptr Parser::parseIfExpr() {
    const size_t start = cursor.position();
    cursor.advance();
    auto condition = parseExpression();
    if (condition == nullptr) {
        reportExpected(DiagnosticId::expectedExpression, "an if condition");
        condition = std::make_shared<syntax::ErrorExprSyntax>(insertionSpan());
    }
    auto thenBranch = parseBlock();

    syntax::ExprPtr elseBranch;
    if (cursor.eat(kwElse) != nullptr) {
        if (cursor.at(kwIf)) {
            elseBranch = parseIfExpr();
        } else {
            elseBranch = parseBlock();
        }
    }

    return std::make_shared<syntax::IfExprSyntax>(
            spanFrom(start),
            std::move(condition),
            std::move(thenBranch),
            std::move(elseBranch));
}

syntax::ReturnExprSyntax::Ptr Parser::parseReturnExpr() {
    const size_t start = cursor.position();
    cursor.advance();
    syntax::ExprPtr value;
    if (shouldParseReturnValue()) {
        value = parseExpression();
        if (value == nullptr) {
            reportExpected(DiagnosticId::expectedExpression, "a return value");
            value = std::make_shared<syntax::ErrorExprSyntax>(insertionSpan());
        }
    }
    return std::make_shared<syntax::ReturnExprSyntax>(spanFrom(start), std::move(value));
}

syntax::MatchExprSyntax::Ptr Parser::parseMatchExpr() {
    const size_t start = cursor.position();
    cursor.advance();
    auto scrutinee = parseExpression();
    if (scrutinee == nullptr) {
        reportExpected(DiagnosticId::expectedExpression, "a match scrutinee");
        scrutinee = std::make_shared<syntax::ErrorExprSyntax>(insertionSpan());
    }
    if (cursor.eat(leftCurly) == nullptr) {
        reportExpected(DiagnosticId::expectedBlock, "'{' to begin match arms");
        return std::make_shared<syntax::MatchExprSyntax>(
                spanFrom(start),
                std::move(scrutinee),
                std::vector<syntax::MatchArmSyntax::Ptr> {});
    }

    std::vector<syntax::MatchArmSyntax::Ptr> arms;
    if (cursor.at(rightCurly)) {
        report(DiagnosticId::missingListElement,
               insertionSpan(),
               "a match expression must contain at least one arm");
    }
    while (!cursor.at(rightCurly) && !cursor.atEnd()) {
        if (cursor.peek()->startsLine &&
            (cursor.at(kwLet) || cursor.at(kwVar) || cursor.at(kwWhile) ||
             cursor.at(kwReturn) || cursor.at(kwIf) || cursor.at(kwMatch))) {
            break;
        }
        const size_t before = cursor.position();
        arms.push_back(parseMatchArm());
        if (cursor.position() == before) {
            cursor.advance();
        }
    }
    expect(rightCurly, "'}' to close the match expression");
    return std::make_shared<syntax::MatchExprSyntax>(
            spanFrom(start), std::move(scrutinee), std::move(arms));
}

syntax::CallArgumentSyntax::Ptr Parser::parseCallArgument() {
    const size_t start = cursor.position();
    Token::Ptr label;
    if (cursor.at(identifier) && cursor.at(colon, 1)) {
        label = cursor.advance();
        cursor.advance();
    }
    Token::Ptr accessMarker;
    if (cursor.at(ampersand) || cursor.at(kwConsume)) {
        accessMarker = cursor.advance();
    }
    auto value = parseExpression();
    if (value == nullptr) {
        reportExpected(DiagnosticId::expectedExpression, "a call argument value");
        value = std::make_shared<syntax::ErrorExprSyntax>(insertionSpan());
    }
    return std::make_shared<syntax::CallArgumentSyntax>(
            spanFrom(start), std::move(label), std::move(accessMarker), std::move(value));
}

std::vector<syntax::CallArgumentSyntax::Ptr> Parser::parseArgumentClause() {
    cursor.advance();
    std::vector<syntax::CallArgumentSyntax::Ptr> arguments;
    if (cursor.eat(rightParen) != nullptr) {
        return arguments;
    }

    while (!cursor.at(rightParen) && !cursor.atEnd()) {
        const size_t before = cursor.position();
        arguments.push_back(parseCallArgument());
        if (cursor.position() == before) cursor.advance();

        if (cursor.eat(comma) != nullptr) {
            if (cursor.at(rightParen)) break;
            continue;
        }
        if (cursor.at(rightParen) || cursor.atEnd()) break;
        reportMissingComma("expected ',' between arguments");
        synchronizeList(rightParen);
        if (cursor.eat(comma) == nullptr) break;
    }
    expect(rightParen, "')' to close the argument clause");
    return arguments;
}

syntax::PatternPtr Parser::parsePattern() {
    if (cursor.at(wildcard)) {
        auto token = cursor.advance();
        return std::make_shared<syntax::WildcardPatternSyntax>(tokenSpan(token), std::move(token));
    }
    if (isLiteral(cursor.peek()->kind)) {
        auto literal = cursor.advance();
        return std::make_shared<syntax::LiteralPatternSyntax>(tokenSpan(literal), std::move(literal));
    }
    if (cursor.at(dot) || (cursor.at(identifier) && cursor.at(dot, 1))) {
        return parseEnumCasePattern();
    }
    if (cursor.at(identifier)) {
        auto name = cursor.advance();
        return std::make_shared<syntax::BindingPatternSyntax>(tokenSpan(name), std::move(name));
    }

    reportExpected(DiagnosticId::expectedPattern, "a match pattern");
    const size_t start = cursor.position();
    if (!cursor.at(fatArrow) && !cursor.atEnd()) cursor.advance();
    return std::make_shared<syntax::ErrorPatternSyntax>(spanFrom(start));
}

syntax::EnumCasePatternSyntax::Ptr Parser::parseEnumCasePattern() {
    const size_t start = cursor.position();
    Token::Ptr qualifier;
    if (cursor.at(identifier)) {
        qualifier = cursor.advance();
    }
    expect(dot, "'.' in an enum case pattern");
    auto name = expect(identifier, "an enum case name");

    const bool hasPayloadClause = cursor.eat(leftParen) != nullptr;
    std::vector<syntax::PatternArgumentSyntax::Ptr> arguments;
    if (hasPayloadClause) {
        if (cursor.at(rightParen)) {
            report(DiagnosticId::missingListElement,
                   insertionSpan(),
                   "an enum pattern payload clause cannot be empty");
        } else {
            while (!cursor.at(rightParen) && !cursor.atEnd()) {
                const size_t before = cursor.position();
                arguments.push_back(parsePatternArgument());
                if (cursor.position() == before) cursor.advance();
                if (cursor.eat(comma) != nullptr) {
                    if (cursor.at(rightParen)) break;
                    continue;
                }
                if (cursor.at(rightParen) || cursor.atEnd()) break;
                                reportMissingComma("expected ',' between enum payload patterns");
                synchronizeList(rightParen);
                if (cursor.eat(comma) == nullptr) break;
            }
        }
        expect(rightParen, "')' to close the enum case pattern");
    }

    return std::make_shared<syntax::EnumCasePatternSyntax>(
            spanFrom(start),
            std::move(qualifier),
            std::move(name),
            hasPayloadClause,
            std::move(arguments));
}

syntax::PatternArgumentSyntax::Ptr Parser::parsePatternArgument() {
    const size_t start = cursor.position();
    Token::Ptr label;
    if (cursor.at(identifier) && cursor.at(colon, 1)) {
        label = cursor.advance();
        cursor.advance();
    }
    auto pattern = parsePattern();
    return std::make_shared<syntax::PatternArgumentSyntax>(
            spanFrom(start), std::move(label), std::move(pattern));
}

syntax::MatchArmSyntax::Ptr Parser::parseMatchArm() {
    const size_t start = cursor.position();
    auto pattern = parsePattern();
    expect(fatArrow, "'=>' after the match pattern");

    syntax::ExprPtr body;
    if (cursor.at(leftCurly)) {
        body = parseBlock();
    } else {
        body = parseExpression();
    }
    if (body == nullptr) {
        reportExpected(DiagnosticId::expectedExpression, "a match arm body");
        body = std::make_shared<syntax::ErrorExprSyntax>(insertionSpan());
    }

    if (cursor.eat(comma) == nullptr &&
        !cursor.at(rightCurly) && !cursor.atEnd() &&
        !cursor.peek()->startsLine) {
        reportMissingComma("expected ',' or a newline after the match arm");
        synchronizeMatchArm();
        cursor.eat(comma);
    }

    return std::make_shared<syntax::MatchArmSyntax>(
            spanFrom(start), std::move(pattern), std::move(body));
}

bool Parser::isLiteral(TokenKind kind) const {
    return kind == decimalLiteral || kind == stringLiteral ||
           kind == byteLiteral || kind == booleanLiteral || kind == nilLiteral;
}

bool Parser::isExpressionStart(const Token::Ptr& token) const {
    return token != nullptr &&
           (isLiteral(token->kind) || token->kind == identifier ||
            token->kind == leftParen || token->kind == leftSquare ||
            token->kind == dot || token->kind == kwIf ||
            token->kind == kwMatch || token->kind == kwReturn ||
            token->kind == minus || token->kind == ampersand);
}

bool Parser::isTopLevelStart(const Token::Ptr& token) const {
    if (token == nullptr) return false;
    return token->kind == kwLet || token->kind == kwVar ||
           token->kind == kwFunc || token->kind == kwStruct ||
           token->kind == kwEnum;
}

bool Parser::isBlockItemStart(const Token::Ptr& token) const {
    return token != nullptr &&
           (token->kind == kwLet || token->kind == kwVar ||
            token->kind == kwWhile || isExpressionStart(token));
}

bool Parser::isAssignmentTarget(const syntax::ExprPtr& expression) const {
    if (expression == nullptr) return false;
    switch (expression->kind) {
        case syntax::Kind::nameExpr:
        case syntax::Kind::memberExpr:
        case syntax::Kind::subscriptExpr:
            return true;
        case syntax::Kind::accessExpr:
            return isAssignmentTarget(
                    std::static_pointer_cast<syntax::AccessExprSyntax>(expression)->operand);
        default:
            return false;
    }
}

bool Parser::shouldParseReturnValue() const {
    if (cursor.atEnd() || cursor.at(rightCurly) || cursor.at(comma)) return false;
    return !cursor.peek()->startsLine && isExpressionStart(cursor.peek());
}

Parser::InfixInfo Parser::infixInfo(TokenKind kind) const {
    switch (kind) {
        case equal: return {1, true, false, true};
        case andAnd: return {2, false, false, false};
        case equalEqual:
        case notEqual: return {3, false, true, false};
        case less:
        case lessEqual:
        case greater:
        case greaterEqual: return {4, false, true, false};
        case plus:
        case minus: return {5, false, false, false};
        case multiply: return {6, false, false, false};
        default: return {0, false, false, false};
    }
}

bool Parser::isInfix(TokenKind kind) const {
    return infixInfo(kind).bindingPower != 0;
}

Token::Ptr Parser::expect(TokenKind kind, const std::string& spelling) {
    if (auto token = cursor.eat(kind)) {
        return token;
    }
    reportExpected(DiagnosticId::expectedToken, spelling);
    addExpectedTokenFixIt(kind);
    return synthetic(kind);
}

Token::Ptr Parser::synthetic(TokenKind kind) const {
    const auto current = cursor.peek();
    return std::make_shared<Token>(
            kind,
            "",
            insertionSpan(),
            current->lineNumber,
            current->columnAt,
            current->startsLine);
}

void Parser::report(DiagnosticId id, SourceSpan span, std::string message) {
    diagnostics.push_back(Diagnostic { id, span, std::move(message) });
}

void Parser::addExpectedTokenFixIt(TokenKind kind) {
    const auto spelling = canonicalTokenSpelling(kind);
    if (!spelling.has_value() || diagnostics.empty()) return;
    const auto insertion = insertionSpan();
    auto& diagnostic = diagnostics.back();
    diagnostic.help = cursor.atEnd()
            ? "insert '" + *spelling + "' at end of file"
            : "insert '" + *spelling + "' before this token";
    diagnostic.fixIt = DiagnosticFixIt { insertion.offset, 0, *spelling };
}

void Parser::reportMissingComma(std::string message) {
    report(DiagnosticId::missingComma, insertionSpan(), std::move(message));
    addExpectedTokenFixIt(comma);
}

void Parser::reportExpected(DiagnosticId id, const std::string& expected) {
    const auto token = cursor.peek();
    if (token->kind == endOfFile) {
        report(DiagnosticId::unexpectedEndOfFile,
               insertionSpan(),
               "unexpected end of file; expected " + expected);
    } else {
        report(id,
               tokenSpan(token),
               "expected " + expected + ", found " + quotedToken(token));
    }
}

void Parser::enforceItemBoundary(bool topLevel) {
    if (cursor.atEnd() || cursor.at(rightCurly) || cursor.peek()->startsLine) return;
    report(DiagnosticId::sameLineItems,
           insertionSpan(),
           "adjacent source or block items must begin on separate lines");
    if (topLevel) synchronizeTopLevel();
    else synchronizeBlock();
}

void Parser::synchronizeTopLevel() {
    while (!cursor.atEnd()) {
        if (cursor.peek()->startsLine && isTopLevelStart(cursor.peek())) return;
        cursor.advance();
    }
}

void Parser::synchronizeBlock() {
    while (!cursor.atEnd() && !cursor.at(rightCurly)) {
        if (cursor.peek()->startsLine && isBlockItemStart(cursor.peek())) return;
        cursor.advance();
    }
}

void Parser::synchronizeList(TokenKind closingKind) {
    while (!cursor.atEnd() && !cursor.at(closingKind) && !cursor.at(comma)) {
        if (cursor.at(rightParen) || cursor.at(rightSquare) ||
            cursor.at(rightCurly) || cursor.at(greater)) {
            return;
        }
        if (cursor.peek()->startsLine &&
            (isTopLevelStart(cursor.peek()) || isBlockItemStart(cursor.peek()))) {
            return;
        }
        cursor.advance();
    }
}

void Parser::synchronizeMatchArm() {
    while (!cursor.atEnd() && !cursor.at(rightCurly) && !cursor.at(comma)) {
        if (cursor.peek()->startsLine &&
            (cursor.at(wildcard) || cursor.at(dot) || cursor.at(identifier) ||
             isLiteral(cursor.peek()->kind))) {
            return;
        }
        cursor.advance();
    }
}

SourceSpan Parser::spanFrom(size_t start) const {
    if (cursor.position() <= start) {
        return SourceSpan { cursor.tokenAt(start)->span.offset, 0 };
    }
    const auto first = cursor.tokenAt(start);
    const auto last = cursor.previous();
    const uint32_t end = spanEnd(last->span);
    return SourceSpan { first->span.offset, end - first->span.offset };
}

SourceSpan Parser::insertionSpan() const {
    return SourceSpan { cursor.peek()->span.offset, 0 };
}

SourceSpan Parser::tokenSpan(const Token::Ptr& token) const {
    return token == nullptr ? insertionSpan() : token->span;
}

} // namespace joyeer::parser
