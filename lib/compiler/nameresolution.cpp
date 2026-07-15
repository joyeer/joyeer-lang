#include "joyeer/compiler/nameresolution.h"

#include <algorithm>
#include <cassert>
#include <sstream>
#include <utility>

namespace joyeer::semantic {

namespace {

SourceSpan tokenSpan(const Token::Ptr& token, SourceSpan fallback = {}) {
    return token == nullptr ? fallback : token->span;
}

std::string tokenText(const Token::Ptr& token) {
    return token == nullptr ? std::string() : token->rawValue;
}

bool isTypeSymbol(SymbolKind kind) {
    return kind == SymbolKind::builtinType ||
           kind == SymbolKind::structure ||
           kind == SymbolKind::enumeration;
}

bool isEnumCaseSymbol(SymbolKind kind) {
    return kind == SymbolKind::builtinEnumCase || kind == SymbolKind::enumCase;
}

} // namespace

class NameResolutionBuilder {
public:
    NameResolutionResult build(const syntax::SourceFileSyntax::Ptr& root) {
        model = SemanticModel::Ptr(new SemanticModel(root));
        if (root == nullptr) {
            return NameResolutionResult { model, {} };
        }

        indexNode(root);
        buildPrelude();
        model->fileScope_ = createScope(
                ScopeKind::file,
                model->preludeScope_,
                nodeId(root));
        setContainingScope(root, model->fileScope_);
        model->introducedScopes_[nodeId(root)] = model->fileScope_;

        collectTopLevelDeclarations(root);
        for (const auto& item : root->items) {
            resolveTopLevelSignature(item);
        }
        for (const auto& item : root->items) {
            resolveTopLevelBody(item);
        }

        return NameResolutionResult { model, std::move(diagnostics) };
    }

private:
    SemanticModel::Ptr model;
    std::vector<NameResolutionDiagnostic> diagnostics;

    NodeId nodeId(const syntax::NodePtr& node) const {
        assert(node != nullptr);
        const auto found = model->nodeIds_.find(node.get());
        assert(found != model->nodeIds_.end());
        return found->second;
    }

    Symbol& symbol(SymbolId id) {
        assert(id < model->symbols_.size());
        return model->symbols_[id];
    }

    const Symbol& symbol(SymbolId id) const {
        assert(id < model->symbols_.size());
        return model->symbols_[id];
    }

    Scope& scope(ScopeId id) {
        assert(id < model->scopes_.size());
        return model->scopes_[id];
    }

    const Scope& scope(ScopeId id) const {
        assert(id < model->scopes_.size());
        return model->scopes_[id];
    }

    void report(NameResolutionDiagnosticId id, SourceSpan span, std::string message) {
        diagnostics.push_back(NameResolutionDiagnostic { id, span, std::move(message) });
    }

    void indexNode(const syntax::NodePtr& node) {
        if (node == nullptr || model->nodeIds_.contains(node.get())) return;

        const auto id = static_cast<NodeId>(model->nodes_.size());
        model->nodeIds_[node.get()] = id;
        model->nodes_.push_back(node);

        using syntax::Kind;
        switch (node->kind) {
            case Kind::sourceFile: {
                const auto value = std::static_pointer_cast<syntax::SourceFileSyntax>(node);
                for (const auto& item : value->items) indexNode(item);
                break;
            }
            case Kind::bindingDecl: {
                const auto value = std::static_pointer_cast<syntax::BindingDeclSyntax>(node);
                indexNode(value->annotation);
                indexNode(value->initializer);
                break;
            }
            case Kind::functionDecl: {
                const auto value = std::static_pointer_cast<syntax::FunctionDeclSyntax>(node);
                for (const auto& parameter : value->parameters) indexNode(parameter);
                indexNode(value->returnType);
                indexNode(value->body);
                break;
            }
            case Kind::parameterDecl: {
                const auto value = std::static_pointer_cast<syntax::ParameterDeclSyntax>(node);
                indexNode(value->type);
                break;
            }
            case Kind::structDecl: {
                const auto value = std::static_pointer_cast<syntax::StructDeclSyntax>(node);
                for (const auto& field : value->fields) indexNode(field);
                break;
            }
            case Kind::structFieldDecl: {
                const auto value = std::static_pointer_cast<syntax::StructFieldDeclSyntax>(node);
                indexNode(value->type);
                indexNode(value->initializer);
                break;
            }
            case Kind::enumDecl: {
                const auto value = std::static_pointer_cast<syntax::EnumDeclSyntax>(node);
                for (const auto& enumCase : value->cases) indexNode(enumCase);
                break;
            }
            case Kind::enumCaseDecl: {
                const auto value = std::static_pointer_cast<syntax::EnumCaseDeclSyntax>(node);
                for (const auto& associatedType : value->associatedTypes) indexNode(associatedType);
                break;
            }
            case Kind::associatedType: {
                const auto value = std::static_pointer_cast<syntax::AssociatedTypeSyntax>(node);
                indexNode(value->type);
                break;
            }
            case Kind::nominalType: {
                const auto value = std::static_pointer_cast<syntax::NominalTypeSyntax>(node);
                for (const auto& argument : value->arguments) indexNode(argument);
                break;
            }
            case Kind::arrayType:
                indexNode(std::static_pointer_cast<syntax::ArrayTypeSyntax>(node)->element);
                break;
            case Kind::dictionaryType: {
                const auto value = std::static_pointer_cast<syntax::DictionaryTypeSyntax>(node);
                indexNode(value->key);
                indexNode(value->value);
                break;
            }
            case Kind::optionalType:
                indexNode(std::static_pointer_cast<syntax::OptionalTypeSyntax>(node)->wrapped);
                break;
            case Kind::parenthesizedExpr:
                indexNode(std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(node)->expression);
                break;
            case Kind::prefixExpr:
                indexNode(std::static_pointer_cast<syntax::PrefixExprSyntax>(node)->operand);
                break;
            case Kind::accessExpr:
                indexNode(std::static_pointer_cast<syntax::AccessExprSyntax>(node)->operand);
                break;
            case Kind::binaryExpr: {
                const auto value = std::static_pointer_cast<syntax::BinaryExprSyntax>(node);
                indexNode(value->left);
                indexNode(value->right);
                break;
            }
            case Kind::assignmentExpr: {
                const auto value = std::static_pointer_cast<syntax::AssignmentExprSyntax>(node);
                indexNode(value->target);
                indexNode(value->value);
                break;
            }
            case Kind::memberExpr:
                indexNode(std::static_pointer_cast<syntax::MemberExprSyntax>(node)->base);
                break;
            case Kind::callExpr: {
                const auto value = std::static_pointer_cast<syntax::CallExprSyntax>(node);
                indexNode(value->callee);
                for (const auto& argument : value->arguments) indexNode(argument);
                break;
            }
            case Kind::callArgument:
                indexNode(std::static_pointer_cast<syntax::CallArgumentSyntax>(node)->value);
                break;
            case Kind::subscriptExpr: {
                const auto value = std::static_pointer_cast<syntax::SubscriptExprSyntax>(node);
                indexNode(value->base);
                indexNode(value->index);
                break;
            }
            case Kind::arrayExpr: {
                const auto value = std::static_pointer_cast<syntax::ArrayExprSyntax>(node);
                for (const auto& element : value->elements) indexNode(element);
                break;
            }
            case Kind::dictionaryExpr: {
                const auto value = std::static_pointer_cast<syntax::DictionaryExprSyntax>(node);
                for (const auto& entry : value->entries) indexNode(entry);
                break;
            }
            case Kind::dictionaryEntry: {
                const auto value = std::static_pointer_cast<syntax::DictionaryEntrySyntax>(node);
                indexNode(value->key);
                indexNode(value->value);
                break;
            }
            case Kind::contextualCaseExpr: {
                const auto value = std::static_pointer_cast<syntax::ContextualCaseExprSyntax>(node);
                for (const auto& argument : value->arguments) indexNode(argument);
                break;
            }
            case Kind::blockExpr: {
                const auto value = std::static_pointer_cast<syntax::BlockExprSyntax>(node);
                for (const auto& item : value->items) indexNode(item);
                break;
            }
            case Kind::whileStmt: {
                const auto value = std::static_pointer_cast<syntax::WhileStmtSyntax>(node);
                indexNode(value->condition);
                indexNode(value->body);
                break;
            }
            case Kind::ifExpr: {
                const auto value = std::static_pointer_cast<syntax::IfExprSyntax>(node);
                indexNode(value->condition);
                indexNode(value->thenBranch);
                indexNode(value->elseBranch);
                break;
            }
            case Kind::returnExpr:
                indexNode(std::static_pointer_cast<syntax::ReturnExprSyntax>(node)->value);
                break;
            case Kind::matchExpr: {
                const auto value = std::static_pointer_cast<syntax::MatchExprSyntax>(node);
                indexNode(value->scrutinee);
                for (const auto& arm : value->arms) indexNode(arm);
                break;
            }
            case Kind::matchArm: {
                const auto value = std::static_pointer_cast<syntax::MatchArmSyntax>(node);
                indexNode(value->pattern);
                indexNode(value->body);
                break;
            }
            case Kind::enumCasePattern: {
                const auto value = std::static_pointer_cast<syntax::EnumCasePatternSyntax>(node);
                for (const auto& argument : value->arguments) indexNode(argument);
                break;
            }
            case Kind::patternArgument:
                indexNode(std::static_pointer_cast<syntax::PatternArgumentSyntax>(node)->pattern);
                break;
            case Kind::errorDecl:
            case Kind::errorType:
            case Kind::errorExpr:
            case Kind::errorPattern:
            case Kind::nameExpr:
            case Kind::literalExpr:
            case Kind::wildcardPattern:
            case Kind::literalPattern:
            case Kind::bindingPattern:
                break;
        }
    }

