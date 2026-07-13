#include "joyeer/compiler/syntax.h"

#include <iomanip>
#include <sstream>
#include <utility>

namespace joyeer::syntax {

const char* kindName(Kind kind) {
    switch (kind) {
        case Kind::sourceFile: return "source_file";
        case Kind::errorDecl: return "error_decl";
        case Kind::bindingDecl: return "binding_decl";
        case Kind::functionDecl: return "function_decl";
        case Kind::parameterDecl: return "parameter_decl";
        case Kind::structDecl: return "struct_decl";
        case Kind::structFieldDecl: return "struct_field_decl";
        case Kind::enumDecl: return "enum_decl";
        case Kind::enumCaseDecl: return "enum_case_decl";
        case Kind::associatedType: return "associated_type";
        case Kind::errorType: return "error_type";
        case Kind::nominalType: return "nominal_type";
        case Kind::arrayType: return "array_type";
        case Kind::dictionaryType: return "dictionary_type";
        case Kind::optionalType: return "optional_type";
        case Kind::errorExpr: return "error_expr";
        case Kind::nameExpr: return "name_expr";
        case Kind::literalExpr: return "literal_expr";
        case Kind::parenthesizedExpr: return "parenthesized_expr";
        case Kind::prefixExpr: return "prefix_expr";
        case Kind::accessExpr: return "access_expr";
        case Kind::binaryExpr: return "binary_expr";
        case Kind::assignmentExpr: return "assignment_expr";
        case Kind::memberExpr: return "member_expr";
        case Kind::callExpr: return "call_expr";
        case Kind::callArgument: return "call_argument";
        case Kind::subscriptExpr: return "subscript_expr";
        case Kind::arrayExpr: return "array_expr";
        case Kind::dictionaryExpr: return "dictionary_expr";
        case Kind::dictionaryEntry: return "dictionary_entry";
        case Kind::contextualCaseExpr: return "contextual_case_expr";
        case Kind::blockExpr: return "block_expr";
        case Kind::whileStmt: return "while_stmt";
        case Kind::ifExpr: return "if_expr";
        case Kind::returnExpr: return "return_expr";
        case Kind::matchExpr: return "match_expr";
        case Kind::matchArm: return "match_arm";
        case Kind::errorPattern: return "error_pattern";
        case Kind::wildcardPattern: return "wildcard_pattern";
        case Kind::literalPattern: return "literal_pattern";
        case Kind::bindingPattern: return "binding_pattern";
        case Kind::enumCasePattern: return "enum_case_pattern";
        case Kind::patternArgument: return "pattern_argument";
    }
    return "unknown";
}

namespace {

std::string tokenText(const Token::Ptr& token) {
    return token == nullptr ? "<missing>" : token->rawValue;
}

void appendHeader(std::ostringstream& out, const NodePtr& node, size_t depth) {
    out << std::string(depth * 2, ' ')
        << kindName(node->kind)
        << '@' << node->span.offset << ':' << node->span.length;
}

void appendNode(std::ostringstream& out, const NodePtr& node, size_t depth) {
    if (node == nullptr) {
        out << std::string(depth * 2, ' ') << "<none>\n";
        return;
    }

    appendHeader(out, node, depth);

    switch (node->kind) {
        case Kind::sourceFile: {
            const auto value = std::static_pointer_cast<SourceFileSyntax>(node);
            out << '\n';
            for (const auto& item : value->items) appendNode(out, item, depth + 1);
            return;
        }
        case Kind::errorDecl:
        case Kind::errorType:
        case Kind::errorExpr:
        case Kind::errorPattern:
            out << '\n';
            return;
        case Kind::bindingDecl: {
            const auto value = std::static_pointer_cast<BindingDeclSyntax>(node);
            out << " keyword=" << tokenText(value->bindingKeyword)
                << " name=" << tokenText(value->name) << '\n';
            if (value->annotation != nullptr) appendNode(out, value->annotation, depth + 1);
            if (value->initializer != nullptr) appendNode(out, value->initializer, depth + 1);
            return;
        }
        case Kind::functionDecl: {
            const auto value = std::static_pointer_cast<FunctionDeclSyntax>(node);
            out << " name=" << tokenText(value->name) << '\n';
            for (const auto& parameter : value->parameters) appendNode(out, parameter, depth + 1);
            if (value->returnType != nullptr) appendNode(out, value->returnType, depth + 1);
            appendNode(out, value->body, depth + 1);
            return;
        }
        case Kind::parameterDecl: {
            const auto value = std::static_pointer_cast<ParameterDeclSyntax>(node);
            out << " label=" << tokenText(value->label)
                << " name=" << tokenText(value->name)
                << " access=" << (value->inoutKeyword == nullptr ? "borrowing" : "inout")
                << '\n';
            appendNode(out, value->type, depth + 1);
            return;
        }
        case Kind::structDecl: {
            const auto value = std::static_pointer_cast<StructDeclSyntax>(node);
            out << " name=" << tokenText(value->name) << '\n';
            for (const auto& field : value->fields) appendNode(out, field, depth + 1);
            return;
        }
        case Kind::structFieldDecl: {
            const auto value = std::static_pointer_cast<StructFieldDeclSyntax>(node);
            out << " keyword=" << tokenText(value->bindingKeyword)
                << " name=" << tokenText(value->name) << '\n';
            appendNode(out, value->type, depth + 1);
            if (value->initializer != nullptr) appendNode(out, value->initializer, depth + 1);
            return;
        }
        case Kind::enumDecl: {
            const auto value = std::static_pointer_cast<EnumDeclSyntax>(node);
            out << " name=" << tokenText(value->name) << '\n';
            for (const auto& enumCase : value->cases) appendNode(out, enumCase, depth + 1);
            return;
        }
        case Kind::enumCaseDecl: {
            const auto value = std::static_pointer_cast<EnumCaseDeclSyntax>(node);
            out << " name=" << tokenText(value->name)
                << " payload=" << (value->hasPayloadClause ? "yes" : "no") << '\n';
            for (const auto& associated : value->associatedTypes) {
                appendNode(out, associated, depth + 1);
            }
            return;
        }
        case Kind::associatedType: {
            const auto value = std::static_pointer_cast<AssociatedTypeSyntax>(node);
            out << " label=" << tokenText(value->label) << '\n';
            appendNode(out, value->type, depth + 1);
            return;
        }
        case Kind::nominalType: {
            const auto value = std::static_pointer_cast<NominalTypeSyntax>(node);
            out << " name=" << tokenText(value->name) << '\n';
            for (const auto& argument : value->arguments) appendNode(out, argument, depth + 1);
            return;
        }
        case Kind::arrayType: {
            const auto value = std::static_pointer_cast<ArrayTypeSyntax>(node);
            out << '\n';
            appendNode(out, value->element, depth + 1);
            return;
        }
        case Kind::dictionaryType: {
            const auto value = std::static_pointer_cast<DictionaryTypeSyntax>(node);
            out << '\n';
            appendNode(out, value->key, depth + 1);
            appendNode(out, value->value, depth + 1);
            return;
        }
        case Kind::optionalType: {
            const auto value = std::static_pointer_cast<OptionalTypeSyntax>(node);
            out << '\n';
            appendNode(out, value->wrapped, depth + 1);
            return;
        }
        case Kind::nameExpr: {
            const auto value = std::static_pointer_cast<NameExprSyntax>(node);
            out << " name=" << tokenText(value->name) << '\n';
            return;
        }
        case Kind::literalExpr: {
            const auto value = std::static_pointer_cast<LiteralExprSyntax>(node);
            out << " value=" << std::quoted(tokenText(value->literal)) << '\n';
            return;
        }
        case Kind::parenthesizedExpr: {
            const auto value = std::static_pointer_cast<ParenthesizedExprSyntax>(node);
            out << '\n';
            appendNode(out, value->expression, depth + 1);
            return;
        }
        case Kind::prefixExpr: {
            const auto value = std::static_pointer_cast<PrefixExprSyntax>(node);
            out << " op=" << tokenText(value->op) << '\n';
            appendNode(out, value->operand, depth + 1);
            return;
        }
        case Kind::accessExpr: {
            const auto value = std::static_pointer_cast<AccessExprSyntax>(node);
            out << " marker=" << tokenText(value->marker) << '\n';
            appendNode(out, value->operand, depth + 1);
            return;
        }
        case Kind::binaryExpr: {
            const auto value = std::static_pointer_cast<BinaryExprSyntax>(node);
            out << " op=" << tokenText(value->op) << '\n';
            appendNode(out, value->left, depth + 1);
            appendNode(out, value->right, depth + 1);
            return;
        }
        case Kind::assignmentExpr: {
            const auto value = std::static_pointer_cast<AssignmentExprSyntax>(node);
            out << " op=" << tokenText(value->op) << '\n';
            appendNode(out, value->target, depth + 1);
            appendNode(out, value->value, depth + 1);
            return;
        }
        case Kind::memberExpr: {
            const auto value = std::static_pointer_cast<MemberExprSyntax>(node);
            out << " member=" << tokenText(value->member) << '\n';
            appendNode(out, value->base, depth + 1);
            return;
        }
        case Kind::callExpr: {
            const auto value = std::static_pointer_cast<CallExprSyntax>(node);
            out << '\n';
            appendNode(out, value->callee, depth + 1);
            for (const auto& argument : value->arguments) appendNode(out, argument, depth + 1);
            return;
        }
        case Kind::callArgument: {
            const auto value = std::static_pointer_cast<CallArgumentSyntax>(node);
            out << " label=" << tokenText(value->label)
                << " access=" << tokenText(value->accessMarker) << '\n';
            appendNode(out, value->value, depth + 1);
            return;
        }
        case Kind::subscriptExpr: {
            const auto value = std::static_pointer_cast<SubscriptExprSyntax>(node);
            out << '\n';
            appendNode(out, value->base, depth + 1);
            appendNode(out, value->index, depth + 1);
            return;
        }
        case Kind::arrayExpr: {
            const auto value = std::static_pointer_cast<ArrayExprSyntax>(node);
            out << '\n';
            for (const auto& element : value->elements) appendNode(out, element, depth + 1);
            return;
        }
        case Kind::dictionaryExpr: {
            const auto value = std::static_pointer_cast<DictionaryExprSyntax>(node);
            out << '\n';
            for (const auto& entry : value->entries) appendNode(out, entry, depth + 1);
            return;
        }
        case Kind::dictionaryEntry: {
            const auto value = std::static_pointer_cast<DictionaryEntrySyntax>(node);
            out << '\n';
            appendNode(out, value->key, depth + 1);
            appendNode(out, value->value, depth + 1);
            return;
        }
        case Kind::contextualCaseExpr: {
            const auto value = std::static_pointer_cast<ContextualCaseExprSyntax>(node);
            out << " name=" << tokenText(value->name)
                << " payload=" << (value->hasPayloadClause ? "yes" : "no") << '\n';
            for (const auto& argument : value->arguments) appendNode(out, argument, depth + 1);
            return;
        }
        case Kind::blockExpr: {
            const auto value = std::static_pointer_cast<BlockExprSyntax>(node);
            out << '\n';
            for (const auto& item : value->items) appendNode(out, item, depth + 1);
            return;
        }
        case Kind::whileStmt: {
            const auto value = std::static_pointer_cast<WhileStmtSyntax>(node);
            out << '\n';
            appendNode(out, value->condition, depth + 1);
            appendNode(out, value->body, depth + 1);
            return;
        }
        case Kind::ifExpr: {
            const auto value = std::static_pointer_cast<IfExprSyntax>(node);
            out << '\n';
            appendNode(out, value->condition, depth + 1);
            appendNode(out, value->thenBranch, depth + 1);
            if (value->elseBranch != nullptr) appendNode(out, value->elseBranch, depth + 1);
            return;
        }
        case Kind::returnExpr: {
            const auto value = std::static_pointer_cast<ReturnExprSyntax>(node);
            out << '\n';
            if (value->value != nullptr) appendNode(out, value->value, depth + 1);
            return;
        }
        case Kind::matchExpr: {
            const auto value = std::static_pointer_cast<MatchExprSyntax>(node);
            out << '\n';
            appendNode(out, value->scrutinee, depth + 1);
            for (const auto& arm : value->arms) appendNode(out, arm, depth + 1);
            return;
        }
        case Kind::matchArm: {
            const auto value = std::static_pointer_cast<MatchArmSyntax>(node);
            out << '\n';
            appendNode(out, value->pattern, depth + 1);
            appendNode(out, value->body, depth + 1);
            return;
        }
        case Kind::wildcardPattern:
            out << '\n';
            return;
        case Kind::literalPattern: {
            const auto value = std::static_pointer_cast<LiteralPatternSyntax>(node);
            out << " value=" << std::quoted(tokenText(value->literal)) << '\n';
            return;
        }
        case Kind::bindingPattern: {
            const auto value = std::static_pointer_cast<BindingPatternSyntax>(node);
            out << " name=" << tokenText(value->name) << '\n';
            return;
        }
        case Kind::enumCasePattern: {
            const auto value = std::static_pointer_cast<EnumCasePatternSyntax>(node);
            out << " qualifier=" << tokenText(value->qualifier)
                << " name=" << tokenText(value->name)
                << " payload=" << (value->hasPayloadClause ? "yes" : "no") << '\n';
            for (const auto& argument : value->arguments) appendNode(out, argument, depth + 1);
            return;
        }
        case Kind::patternArgument: {
            const auto value = std::static_pointer_cast<PatternArgumentSyntax>(node);
            out << " label=" << tokenText(value->label) << '\n';
            appendNode(out, value->pattern, depth + 1);
            return;
        }
    }
}

} // namespace

std::string dump(const NodePtr& node) {
    std::ostringstream out;
    appendNode(out, node, 0);
    return out.str();
}

} // namespace joyeer::syntax