    ScopeId createScope(
            ScopeKind kind,
            std::optional<ScopeId> parent,
            std::optional<NodeId> owner) {
        const auto id = static_cast<ScopeId>(model->scopes_.size());
        model->scopes_.push_back(Scope { id, kind, parent, owner, {}, {} });
        return id;
    }

    SymbolId declareSymbol(
            ScopeId ownerScope,
            SymbolNamespace nameSpace,
            SymbolKind kind,
            std::string name,
            SourceSpan span,
            std::optional<NodeId> declaration = std::nullopt,
            bool insertIntoScope = true,
            std::optional<SymbolId> containingSymbol = std::nullopt) {
        const auto id = static_cast<SymbolId>(model->symbols_.size());
        Symbol value {
            id,
            kind,
            nameSpace,
            std::move(name),
            ownerScope,
            span,
            declaration,
            containingSymbol,
        };

        if (insertIntoScope) {
            auto& owner = scope(ownerScope);
            auto& names = nameSpace == SymbolNamespace::value ? owner.values : owner.types;
            const auto& otherNames = nameSpace == SymbolNamespace::value ? owner.types : owner.values;
            const auto duplicate = names.find(value.name);
            const auto crossNamespaceDuplicate = otherNames.find(value.name);
            if (duplicate != names.end() || crossNamespaceDuplicate != otherNames.end()) {
                value.isInvalid = true;
                report(
                        NameResolutionDiagnosticId::duplicateDeclaration,
                        span,
                        "duplicate declaration of '" + value.name + "' in the same scope");
            } else {
                names.emplace(value.name, id);
            }
        }

        model->symbols_.push_back(std::move(value));
        if (declaration.has_value()) {
            model->declarationSymbols_[*declaration] = id;
        }
        return id;
    }

    SymbolId declareBuiltinType(const std::string& name) {
        const auto id = declareSymbol(
                model->preludeScope_,
                SymbolNamespace::type,
                SymbolKind::builtinType,
                name,
                {});
        const auto members = createScope(ScopeKind::typeMembers, model->preludeScope_, std::nullopt);
        symbol(id).memberScope = members;
        return id;
    }

    SymbolId declareBuiltinMember(
            SymbolId parent,
            SymbolKind kind,
            const std::string& name,
            std::optional<SymbolId> resultType,
            std::optional<CallableSignature> callable = std::nullopt) {
        assert(symbol(parent).memberScope.has_value());
        const auto id = declareSymbol(
                *symbol(parent).memberScope,
                SymbolNamespace::value,
                kind,
                name,
                {},
                std::nullopt,
                true,
                parent);
        symbol(id).declaredType = resultType;
        symbol(id).callable = std::move(callable);
        return id;
    }

    void buildPrelude() {
        model->preludeScope_ = createScope(ScopeKind::prelude, std::nullopt, std::nullopt);

        const auto voidType = declareBuiltinType("Void");
        declareBuiltinType("Never");
        const auto intType = declareBuiltinType("Int");
        declareBuiltinType("Bool");
        const auto stringType = declareBuiltinType("String");
        const auto byteType = declareBuiltinType("UInt8");
        declareBuiltinType("Any");
        const auto arrayType = declareBuiltinType("Array");
        const auto dictionaryType = declareBuiltinType("Dict");
        const auto optionalType = declareBuiltinType("Optional");
        const auto resultType = declareBuiltinType("Result");

        declareBuiltinMember(stringType, SymbolKind::builtinMember, "count", intType);
        declareBuiltinMember(arrayType, SymbolKind::builtinMember, "count", intType);
        declareBuiltinMember(dictionaryType, SymbolKind::builtinMember, "count", intType);
        CallableSignature appendSignature {
            CallableKind::function,
            true,
            { CallableParameter {
                std::string("element"),
                true,
                std::nullopt,
                std::nullopt,
                std::nullopt,
            } },
        };
        const auto append = declareBuiltinMember(
                arrayType,
                SymbolKind::builtinMember,
                "append",
                voidType,
                std::move(appendSignature));
        symbol(append).isMutable = true;

        CallableSignature singlePayload {
            CallableKind::enumCase,
            true,
            { CallableParameter { std::nullopt, true, std::nullopt, std::nullopt, std::nullopt } },
        };
        CallableSignature noPayload { CallableKind::enumCase, false, {} };
        declareBuiltinMember(
                optionalType,
                SymbolKind::builtinEnumCase,
                "Some",
                optionalType,
                singlePayload);
        declareBuiltinMember(
                optionalType,
                SymbolKind::builtinEnumCase,
                "None",
                optionalType,
                noPayload);
        declareBuiltinMember(
                resultType,
                SymbolKind::builtinEnumCase,
                "Ok",
                resultType,
                singlePayload);
        declareBuiltinMember(
                resultType,
                SymbolKind::builtinEnumCase,
                "Err",
                resultType,
                singlePayload);

        CallableSignature printSignature {
            CallableKind::function,
            true,
            { CallableParameter { std::string("value"), true, std::nullopt, std::nullopt, std::nullopt } },
        };
        const auto print = declareSymbol(
                model->preludeScope_,
                SymbolNamespace::value,
                SymbolKind::builtinFunction,
                "print",
                {});
        symbol(print).declaredType = voidType;
        symbol(print).callable = std::move(printSignature);

        CallableSignature readFileSignature {
            CallableKind::function,
            true,
            { CallableParameter {
                std::string("path"),
                true,
                std::nullopt,
                std::nullopt,
                stringType,
            } },
        };
        const auto readFile = declareSymbol(
                model->preludeScope_,
                SymbolNamespace::value,
                SymbolKind::builtinFunction,
                "readFile",
                {});
        symbol(readFile).declaredType = resultType;
        symbol(readFile).callable = std::move(readFileSignature);

        CallableSignature byteConversionSignature {
            CallableKind::function,
            true,
            { CallableParameter {
                std::string("value"),
                true,
                std::nullopt,
                std::nullopt,
                byteType,
            } },
        };
        const auto byteToInt = declareSymbol(
                model->preludeScope_,
                SymbolNamespace::value,
                SymbolKind::builtinFunction,
                "byteToInt",
                {});
        symbol(byteToInt).declaredType = intType;
        symbol(byteToInt).callable = byteConversionSignature;
        const auto byteToString = declareSymbol(
                model->preludeScope_,
                SymbolNamespace::value,
                SymbolKind::builtinFunction,
                "byteToString",
                {});
        symbol(byteToString).declaredType = stringType;
        symbol(byteToString).callable = std::move(byteConversionSignature);
    }

    void collectTopLevelDeclarations(const syntax::SourceFileSyntax::Ptr& root) {
        for (const auto& item : root->items) {
            setContainingScope(item, model->fileScope_);
            switch (item->kind) {
                case syntax::Kind::bindingDecl:
                    collectTopLevelBinding(
                            std::static_pointer_cast<syntax::BindingDeclSyntax>(item));
                    break;
                case syntax::Kind::functionDecl:
                    collectFunction(std::static_pointer_cast<syntax::FunctionDeclSyntax>(item));
                    break;
                case syntax::Kind::structDecl:
                    collectStructure(std::static_pointer_cast<syntax::StructDeclSyntax>(item));
                    break;
                case syntax::Kind::enumDecl:
                    collectEnumeration(std::static_pointer_cast<syntax::EnumDeclSyntax>(item));
                    break;
                default:
                    break;
            }
        }
    }

    void collectTopLevelBinding(const syntax::BindingDeclSyntax::Ptr& declaration) {
        const auto id = declareSymbol(
                model->fileScope_,
                SymbolNamespace::value,
                SymbolKind::binding,
                tokenText(declaration->name),
                tokenSpan(declaration->name, declaration->span),
                nodeId(declaration));
        symbol(id).isMutable = declaration->isMutable();
    }

    void collectFunction(const syntax::FunctionDeclSyntax::Ptr& declaration) {
        CallableSignature signature { CallableKind::function, true, {} };
        for (const auto& parameter : declaration->parameters) {
            signature.parameters.push_back(CallableParameter {
                tokenText(parameter->label),
                true,
                nodeId(parameter),
                nodeId(parameter->type),
                std::nullopt,
                parameter->accessEffect(),
            });
        }

        const auto id = declareSymbol(
                model->fileScope_,
                SymbolNamespace::value,
                SymbolKind::function,
                tokenText(declaration->name),
                tokenSpan(declaration->name, declaration->span),
                nodeId(declaration));
        symbol(id).callable = std::move(signature);
    }

    void collectStructure(const syntax::StructDeclSyntax::Ptr& declaration) {
        const auto type = declareSymbol(
                model->fileScope_,
                SymbolNamespace::type,
                SymbolKind::structure,
                tokenText(declaration->name),
                tokenSpan(declaration->name, declaration->span),
                nodeId(declaration));
        const auto members = createScope(
                ScopeKind::typeMembers,
                model->fileScope_,
                nodeId(declaration));
        symbol(type).memberScope = members;
        model->introducedScopes_[nodeId(declaration)] = members;

        CallableSignature initializerSignature {
            CallableKind::structureInitializer,
            true,
            {},
        };
        for (const auto& field : declaration->fields) {
            const auto fieldSymbol = declareSymbol(
                    members,
                    SymbolNamespace::value,
                    SymbolKind::structureField,
                    tokenText(field->name),
                    tokenSpan(field->name, field->span),
                    nodeId(field),
                    true,
                    type);
            symbol(fieldSymbol).isMutable =
                    field->bindingKeyword != nullptr && field->bindingKeyword->kind == kwVar;
            initializerSignature.parameters.push_back(CallableParameter {
                tokenText(field->name),
                field->initializer == nullptr,
                nodeId(field),
                nodeId(field->type),
                std::nullopt,
            });
        }

        const auto initializer = declareSymbol(
                model->fileScope_,
                SymbolNamespace::value,
                SymbolKind::synthesizedInitializer,
                tokenText(declaration->name) + ".init",
                tokenSpan(declaration->name, declaration->span),
                nodeId(declaration),
                false,
                type);
        symbol(initializer).declaredType = type;
        symbol(initializer).callable = std::move(initializerSignature);
        symbol(type).synthesizedInitializer = initializer;
        model->declarationSymbols_[nodeId(declaration)] = type;
    }

    void collectEnumeration(const syntax::EnumDeclSyntax::Ptr& declaration) {
        const auto type = declareSymbol(
                model->fileScope_,
                SymbolNamespace::type,
                SymbolKind::enumeration,
                tokenText(declaration->name),
                tokenSpan(declaration->name, declaration->span),
                nodeId(declaration));
        const auto members = createScope(
                ScopeKind::typeMembers,
                model->fileScope_,
                nodeId(declaration));
        symbol(type).memberScope = members;
        model->introducedScopes_[nodeId(declaration)] = members;

        for (const auto& enumCase : declaration->cases) {
            CallableSignature signature {
                CallableKind::enumCase,
                enumCase->hasPayloadClause,
                {},
            };
            for (const auto& associatedType : enumCase->associatedTypes) {
                signature.parameters.push_back(CallableParameter {
                    associatedType->label == nullptr
                            ? std::optional<std::string>()
                            : std::optional<std::string>(associatedType->label->rawValue),
                    true,
                    nodeId(associatedType),
                    nodeId(associatedType->type),
                    std::nullopt,
                });
            }
            const auto caseSymbol = declareSymbol(
                    members,
                    SymbolNamespace::value,
                    SymbolKind::enumCase,
                    tokenText(enumCase->name),
                    tokenSpan(enumCase->name, enumCase->span),
                    nodeId(enumCase),
                    true,
                    type);
            symbol(caseSymbol).declaredType = type;
            symbol(caseSymbol).callable = std::move(signature);
        }
    }

    void resolveTopLevelSignature(const syntax::NodePtr& node) {
        if (node == nullptr) return;
        setContainingScope(node, model->fileScope_);
        switch (node->kind) {
            case syntax::Kind::bindingDecl: {
                const auto declaration =
                        std::static_pointer_cast<syntax::BindingDeclSyntax>(node);
                const auto type = resolveType(declaration->annotation, model->fileScope_);
                const auto declarationSymbol = declaredSymbolId(declaration);
                if (declarationSymbol.has_value()) {
                    symbol(*declarationSymbol).declaredType = type;
                }
                break;
            }
            case syntax::Kind::functionDecl:
                resolveFunctionSignature(
                        std::static_pointer_cast<syntax::FunctionDeclSyntax>(node));
                break;
            case syntax::Kind::structDecl:
                resolveStructureSignature(
                        std::static_pointer_cast<syntax::StructDeclSyntax>(node));
                break;
            case syntax::Kind::enumDecl:
                resolveEnumerationSignature(
                        std::static_pointer_cast<syntax::EnumDeclSyntax>(node));
                break;
            default:
                break;
        }
    }

    void resolveTopLevelBody(const syntax::NodePtr& node) {
        if (node == nullptr) return;
        switch (node->kind) {
            case syntax::Kind::bindingDecl:
                resolveExpression(
                        std::static_pointer_cast<syntax::BindingDeclSyntax>(node)->initializer,
                        model->fileScope_);
                break;
            case syntax::Kind::functionDecl: {
                const auto declaration =
                        std::static_pointer_cast<syntax::FunctionDeclSyntax>(node);
                const auto functionScope = introducedScopeId(declaration);
                if (functionScope.has_value()) {
                    resolveBlock(declaration->body, *functionScope);
                }
                break;
            }
            case syntax::Kind::structDecl: {
                const auto declaration =
                        std::static_pointer_cast<syntax::StructDeclSyntax>(node);
                const auto structure = declaredSymbolId(declaration);
                if (!structure.has_value() || !symbol(*structure).memberScope.has_value()) break;
                const auto memberScope = *symbol(*structure).memberScope;
                for (const auto& field : declaration->fields) {
                    resolveExpression(field->initializer, memberScope);
                }
                break;
            }
            default:
                break;
        }
    }

    void resolveBinding(
            const syntax::BindingDeclSyntax::Ptr& declaration,
            ScopeId currentScope) {
        setContainingScope(declaration, currentScope);
        const auto declaredType = resolveType(declaration->annotation, currentScope);
        resolveExpression(declaration->initializer, currentScope);

        const auto id = declareSymbol(
                currentScope,
                SymbolNamespace::value,
                SymbolKind::binding,
                tokenText(declaration->name),
                tokenSpan(declaration->name, declaration->span),
                nodeId(declaration));
        symbol(id).isMutable = declaration->isMutable();
        if (declaredType.has_value()) {
            symbol(id).declaredType = declaredType;
        }
    }

    void resolveFunctionSignature(
            const syntax::FunctionDeclSyntax::Ptr& declaration) {
        setContainingScope(declaration, model->fileScope_);
        const auto functionSymbol = declaredSymbolId(declaration);
        if (!functionSymbol.has_value()) return;
        const auto functionScope = createScope(
                ScopeKind::function,
                model->fileScope_,
                nodeId(declaration));
        model->introducedScopes_[nodeId(declaration)] = functionScope;

        for (size_t index = 0; index < declaration->parameters.size(); ++index) {
            const auto& parameter = declaration->parameters[index];
            setContainingScope(parameter, functionScope);
            const auto parameterType = resolveType(parameter->type, functionScope);
            const auto parameterSymbol = declareSymbol(
                    functionScope,
                    SymbolNamespace::value,
                    SymbolKind::parameter,
                    tokenText(parameter->name),
                    tokenSpan(parameter->name, parameter->span),
                    nodeId(parameter));
                symbol(parameterSymbol).isMutable =
                    parameter->accessEffect() == syntax::AccessEffect::inout ||
                    parameter->accessEffect() == syntax::AccessEffect::consuming;
            symbol(parameterSymbol).declaredType = parameterType;

            auto& callable = *symbol(*functionSymbol).callable;
            if (index < callable.parameters.size()) {
                callable.parameters[index].type = parameterType;
            }
        }

        auto returnType = resolveType(declaration->returnType, functionScope);
        if (!returnType.has_value()) {
            returnType = lookupType(functionScope, "Void");
        }
        symbol(*functionSymbol).declaredType = returnType;
    }

    void resolveStructureSignature(const syntax::StructDeclSyntax::Ptr& declaration) {
        const auto structure = declaredSymbolId(declaration);
        if (!structure.has_value() || !symbol(*structure).memberScope.has_value() ||
            !symbol(*structure).synthesizedInitializer.has_value()) {
            return;
        }
        const auto memberScope = *symbol(*structure).memberScope;
        setContainingScope(declaration, model->fileScope_);

        const auto initializer = *symbol(*structure).synthesizedInitializer;
        for (size_t index = 0; index < declaration->fields.size(); ++index) {
            const auto& field = declaration->fields[index];
            setContainingScope(field, memberScope);
            const auto fieldType = resolveType(field->type, memberScope);
            const auto fieldSymbol = declaredSymbolId(field);
            if (fieldSymbol.has_value()) symbol(*fieldSymbol).declaredType = fieldType;
            auto& parameters = symbol(initializer).callable->parameters;
            if (index < parameters.size()) parameters[index].type = fieldType;
        }
    }

    void resolveEnumerationSignature(const syntax::EnumDeclSyntax::Ptr& declaration) {
        const auto enumeration = declaredSymbolId(declaration);
        if (!enumeration.has_value() || !symbol(*enumeration).memberScope.has_value()) return;
        const auto memberScope = *symbol(*enumeration).memberScope;
        setContainingScope(declaration, model->fileScope_);

        for (const auto& enumCase : declaration->cases) {
            setContainingScope(enumCase, memberScope);
            const auto caseSymbol = declaredSymbolId(enumCase);
            if (!caseSymbol.has_value()) continue;
            for (size_t index = 0; index < enumCase->associatedTypes.size(); ++index) {
                const auto& associatedType = enumCase->associatedTypes[index];
                setContainingScope(associatedType, memberScope);
                const auto payloadType = resolveType(associatedType->type, memberScope);
                auto& parameters = symbol(*caseSymbol).callable->parameters;
                if (index < parameters.size()) parameters[index].type = payloadType;
            }
        }
    }

    std::optional<SymbolId> resolveType(
            const syntax::TypePtr& type,
            ScopeId currentScope) {
        if (type == nullptr) return std::nullopt;
        setContainingScope(type, currentScope);

        switch (type->kind) {
            case syntax::Kind::errorType:
                return std::nullopt;
            case syntax::Kind::nominalType: {
                const auto nominal = std::static_pointer_cast<syntax::NominalTypeSyntax>(type);
                for (const auto& argument : nominal->arguments) {
                    resolveType(argument, currentScope);
                }
                const auto resolved = lookupType(currentScope, tokenText(nominal->name));
                if (!resolved.has_value()) {
                    report(
                            NameResolutionDiagnosticId::undefinedType,
                            tokenSpan(nominal->name, nominal->span),
                            "cannot find type '" + tokenText(nominal->name) + "' in scope");
                    return std::nullopt;
                }
                model->referenceSymbols_[nodeId(type)] = *resolved;
                return resolved;
            }
            case syntax::Kind::arrayType: {
                const auto array = std::static_pointer_cast<syntax::ArrayTypeSyntax>(type);
                resolveType(array->element, currentScope);
                return bindBuiltinType(type, currentScope, "Array");
            }
            case syntax::Kind::dictionaryType: {
                const auto dictionary = std::static_pointer_cast<syntax::DictionaryTypeSyntax>(type);
                resolveType(dictionary->key, currentScope);
                resolveType(dictionary->value, currentScope);
                return bindBuiltinType(type, currentScope, "Dict");
            }
            case syntax::Kind::optionalType: {
                const auto optional = std::static_pointer_cast<syntax::OptionalTypeSyntax>(type);
                resolveType(optional->wrapped, currentScope);
                return bindBuiltinType(type, currentScope, "Optional");
            }
            default:
                return std::nullopt;
        }
    }

    std::optional<SymbolId> bindBuiltinType(
            const syntax::TypePtr& node,
            ScopeId currentScope,
            const std::string& name) {
        const auto resolved = lookupType(currentScope, name);
        assert(resolved.has_value());
        model->referenceSymbols_[nodeId(node)] = *resolved;
        return resolved;
    }

    void resolveBlock(const syntax::BlockExprSyntax::Ptr& block, ScopeId parentScope) {
        if (block == nullptr) return;
        setContainingScope(block, parentScope);
        const auto blockScope = createScope(ScopeKind::block, parentScope, nodeId(block));
        model->introducedScopes_[nodeId(block)] = blockScope;

        for (const auto& item : block->items) {
            if (item->kind == syntax::Kind::bindingDecl) {
                resolveBinding(
                        std::static_pointer_cast<syntax::BindingDeclSyntax>(item),
                        blockScope);
            } else {
                resolveNode(item, blockScope);
            }
        }
    }

    void resolveNode(const syntax::NodePtr& node, ScopeId currentScope) {
        if (node == nullptr) return;
        setContainingScope(node, currentScope);

        switch (node->kind) {
            case syntax::Kind::whileStmt: {
                const auto statement = std::static_pointer_cast<syntax::WhileStmtSyntax>(node);
                resolveExpression(statement->condition, currentScope);
                resolveBlock(statement->body, currentScope);
                break;
            }
            case syntax::Kind::bindingDecl:
                resolveBinding(
                        std::static_pointer_cast<syntax::BindingDeclSyntax>(node),
                        currentScope);
                break;
            case syntax::Kind::blockExpr:
                resolveBlock(std::static_pointer_cast<syntax::BlockExprSyntax>(node), currentScope);
                break;
            default:
                if (isExpressionKind(node->kind)) {
                    resolveExpression(std::static_pointer_cast<syntax::ExprSyntax>(node), currentScope);
                }
                break;
        }
    }

    bool isExpressionKind(syntax::Kind kind) const {
        switch (kind) {
            case syntax::Kind::errorExpr:
            case syntax::Kind::nameExpr:
            case syntax::Kind::literalExpr:
            case syntax::Kind::parenthesizedExpr:
            case syntax::Kind::prefixExpr:
            case syntax::Kind::accessExpr:
            case syntax::Kind::binaryExpr:
            case syntax::Kind::assignmentExpr:
            case syntax::Kind::memberExpr:
            case syntax::Kind::callExpr:
            case syntax::Kind::subscriptExpr:
            case syntax::Kind::arrayExpr:
            case syntax::Kind::dictionaryExpr:
            case syntax::Kind::contextualCaseExpr:
            case syntax::Kind::blockExpr:
            case syntax::Kind::ifExpr:
            case syntax::Kind::returnExpr:
            case syntax::Kind::matchExpr:
                return true;
            default:
                return false;
        }
    }

    void resolveExpression(const syntax::ExprPtr& expression, ScopeId currentScope) {
        if (expression == nullptr) return;
        setContainingScope(expression, currentScope);

        switch (expression->kind) {
            case syntax::Kind::errorExpr:
            case syntax::Kind::literalExpr:
                break;
            case syntax::Kind::nameExpr:
                resolveName(std::static_pointer_cast<syntax::NameExprSyntax>(expression), currentScope);
                break;
            case syntax::Kind::parenthesizedExpr:
                resolveExpression(
                        std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression,
                        currentScope);
                break;
            case syntax::Kind::prefixExpr:
                resolveExpression(
                        std::static_pointer_cast<syntax::PrefixExprSyntax>(expression)->operand,
                        currentScope);
                break;
            case syntax::Kind::accessExpr:
                resolveExpression(
                        std::static_pointer_cast<syntax::AccessExprSyntax>(expression)->operand,
                        currentScope);
                break;
            case syntax::Kind::binaryExpr: {
                const auto binary = std::static_pointer_cast<syntax::BinaryExprSyntax>(expression);
                resolveExpression(binary->left, currentScope);
                resolveExpression(binary->right, currentScope);
                break;
            }
            case syntax::Kind::assignmentExpr: {
                const auto assignment = std::static_pointer_cast<syntax::AssignmentExprSyntax>(expression);
                resolveExpression(assignment->target, currentScope);
                resolveExpression(assignment->value, currentScope);
                break;
            }
            case syntax::Kind::memberExpr:
                resolveMember(std::static_pointer_cast<syntax::MemberExprSyntax>(expression), currentScope);
                break;
            case syntax::Kind::callExpr:
                resolveCall(std::static_pointer_cast<syntax::CallExprSyntax>(expression), currentScope);
                break;
            case syntax::Kind::subscriptExpr: {
                const auto subscript = std::static_pointer_cast<syntax::SubscriptExprSyntax>(expression);
                resolveExpression(subscript->base, currentScope);
                resolveExpression(subscript->index, currentScope);
                break;
            }
            case syntax::Kind::arrayExpr: {
                const auto array = std::static_pointer_cast<syntax::ArrayExprSyntax>(expression);
                for (const auto& element : array->elements) resolveExpression(element, currentScope);
                break;
            }
            case syntax::Kind::dictionaryExpr: {
                const auto dictionary = std::static_pointer_cast<syntax::DictionaryExprSyntax>(expression);
                for (const auto& entry : dictionary->entries) {
                    setContainingScope(entry, currentScope);
                    resolveExpression(entry->key, currentScope);
                    resolveExpression(entry->value, currentScope);
                }
                break;
            }
            case syntax::Kind::contextualCaseExpr:
                resolveContextualCase(
                        std::static_pointer_cast<syntax::ContextualCaseExprSyntax>(expression),
                        currentScope);
                break;
            case syntax::Kind::blockExpr:
                resolveBlock(std::static_pointer_cast<syntax::BlockExprSyntax>(expression), currentScope);
                break;
            case syntax::Kind::ifExpr: {
                const auto conditional = std::static_pointer_cast<syntax::IfExprSyntax>(expression);
                resolveExpression(conditional->condition, currentScope);
                resolveBlock(conditional->thenBranch, currentScope);
                resolveExpression(conditional->elseBranch, currentScope);
                break;
            }
            case syntax::Kind::returnExpr:
                resolveExpression(
                        std::static_pointer_cast<syntax::ReturnExprSyntax>(expression)->value,
                        currentScope);
                break;
            case syntax::Kind::matchExpr:
                resolveMatch(std::static_pointer_cast<syntax::MatchExprSyntax>(expression), currentScope);
                break;
            default:
                break;
        }
    }

    void resolveName(const syntax::NameExprSyntax::Ptr& expression, ScopeId currentScope) {
        const auto name = tokenText(expression->name);
        const auto resolved = lookupValueOrType(currentScope, name);
        if (!resolved.has_value()) {
            report(
                    NameResolutionDiagnosticId::undefinedName,
                    tokenSpan(expression->name, expression->span),
                    "cannot find '" + name + "' in scope");
            return;
        }
        model->referenceSymbols_[nodeId(expression)] = *resolved;
    }

    void resolveMember(const syntax::MemberExprSyntax::Ptr& expression, ScopeId currentScope) {
        resolveExpression(expression->base, currentScope);
        const auto baseType = expressionType(expression->base);
        const auto name = tokenText(expression->member);
        if (!baseType.has_value()) {
            if (hasHardResolutionFailure(expression->base)) return;
            addDeferred(
                    expression,
                    DeferredResolutionKind::memberNeedsBaseType,
                    name,
                    tokenSpan(expression->member, expression->span));
            return;
        }

        const auto& type = symbol(*baseType);
        if (!type.memberScope.has_value()) {
            report(
                    NameResolutionDiagnosticId::unknownMember,
                    tokenSpan(expression->member, expression->span),
                    "type '" + type.name + "' has no member named '" + name + "'");
            return;
        }
        const auto member = lookupMember(*baseType, name);
        if (!member.has_value()) {
            report(
                    NameResolutionDiagnosticId::unknownMember,
                    tokenSpan(expression->member, expression->span),
                    "type '" + type.name + "' has no member named '" + name + "'");
            return;
        }
        model->referenceSymbols_[nodeId(expression)] = *member;
    }

    void resolveCall(const syntax::CallExprSyntax::Ptr& expression, ScopeId currentScope) {
        resolveExpression(expression->callee, currentScope);
        for (const auto& argument : expression->arguments) {
            setContainingScope(argument, currentScope);
            resolveExpression(argument->value, currentScope);
        }

        const auto referenced = referencedSymbol(expression->callee);
        if (!referenced.has_value()) {
            if (hasDeferredReference(expression->callee)) {
                addDeferred(
                        expression,
                        DeferredResolutionKind::callNeedsCalleeType,
                        "<callee>",
                        expression->callee->span);
            }
            return;
        }

        auto target = *referenced;
        const auto& callee = symbol(target);
        if (callee.kind == SymbolKind::structure) {
            if (!callee.synthesizedInitializer.has_value()) {
                report(
                        NameResolutionDiagnosticId::notCallable,
                        expression->callee->span,
                        "struct '" + callee.name + "' has no synthesized initializer");
                return;
            }
            target = *callee.synthesizedInitializer;
        }

        if (!symbol(target).callable.has_value()) {
            report(
                    NameResolutionDiagnosticId::notCallable,
                    expression->callee->span,
                    "'" + callee.name + "' is not callable");
            return;
        }

        model->callTargets_[nodeId(expression)] = target;
        validateCallArguments(expression, target);
    }

    void resolveContextualCase(
            const syntax::ContextualCaseExprSyntax::Ptr& expression,
            ScopeId currentScope) {
        for (const auto& argument : expression->arguments) {
            setContainingScope(argument, currentScope);
            resolveExpression(argument->value, currentScope);
        }
        addDeferred(
                expression,
                DeferredResolutionKind::contextualEnumCaseNeedsType,
                tokenText(expression->name),
                tokenSpan(expression->name, expression->span));
    }

    void resolveMatch(const syntax::MatchExprSyntax::Ptr& expression, ScopeId currentScope) {
        resolveExpression(expression->scrutinee, currentScope);
        for (const auto& arm : expression->arms) {
            setContainingScope(arm, currentScope);
            const auto armScope = createScope(
                    ScopeKind::matchArm,
                    currentScope,
                    nodeId(arm));
            model->introducedScopes_[nodeId(arm)] = armScope;
            resolvePattern(arm->pattern, armScope, std::nullopt);
            resolveExpression(arm->body, armScope);
        }
    }

    void resolvePattern(
            const syntax::PatternPtr& pattern,
            ScopeId currentScope,
            std::optional<SymbolId> expectedType) {
        if (pattern == nullptr) return;
        setContainingScope(pattern, currentScope);

        switch (pattern->kind) {
            case syntax::Kind::errorPattern:
            case syntax::Kind::wildcardPattern:
            case syntax::Kind::literalPattern:
                break;
            case syntax::Kind::bindingPattern: {
                const auto binding = std::static_pointer_cast<syntax::BindingPatternSyntax>(pattern);
                const auto bindingSymbol = declareSymbol(
                        currentScope,
                        SymbolNamespace::value,
                        SymbolKind::patternBinding,
                        tokenText(binding->name),
                        tokenSpan(binding->name, binding->span),
                        nodeId(binding));
                symbol(bindingSymbol).declaredType = expectedType;
                break;
            }
            case syntax::Kind::enumCasePattern:
                resolveEnumCasePattern(
                        std::static_pointer_cast<syntax::EnumCasePatternSyntax>(pattern),
                        currentScope);
                break;
            default:
                break;
        }
    }

    void resolveEnumCasePattern(
            const syntax::EnumCasePatternSyntax::Ptr& pattern,
            ScopeId currentScope) {
        std::optional<SymbolId> caseSymbol;
        if (pattern->qualifier != nullptr) {
            const auto qualifier = lookupType(currentScope, tokenText(pattern->qualifier));
            if (!qualifier.has_value()) {
                report(
                        NameResolutionDiagnosticId::undefinedType,
                        pattern->qualifier->span,
                        "cannot find type '" + tokenText(pattern->qualifier) + "' in scope");
            } else {
                model->qualifierSymbols_[nodeId(pattern)] = *qualifier;
                caseSymbol = lookupMember(*qualifier, tokenText(pattern->name));
                if (!caseSymbol.has_value() || !isEnumCaseSymbol(symbol(*caseSymbol).kind)) {
                    report(
                            NameResolutionDiagnosticId::unknownEnumCase,
                            tokenSpan(pattern->name, pattern->span),
                            "enum '" + symbol(*qualifier).name + "' has no case named '" +
                                    tokenText(pattern->name) + "'");
                    caseSymbol.reset();
                } else {
                    model->referenceSymbols_[nodeId(pattern)] = *caseSymbol;
                    validatePatternArguments(pattern, *caseSymbol);
                }
            }
        } else {
            addDeferred(
                    pattern,
                    DeferredResolutionKind::contextualEnumPatternNeedsType,
                    tokenText(pattern->name),
                    tokenSpan(pattern->name, pattern->span));
        }

        for (size_t index = 0; index < pattern->arguments.size(); ++index) {
            const auto& argument = pattern->arguments[index];
            setContainingScope(argument, currentScope);
            std::optional<SymbolId> payloadType;
            if (caseSymbol.has_value()) {
                const auto& parameters = symbol(*caseSymbol).callable->parameters;
                if (index < parameters.size()) payloadType = parameters[index].type;
            }
            resolvePattern(argument->pattern, currentScope, payloadType);
        }
    }

    void validateCallArguments(
            const syntax::CallExprSyntax::Ptr& call,
            SymbolId target) {
        const auto& callable = *symbol(target).callable;
        if (!callable.acceptsArgumentClause) {
            report(
                    NameResolutionDiagnosticId::unexpectedArgumentClause,
                    call->span,
                    "'" + symbol(target).name + "' has no payload and must be used without parentheses");
            return;
        }

        if (callable.kind == CallableKind::enumCase) {
            if (call->arguments.size() != callable.parameters.size()) {
                reportArgumentCount(call->span, symbol(target), call->arguments.size());
            }
            const auto count = std::min(call->arguments.size(), callable.parameters.size());
            for (size_t index = 0; index < count; ++index) {
                validateLabel(
                        callable.parameters[index].label,
                        call->arguments[index]->label,
                        call->arguments[index]->span);
            }
            return;
        }

        std::vector<bool> matched(callable.parameters.size(), false);
        size_t previousIndex = 0;
        bool hasPrevious = false;
        for (const auto& argument : call->arguments) {
            if (argument->label == nullptr) {
                const auto expected = hasPrevious ? previousIndex + 1 : 0;
                const auto expectedLabel = expected < callable.parameters.size()
                        ? callable.parameters[expected].label
                        : std::optional<std::string>();
                report(
                        NameResolutionDiagnosticId::missingArgumentLabel,
                        argument->span,
                        expectedLabel.has_value()
                                ? "argument requires label '" + *expectedLabel + ":'"
                                : std::string("ordinary calls require an argument label"));
                continue;
            }

            const auto label = argument->label->rawValue;
            const auto found = std::find_if(
                    callable.parameters.begin(),
                    callable.parameters.end(),
                    [&label](const CallableParameter& parameter) {
                        return parameter.label.has_value() && *parameter.label == label;
                    });
            if (found == callable.parameters.end()) {
                report(
                        NameResolutionDiagnosticId::unexpectedArgumentLabel,
                        argument->label->span,
                        "call target '" + symbol(target).name +
                                "' has no parameter labeled '" + label + ":'");
                continue;
            }

            const auto index = static_cast<size_t>(
                    std::distance(callable.parameters.begin(), found));
            if (matched[index]) {
                report(
                        NameResolutionDiagnosticId::unexpectedArgumentLabel,
                        argument->label->span,
                        "argument label '" + label + ":' is supplied more than once");
                continue;
            }
            if (hasPrevious && index < previousIndex) {
                report(
                        NameResolutionDiagnosticId::argumentOutOfOrder,
                        argument->label->span,
                        "argument '" + label + ":' is out of declaration order");
            }
            matched[index] = true;
            previousIndex = index;
            hasPrevious = true;
        }

        bool hasMissingRequired = false;
        for (size_t index = 0; index < callable.parameters.size(); ++index) {
            if (callable.parameters[index].required && !matched[index]) {
                hasMissingRequired = true;
                break;
            }
        }
        if (hasMissingRequired) {
            reportArgumentCount(call->span, symbol(target), call->arguments.size());
        }
    }

    void validatePatternArguments(
            const syntax::EnumCasePatternSyntax::Ptr& pattern,
            SymbolId target) {
        const auto& callable = *symbol(target).callable;
        if (!callable.acceptsArgumentClause && pattern->hasPayloadClause) {
            report(
                    NameResolutionDiagnosticId::unexpectedArgumentClause,
                    pattern->span,
                    "enum case '" + symbol(target).name +
                            "' has no payload and must be matched without parentheses");
            return;
        }
        if (pattern->arguments.size() != callable.parameters.size()) {
            report(
                    NameResolutionDiagnosticId::argumentCountMismatch,
                    pattern->span,
                    "enum case '" + symbol(target).name + "' expects " +
                            std::to_string(callable.parameters.size()) +
                            " payload pattern(s), but got " +
                            std::to_string(pattern->arguments.size()));
        }
        const auto count = std::min(pattern->arguments.size(), callable.parameters.size());
        for (size_t index = 0; index < count; ++index) {
            validateLabel(
                    callable.parameters[index].label,
                    pattern->arguments[index]->label,
                    pattern->arguments[index]->span);
        }
    }

    void validateLabel(
            const std::optional<std::string>& expected,
            const Token::Ptr& actual,
            SourceSpan span) {
        if (!expected.has_value() && actual == nullptr) return;
        if (expected.has_value() && actual != nullptr && *expected == actual->rawValue) return;

        if (expected.has_value() && actual == nullptr) {
            report(
                    NameResolutionDiagnosticId::missingArgumentLabel,
                    span,
                    "argument requires label '" + *expected + ":'");
        } else if (!expected.has_value()) {
            report(
                    NameResolutionDiagnosticId::unexpectedArgumentLabel,
                    actual->span,
                    "this enum payload position is unlabeled");
        } else {
            report(
                    NameResolutionDiagnosticId::unexpectedArgumentLabel,
                    actual->span,
                    "expected argument label '" + *expected + ":', but got '" +
                            actual->rawValue + ":'");
        }
    }

    void reportArgumentCount(SourceSpan span, const Symbol& target, size_t actualCount) {
        const auto expectedCount = target.callable->parameters.size();
        const auto requiredCount = static_cast<size_t>(std::count_if(
            target.callable->parameters.begin(),
            target.callable->parameters.end(),
            [](const CallableParameter& parameter) { return parameter.required; }));
        const auto expectation = requiredCount == expectedCount
            ? std::to_string(expectedCount)
            : std::to_string(requiredCount) + " to " + std::to_string(expectedCount);
        report(
                NameResolutionDiagnosticId::argumentCountMismatch,
                span,
            "call target '" + target.name + "' expects " + expectation +
                " argument(s), but got " +
                        std::to_string(actualCount));
    }

    std::optional<SymbolId> referencedSymbol(const syntax::NodePtr& node) const {
        if (node == nullptr) return std::nullopt;
        const auto found = model->referenceSymbols_.find(nodeId(node));
        if (found == model->referenceSymbols_.end()) return std::nullopt;
        return found->second;
    }

    std::optional<SymbolId> declaredSymbolId(const syntax::NodePtr& node) const {
        if (node == nullptr) return std::nullopt;
        const auto found = model->declarationSymbols_.find(nodeId(node));
        if (found == model->declarationSymbols_.end()) return std::nullopt;
        return found->second;
    }

    std::optional<ScopeId> introducedScopeId(const syntax::NodePtr& node) const {
        if (node == nullptr) return std::nullopt;
        const auto found = model->introducedScopes_.find(nodeId(node));
        if (found == model->introducedScopes_.end()) return std::nullopt;
        return found->second;
    }

    bool hasDeferredReference(const syntax::NodePtr& node) const {
        return node != nullptr &&
               model->deferredReferenceIndices_.contains(nodeId(node));
    }

    bool hasHardResolutionFailure(const syntax::ExprPtr& expression) const {
        if (expression == nullptr || referencedSymbol(expression).has_value() ||
            hasDeferredReference(expression)) {
            return false;
        }
        switch (expression->kind) {
            case syntax::Kind::nameExpr:
            case syntax::Kind::memberExpr:
            case syntax::Kind::callExpr:
                return true;
            default:
                return false;
        }
    }

    std::optional<SymbolId> expressionType(const syntax::ExprPtr& expression) const {
        if (expression == nullptr) return std::nullopt;
        switch (expression->kind) {
            case syntax::Kind::nameExpr:
            case syntax::Kind::memberExpr:
            case syntax::Kind::contextualCaseExpr: {
                const auto referenced = referencedSymbol(expression);
                if (!referenced.has_value()) return std::nullopt;
                const auto& referencedValue = symbol(*referenced);
                if (isTypeSymbol(referencedValue.kind)) return referenced;
                return referencedValue.declaredType;
            }
            case syntax::Kind::callExpr: {
                const auto found = model->callTargets_.find(nodeId(expression));
                if (found == model->callTargets_.end()) return std::nullopt;
                return symbol(found->second).declaredType;
            }
            case syntax::Kind::literalExpr: {
                const auto literal = std::static_pointer_cast<syntax::LiteralExprSyntax>(expression);
                switch (literal->literal->kind) {
                    case decimalLiteral: return lookupType(model->fileScope_, "Int");
                    case stringLiteral: return lookupType(model->fileScope_, "String");
                    case byteLiteral: return lookupType(model->fileScope_, "UInt8");
                    case booleanLiteral: return lookupType(model->fileScope_, "Bool");
                    default: return std::nullopt;
                }
            }
            case syntax::Kind::parenthesizedExpr:
                return expressionType(
                        std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression);
            case syntax::Kind::prefixExpr:
                return expressionType(
                        std::static_pointer_cast<syntax::PrefixExprSyntax>(expression)->operand);
            case syntax::Kind::accessExpr:
                return expressionType(
                        std::static_pointer_cast<syntax::AccessExprSyntax>(expression)->operand);
            case syntax::Kind::assignmentExpr:
                return expressionType(
                        std::static_pointer_cast<syntax::AssignmentExprSyntax>(expression)->value);
            case syntax::Kind::arrayExpr:
                return lookupType(model->fileScope_, "Array");
            case syntax::Kind::dictionaryExpr:
                return lookupType(model->fileScope_, "Dict");
            case syntax::Kind::subscriptExpr: {
                const auto subscript = std::static_pointer_cast<syntax::SubscriptExprSyntax>(expression);
                const auto base = expressionType(subscript->base);
                if (base.has_value() && symbol(*base).name == "String") {
                    return lookupType(model->fileScope_, "UInt8");
                }
                return std::nullopt;
            }
            default:
                return std::nullopt;
        }
    }

    std::optional<SymbolId> lookupValueOrType(
            ScopeId currentScope,
            const std::string& name) const {
        auto current = std::optional<ScopeId>(currentScope);
        while (current.has_value()) {
            const auto& currentScopeValue = scope(*current);
            const auto value = currentScopeValue.values.find(name);
            if (value != currentScopeValue.values.end()) return value->second;
            const auto type = currentScopeValue.types.find(name);
            if (type != currentScopeValue.types.end()) return type->second;
            current = currentScopeValue.parent;
        }
        return std::nullopt;
    }

    std::optional<SymbolId> lookupType(
            ScopeId currentScope,
            const std::string& name) const {
        auto current = std::optional<ScopeId>(currentScope);
        while (current.has_value()) {
            const auto& currentScopeValue = scope(*current);
            const auto type = currentScopeValue.types.find(name);
            if (type != currentScopeValue.types.end()) return type->second;
            current = currentScopeValue.parent;
        }
        return std::nullopt;
    }

    std::optional<SymbolId> lookupMember(
            SymbolId type,
            const std::string& name) const {
        if (!symbol(type).memberScope.has_value()) return std::nullopt;
        const auto& members = scope(*symbol(type).memberScope).values;
        const auto found = members.find(name);
        if (found == members.end()) return std::nullopt;
        return found->second;
    }

    void addDeferred(
            const syntax::NodePtr& node,
            DeferredResolutionKind kind,
            std::string name,
            SourceSpan span) {
        const auto id = nodeId(node);
        if (model->deferredReferenceIndices_.contains(id)) return;
        const auto index = model->deferredReferences_.size();
        model->deferredReferenceIndices_[id] = index;
        model->deferredReferences_.push_back(DeferredReference {
            id,
            kind,
            std::move(name),
            span,
        });
    }

    void setContainingScope(const syntax::NodePtr& node, ScopeId currentScope) {
        if (node == nullptr) return;
        model->containingScopes_[nodeId(node)] = currentScope;
    }
};

NameResolutionResult NameResolver::resolve(
        const syntax::SourceFileSyntax::Ptr& root) const {
    NameResolutionBuilder builder;
    return builder.build(root);
}

const char* diagnosticName(NameResolutionDiagnosticId id) {
    switch (id) {
        case NameResolutionDiagnosticId::duplicateDeclaration:
            return "name-resolution.duplicate-declaration";
        case NameResolutionDiagnosticId::undefinedName:
            return "name-resolution.undefined-name";
        case NameResolutionDiagnosticId::undefinedType:
            return "name-resolution.undefined-type";
        case NameResolutionDiagnosticId::unknownMember:
            return "name-resolution.unknown-member";
        case NameResolutionDiagnosticId::unknownEnumCase:
            return "name-resolution.unknown-enum-case";
        case NameResolutionDiagnosticId::notCallable:
            return "name-resolution.not-callable";
        case NameResolutionDiagnosticId::unexpectedArgumentClause:
            return "name-resolution.unexpected-argument-clause";
        case NameResolutionDiagnosticId::argumentCountMismatch:
            return "name-resolution.argument-count-mismatch";
        case NameResolutionDiagnosticId::missingArgumentLabel:
            return "name-resolution.missing-argument-label";
        case NameResolutionDiagnosticId::unexpectedArgumentLabel:
            return "name-resolution.unexpected-argument-label";
        case NameResolutionDiagnosticId::argumentOutOfOrder:
            return "name-resolution.argument-out-of-order";
    }
    return "name-resolution.unknown";
}

std::string dump(const std::vector<NameResolutionDiagnostic>& diagnostics) {
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << diagnosticName(diagnostic.id)
            << '@' << diagnostic.span.offset << ':' << diagnostic.span.length
            << ' ' << diagnostic.message << '\n';
    }
    return out.str();
}

} // namespace joyeer::semantic