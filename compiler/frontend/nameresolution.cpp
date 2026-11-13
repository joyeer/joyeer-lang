#include "nameresolution.h"
#include "hostbuiltins.h"

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
           kind == SymbolKind::enumeration ||
           kind == SymbolKind::protocol ||
           kind == SymbolKind::typeParameter;
}

bool isEnumCaseSymbol(SymbolKind kind) {
    return kind == SymbolKind::builtinEnumCase || kind == SymbolKind::enumCase;
}

void validateArgumentLabel(
        const std::optional<std::string>& expected,
        const Token::Ptr& actual,
        SourceSpan span,
        std::vector<NameResolutionDiagnostic>& diagnostics) {
    if (!expected.has_value() && actual == nullptr) return;
    if (expected.has_value() && actual != nullptr && *expected == actual->rawValue) return;

    if (expected.has_value() && actual == nullptr) {
        diagnostics.push_back({
            NameResolutionDiagnosticId::missingArgumentLabel,
            span,
            "argument requires label '" + *expected + ":'",
        });
    } else if (!expected.has_value()) {
        diagnostics.push_back({
            NameResolutionDiagnosticId::unexpectedArgumentLabel,
            actual->span,
            "this enum payload position is unlabeled",
        });
    } else {
        diagnostics.push_back({
            NameResolutionDiagnosticId::unexpectedArgumentLabel,
            actual->span,
            "expected argument label '" + *expected + ":', but got '" +
                    actual->rawValue + ":'",
        });
    }
}

} // namespace

std::vector<NameResolutionDiagnostic> validateCallArguments(
        const syntax::CallExprSyntax& call,
        const Symbol& target) {
    assert(target.callable.has_value());
    const auto& callable = *target.callable;
    std::vector<NameResolutionDiagnostic> diagnostics;
    const auto report = [&](NameResolutionDiagnosticId id, SourceSpan span, std::string message) {
        diagnostics.push_back({ id, span, std::move(message) });
    };
    const auto reportArgumentCount = [&] {
        const auto expectedCount = callable.parameters.size();
        const auto requiredCount = static_cast<size_t>(std::count_if(
                callable.parameters.begin(),
                callable.parameters.end(),
                [](const CallableParameter& parameter) { return parameter.required; }));
        const auto expectation = requiredCount == expectedCount
                ? std::to_string(expectedCount)
                : std::to_string(requiredCount) + " to " + std::to_string(expectedCount);
        report(
                NameResolutionDiagnosticId::argumentCountMismatch,
                call.span,
                "call target '" + target.name + "' expects " + expectation +
                        " argument(s), but got " + std::to_string(call.arguments.size()));
    };
    if (!callable.acceptsArgumentClause) {
        report(
                NameResolutionDiagnosticId::unexpectedArgumentClause,
                call.span,
                "'" + target.name + "' has no payload and must be used without parentheses");
        return diagnostics;
    }

    if (callable.kind == CallableKind::enumCase) {
        if (call.arguments.size() != callable.parameters.size()) {
            reportArgumentCount();
        }
        const auto count = std::min(call.arguments.size(), callable.parameters.size());
        for (size_t index = 0; index < count; ++index) {
            validateArgumentLabel(
                    callable.parameters[index].label,
                    call.arguments[index]->label,
                    call.arguments[index]->span,
                    diagnostics);
        }
        return diagnostics;
    }

    std::vector<bool> matched(callable.parameters.size(), false);
    size_t previousIndex = 0;
    bool hasPrevious = false;
    for (const auto& argument : call.arguments) {
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
                    "call target '" + target.name +
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
    if (hasMissingRequired) reportArgumentCount();
    return diagnostics;
}

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
        model->sourceModules_[root->span.sourceId] = 0;
        for (const auto& import : root->imports) {
            report(NameResolutionDiagnosticId::unknownImport, import.span,
                   "imports require named compilation units and explicit dependency source files");
        }

        collectTopLevelDeclarations(root);
        collectMembers(root);
        for (const auto& item : root->items) {
            resolveTopLevelSignature(item);
        }
        validateExposedTypes();
        for (const auto& item : root->items) {
            resolveTopLevelBody(item);
        }

        return NameResolutionResult { model, std::move(diagnostics) };
    }

    NameResolutionResult build(const std::vector<ModuleInput>& modules) {
        auto root = std::make_shared<syntax::SourceFileSyntax>(
                SourceSpan {}, std::vector<syntax::NodePtr> {});
        for (const auto& module : modules) {
            for (const auto& file : module.files) {
                root->items.insert(root->items.end(), file->items.begin(), file->items.end());
            }
        }
        model = SemanticModel::Ptr(new SemanticModel(root));
        indexNode(root);
        buildPrelude();
        std::vector<ScopeId> moduleScopes;
        for (const auto& module : modules) {
            const auto id = createScope(ScopeKind::module, model->preludeScope_, std::nullopt);
            moduleScopes.push_back(id);
            model->moduleNames_.push_back(module.name);
            namedModules.emplace(module.name, id);
        }
        struct FileContext {
            syntax::SourceFileSyntax::Ptr file;
            ScopeId scope;
            uint32_t module;
        };
        std::vector<FileContext> files;
        for (size_t index = 0; index < modules.size(); ++index) {
            activeModule = static_cast<uint32_t>(index);
            for (const auto& file : modules[index].files) {
                indexNode(file);
                model->sourceModules_[file->span.sourceId] = activeModule;
                model->runtimeSources_[file->span.sourceId] = modules[index].runtimeContext;
                model->coreSources_[file->span.sourceId] = modules[index].coreContext;
                model->fileScope_ = createScope(
                        ScopeKind::file, moduleScopes[index], nodeId(file));
                model->introducedScopes_[nodeId(file)] = model->fileScope_;
                setContainingScope(file, model->fileScope_);
                files.push_back({file, model->fileScope_, activeModule});
                collectTopLevelDeclarations(file);
                for (const auto& item : file->items) {
                    const auto id = declaredSymbolId(item);
                    if (!id.has_value()) continue;
                    auto& declaration = symbol(*id);
                    if (declaration.visibility == syntax::Visibility::private_) continue;
                    auto& moduleScope = scope(moduleScopes[index]);
                    if (moduleScope.values.contains(declaration.name) ||
                        moduleScope.types.contains(declaration.name)) {
                        report(NameResolutionDiagnosticId::duplicateDeclaration, declaration.span,
                               "duplicate module declaration of '" + declaration.name + "'");
                        declaration.isInvalid = true;
                    } else {
                        auto& names = declaration.nameSpace == SymbolNamespace::type
                                ? moduleScope.types : moduleScope.values;
                        names.emplace(declaration.name, *id);
                    }
                }
            }
        }
        for (const auto& file : files) {
            auto& imports = fileImports[file.scope];
            for (const auto& import : file.file->imports) {
                const auto target = namedModules.find(import.name);
                if (target == namedModules.end()) {
                    report(NameResolutionDiagnosticId::unknownImport, import.span,
                           "unknown module '" + import.name + "'");
                    continue;
                }
                const auto prefix = import.name.substr(0, import.name.find('.'));
                const auto& local = scope(file.scope);
                if (local.values.contains(prefix) || local.types.contains(prefix)) {
                    report(NameResolutionDiagnosticId::importCollision, import.span,
                           "import '" + import.name + "' collides with declaration '" + prefix + "'");
                }
                imports.emplace(import.name, target->second);
            }
        }
        for (const auto& file : files) {
            activeModule = file.module;
            model->fileScope_ = file.scope;
            collectMembers(file.file);
        }
        for (const auto& file : files) {
            activeModule = file.module;
            model->fileScope_ = file.scope;
            for (const auto& item : file.file->items) resolveTopLevelSignature(item);
        }
        validateExposedTypes();
        for (const auto& file : files) {
            activeModule = file.module;
            model->fileScope_ = file.scope;
            for (const auto& item : file.file->items) resolveTopLevelBody(item);
        }
        if (!files.empty()) model->fileScope_ = files.front().scope;
        return {model, std::move(diagnostics)};
    }

private:
    struct NameLookupResult {
        std::optional<SymbolId> symbol;
        std::vector<std::string> ambiguousCandidates;
    };

    SemanticModel::Ptr model;
    std::vector<NameResolutionDiagnostic> diagnostics;
    uint32_t activeModule = 0;
    // The type `Self` denotes while resolving the signature or body of a member.
    std::optional<SymbolId> selfType;
    // The type parameters of the generic struct, enum or extension whose methods are being resolved
    // (spec 20.12): each method scope can name them.
    std::vector<SymbolId> ambientTypeParameters;
    std::unordered_map<std::string, ScopeId> namedModules;
    std::unordered_map<ScopeId, std::unordered_map<std::string, ScopeId>> fileImports;

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
                for (const auto& parameter : value->genericParameters) indexNode(parameter);
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
                for (const auto& parameter : value->genericParameters) indexNode(parameter);
                for (const auto& field : value->fields) indexNode(field);
                for (const auto& method : value->methods) indexNode(method);
                indexNode(value->deinitializer);
                break;
            }
            case Kind::extensionDecl: {
                const auto value = std::static_pointer_cast<syntax::ExtensionDeclSyntax>(node);
                indexNode(value->extendedType);
                for (const auto& conformance : value->conformances) indexNode(conformance);
                for (const auto& method : value->methods) indexNode(method);
                break;
            }
            case Kind::protocolDecl: {
                const auto value = std::static_pointer_cast<syntax::ProtocolDeclSyntax>(node);
                for (const auto& requirement : value->requirements) indexNode(requirement);
                break;
            }
            case Kind::genericParameter: {
                const auto value = std::static_pointer_cast<syntax::GenericParameterSyntax>(node);
                for (const auto& constraint : value->constraints) indexNode(constraint);
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
                for (const auto& parameter : value->genericParameters) indexNode(parameter);
                for (const auto& enumCase : value->cases) indexNode(enumCase);
                for (const auto& method : value->methods) indexNode(method);
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
            case Kind::functionType: {
                const auto value = std::static_pointer_cast<syntax::FunctionTypeSyntax>(node);
                for (const auto& parameter : value->parameters) indexNode(parameter.type);
                indexNode(value->result);
                break;
            }
            case Kind::parenthesizedExpr:
                indexNode(std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(node)->expression);
                break;
            case Kind::prefixExpr: {
                const auto prefix = std::static_pointer_cast<syntax::PrefixExprSyntax>(node);
                indexNode(prefix->operand);
                if (prefix->castTarget != nullptr) indexNode(prefix->castTarget);
                break;
            }
            case Kind::accessExpr:
                indexNode(std::static_pointer_cast<syntax::AccessExprSyntax>(node)->operand);
                break;
            case Kind::propagateExpr:
                indexNode(std::static_pointer_cast<syntax::PropagateExprSyntax>(node)->operand);
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
            case Kind::unitType:
            case Kind::unitExpr:
            case Kind::unitPattern:
            case Kind::wildcardPattern:
            case Kind::literalPattern:
            case Kind::bindingPattern:
            case Kind::breakStmt:
            case Kind::continueStmt:
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
        value.moduleId = activeModule;
        value.isRootModule = activeModule == 0;
        if (declaration.has_value()) {
            value.visibility = model->node(*declaration)->visibility;
        }

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
        // The builtin marker protocol of spec 20.5: no requirements, satisfied by copyable types.
        const auto copyable = declareSymbol(
                model->preludeScope_, SymbolNamespace::type, SymbolKind::protocol, "Copyable", {});
        symbol(copyable).memberScope =
                createScope(ScopeKind::typeMembers, model->preludeScope_, std::nullopt);
        const auto intType = declareBuiltinType("Int");
        declareBuiltinType("Bool");
        const auto stringType = declareBuiltinType("String");
        const auto byteType = declareBuiltinType("UInt8");
        declareBuiltinType("Any");
        const auto arrayType = declareBuiltinType("Array");
        const auto dictionaryType = declareBuiltinType("Dict");
        const auto optionalType = declareBuiltinType("Optional");
        const auto resultType = declareBuiltinType("Result");
        const auto joinHandleType = declareBuiltinType("JoinHandle");
        const auto channelSenderType = declareBuiltinType("ChannelSender");
        const auto channelReceiverType = declareBuiltinType("ChannelReceiver");
        const auto ioErrorType = declareBuiltinType("IOError");

        declareBuiltinMember(stringType, SymbolKind::builtinMember, "count", intType);
        declareBuiltinMember(arrayType, SymbolKind::builtinMember, "count", intType);
        declareBuiltinMember(dictionaryType, SymbolKind::builtinMember, "count", intType);
        declareBuiltinMember(
                dictionaryType,
                SymbolKind::builtinMember,
                "get",
                optionalType,
                CallableSignature {
                    CallableKind::function,
                    true,
                    { CallableParameter {
                        std::string("key"),
                        true,
                        std::nullopt,
                        std::nullopt,
                        std::nullopt,
                    } },
                });
        CallableSignature utf8Signature { CallableKind::function, true, {} };
        declareBuiltinMember(
            stringType,
            SymbolKind::builtinMember,
            "utf8",
            arrayType,
            std::move(utf8Signature));
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

        // `removeLast()` gives back the last element as `Optional<T>`, or `.None` when empty; the
        // element type comes from the receiver, as for `Dict.get`.
        const auto removeLast = declareBuiltinMember(
                arrayType,
                SymbolKind::builtinMember,
                "removeLast",
                optionalType,
                CallableSignature { CallableKind::function, true, {} });
        symbol(removeLast).isMutable = true;

        // `consume a.consumeEmpty()` ends an empty array (spec 2.6.4). The receiver is consumed, so it
        // is spelled in the signature the way a user method's `consuming` receiver is.
        declareBuiltinMember(
                arrayType,
                SymbolKind::builtinMember,
                "consumeEmpty",
                voidType,
                CallableSignature {
                    CallableKind::function,
                    true,
                    {},
                    CallableReceiver { std::nullopt, syntax::AccessEffect::consuming, std::nullopt },
                });

        // `consume handle.join()` waits for the task and gives back its result (spec 21.3). Like
        // `consumeEmpty`, the receiver is consumed; the result type is the handle's argument, computed
        // by the type checker.
        declareBuiltinMember(
                joinHandleType,
                SymbolKind::builtinMember,
                "join",
                voidType,
                CallableSignature {
                    CallableKind::function,
                    true,
                    {},
                    CallableReceiver { std::nullopt, syntax::AccessEffect::consuming, std::nullopt },
                });

        // The ends of a channel (spec 21.9). `send` and `receive` borrow their end, `close` consumes it;
        // the type checker computes their result types from the end's element type.
        declareBuiltinMember(
                channelSenderType,
                SymbolKind::builtinMember,
                "send",
                voidType,
                CallableSignature {
                    CallableKind::function,
                    true,
                    { CallableParameter {
                        std::string("value"), true, std::nullopt, std::nullopt, std::nullopt,
                        syntax::AccessEffect::consuming } },
                });
        declareBuiltinMember(
                channelReceiverType,
                SymbolKind::builtinMember,
                "receive",
                voidType,
                CallableSignature { CallableKind::function, true, {} });
        for (const auto end : { channelSenderType, channelReceiverType }) {
            declareBuiltinMember(
                    end,
                    SymbolKind::builtinMember,
                    "close",
                    voidType,
                    CallableSignature {
                        CallableKind::function,
                        true,
                        {},
                        CallableReceiver { std::nullopt, syntax::AccessEffect::consuming, std::nullopt },
                    });
        }

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

        CallableSignature ioErrorCase {
            CallableKind::enumCase,
            true,
            { CallableParameter {
                std::nullopt,
                true,
                std::nullopt,
                std::nullopt,
                intType,
            } },
        };
        for (const auto* name : {
                 "NotFound", "PermissionDenied", "InvalidPath", "Other",
             }) {
            declareBuiltinMember(
                    ioErrorType,
                    SymbolKind::builtinEnumCase,
                    name,
                    ioErrorType,
                    ioErrorCase);
        }

        for (const auto& descriptor : hostbuiltins::enumerations) {
            const auto type = declareBuiltinType(std::string(descriptor.name));
            for (const auto name : descriptor.cases) {
                declareBuiltinMember(
                        type, SymbolKind::builtinEnumCase, std::string(name), type,
                        descriptor.integerPayload ? ioErrorCase : noPayload);
            }
        }

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

        // `channel(capacity: n, sender: &s, receiver: &r)` creates a channel (spec 21.9.1). Both ends are
        // `initializing` outputs; the type checker requires a sender and a receiver of one element type.
        const auto channel = declareSymbol(
                model->preludeScope_,
                SymbolNamespace::value,
                SymbolKind::builtinFunction,
                "channel",
                {});
        symbol(channel).declaredType = voidType;
        symbol(channel).callable = CallableSignature {
            CallableKind::function,
            true,
            {
                CallableParameter { std::string("capacity"), true, std::nullopt, std::nullopt, intType },
                CallableParameter {
                    std::string("sender"), true, std::nullopt, std::nullopt, std::nullopt,
                    syntax::AccessEffect::initializing },
                CallableParameter {
                    std::string("receiver"), true, std::nullopt, std::nullopt, std::nullopt,
                    syntax::AccessEffect::initializing },
            },
        };

        // `spawn(function: f, input: consume x)` starts a task (spec 21.2). Its parameters only fix the
        // labels, their order and that `input` is consumed; the type checker derives the result type from
        // the named function.
        CallableSignature spawnSignature {
            CallableKind::function,
            true,
            {
                CallableParameter { std::string("function"), true, std::nullopt, std::nullopt, std::nullopt },
                CallableParameter {
                    std::string("input"), true, std::nullopt, std::nullopt, std::nullopt,
                    syntax::AccessEffect::consuming },
            },
        };
        const auto spawn = declareSymbol(
                model->preludeScope_,
                SymbolNamespace::value,
                SymbolKind::builtinFunction,
                "spawn",
                {});
        symbol(spawn).declaredType = voidType;
        symbol(spawn).callable = std::move(spawnSignature);

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

        for (const auto* name : {"Int8", "Int16", "Int32", "Int64", "UInt",
                "UInt16", "UInt32", "UInt64", "Float", "Double"}) {
            declareBuiltinType(name);
        }
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
                case syntax::Kind::protocolDecl:
                    collectProtocol(std::static_pointer_cast<syntax::ProtocolDeclSyntax>(item));
                    break;
                default:
                    break;
            }
        }
    }

    // Runs after every file's types are declared and imports are known, so an
    // extension may precede, or live in another file of the module than, its type.
    void collectMembers(const syntax::SourceFileSyntax::Ptr& root) {
        for (const auto& item : root->items) {
            switch (item->kind) {
                case syntax::Kind::structDecl: {
                    const auto declaration =
                            std::static_pointer_cast<syntax::StructDeclSyntax>(item);
                    const auto type = declaredSymbolId(declaration);
                    if (type.has_value()) collectMethods(*type, structureMembers(declaration));
                    break;
                }
                case syntax::Kind::enumDecl: {
                    const auto declaration =
                            std::static_pointer_cast<syntax::EnumDeclSyntax>(item);
                    const auto type = declaredSymbolId(declaration);
                    if (type.has_value()) collectMethods(*type, declaration->methods);
                    break;
                }
                case syntax::Kind::extensionDecl:
                    collectExtension(std::static_pointer_cast<syntax::ExtensionDeclSyntax>(item));
                    break;
                case syntax::Kind::protocolDecl: {
                    const auto declaration =
                            std::static_pointer_cast<syntax::ProtocolDeclSyntax>(item);
                    const auto protocol = declaredSymbolId(declaration);
                    if (!protocol.has_value()) break;
                    collectMethods(*protocol, declaration->requirements);
                    // A requirement has the visibility of its protocol (spec 20.2).
                    for (const auto& requirement : declaration->requirements) {
                        const auto id = declaredSymbolId(requirement);
                        if (id.has_value()) symbol(*id).visibility = symbol(*protocol).visibility;
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }

    // A struct's `deinit` is a member function with an `inout` receiver and no parameters
    // (decision 0015). `deinit` is a keyword, so the member cannot be named or called.
    static std::vector<syntax::FunctionDeclSyntax::Ptr> structureMembers(
            const syntax::StructDeclSyntax::Ptr& declaration) {
        auto members = declaration->methods;
        if (declaration->deinitializer != nullptr) members.push_back(declaration->deinitializer);
        return members;
    }

    void collectExtension(const syntax::ExtensionDeclSyntax::Ptr& extension) {
        setContainingScope(extension, model->fileScope_);
        const auto target = resolveExtensionTarget(extension);
        if (!target.has_value()) return;
        const auto& type = symbol(*target);
        const auto isUserType = type.kind == SymbolKind::structure ||
                type.kind == SymbolKind::enumeration;
        if (extension->extendedType->kind != syntax::Kind::nominalType ||
            !isUserType || !type.memberScope.has_value() || type.moduleId != activeModule) {
            report(NameResolutionDiagnosticId::invalidExtensionTarget,
                   extension->extendedType->span,
                   "an extension must extend a struct or enum declared in the same module");
            return;
        }
        collectMethods(*target, extension->methods);
    }

    // `extension Box<T>` (spec 20.12.2): when the extended type is a generic struct or enum, each
    // unresolvable bare argument name declares a type parameter local to the extension, so the header
    // is not looked up as concrete types. An argument that already names a type is left for the type
    // checker to reject as a specialization.
    std::optional<SymbolId> resolveExtensionTarget(const syntax::ExtensionDeclSyntax::Ptr& extension) {
        const auto& extended = extension->extendedType;
        if (extended != nullptr && extended->kind == syntax::Kind::nominalType) {
            const auto nominal = std::static_pointer_cast<syntax::NominalTypeSyntax>(extended);
            if (!nominal->arguments.empty()) {
                const auto head = lookupType(model->fileScope_, tokenText(nominal->name)).symbol;
                if (head.has_value() && !symbol(*head).typeParameters.empty()) {
                    setContainingScope(extended, model->fileScope_);
                    model->referenceSymbols_[nodeId(extended)] = *head;
                    const auto scopeId = createScope(
                            ScopeKind::typeMembers, model->fileScope_, nodeId(extension));
                    model->introducedScopes_[nodeId(extension)] = scopeId;
                    auto& declared = model->extensionTypeParameters_[nodeId(extension)];
                    for (const auto& argument : nominal->arguments) {
                        setContainingScope(argument, scopeId);
                        const auto bare = argument->kind == syntax::Kind::nominalType
                                ? std::static_pointer_cast<syntax::NominalTypeSyntax>(argument)
                                : nullptr;
                        if (bare != nullptr && bare->arguments.empty() &&
                            !lookupType(model->fileScope_, tokenText(bare->name)).symbol.has_value() &&
                            tokenText(bare->name) != "Self") {
                            const auto id = declareSymbol(
                                    scopeId,
                                    SymbolNamespace::type,
                                    SymbolKind::typeParameter,
                                    tokenText(bare->name),
                                    tokenSpan(bare->name, bare->span),
                                    nodeId(argument),
                                    true,
                                    *head);
                            // The requirements of the declaration's constraint at this position.
                            const auto position = declared.size();
                            symbol(id).memberScope = position < symbol(*head).typeParameters.size()
                                    ? symbol(symbol(*head).typeParameters[position]).memberScope
                                    : std::optional<ScopeId>(createScope(
                                            ScopeKind::typeMembers, scopeId, nodeId(argument)));
                            model->referenceSymbols_[nodeId(argument)] = id;
                            declared.push_back(id);
                        } else {
                            resolveType(argument, model->fileScope_);
                        }
                    }
                    return head;
                }
            }
        }
        return resolveType(extended, model->fileScope_);
    }

    void collectMethods(
            SymbolId type,
            const std::vector<syntax::FunctionDeclSyntax::Ptr>& methods) {
        if (!symbol(type).memberScope.has_value()) return;
        const auto members = *symbol(type).memberScope;
        for (const auto& method : methods) {
            CallableSignature signature { CallableKind::function, true, {}, std::nullopt };
            for (const auto& parameter : method->parameters) {
                if (parameter->isReceiver) {
                    signature.receiver = CallableReceiver {
                        nodeId(parameter),
                        parameter->accessEffect(),
                        std::nullopt,
                    };
                    continue;
                }
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
                    members,
                    SymbolNamespace::value,
                    SymbolKind::method,
                    tokenText(method->name),
                    tokenSpan(method->name, method->span),
                    nodeId(method),
                    true,
                    type);
            symbol(id).callable = std::move(signature);
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

    void collectProtocol(const syntax::ProtocolDeclSyntax::Ptr& declaration) {
        rejectReservedTypeName(declaration->name, declaration->span);
        const auto protocol = declareSymbol(
                model->fileScope_,
                SymbolNamespace::type,
                SymbolKind::protocol,
                tokenText(declaration->name),
                tokenSpan(declaration->name, declaration->span),
                nodeId(declaration));
        symbol(protocol).memberScope = createScope(
                ScopeKind::typeMembers, model->fileScope_, nodeId(declaration));
    }

    // `<T: P & Q>` (spec 20.4): each parameter is a type in the function's scope. Its member scope
    // holds the requirements of its constraint protocols; when two protocols require one name, the
    // first is kept and the type checker compares them.
    void declareGenericParameters(
            const std::vector<syntax::GenericParameterSyntax::Ptr>& parameters,
            SymbolId owner,
            ScopeId parameterScope) {
        const auto ids = declareTypeParameterSymbols(parameters, owner, parameterScope);
        resolveTypeParameterConstraints(parameters, ids, parameterScope);
    }

    // Declares one `typeParameter` symbol per parameter in `parameterScope`, in order. A struct or
    // enum does this when it is collected, before every protocol is known; the constraints follow in
    // resolveTypeParameterConstraints.
    std::vector<SymbolId> declareTypeParameterSymbols(
            const std::vector<syntax::GenericParameterSyntax::Ptr>& parameters,
            SymbolId owner,
            ScopeId parameterScope) {
        std::vector<SymbolId> ids;
        for (const auto& parameter : parameters) {
            setContainingScope(parameter, parameterScope);
            rejectReservedTypeName(parameter->name, parameter->span);
            const auto id = declareSymbol(
                    parameterScope,
                    SymbolNamespace::type,
                    SymbolKind::typeParameter,
                    tokenText(parameter->name),
                    tokenSpan(parameter->name, parameter->span),
                    nodeId(parameter),
                    true,
                    owner);
            symbol(id).memberScope = createScope(
                    ScopeKind::typeMembers, parameterScope, nodeId(parameter));
            ids.push_back(id);
        }
        return ids;
    }

    void resolveTypeParameterConstraints(
            const std::vector<syntax::GenericParameterSyntax::Ptr>& parameters,
            const std::vector<SymbolId>& ids,
            ScopeId) {
        for (size_t index = 0; index < parameters.size() && index < ids.size(); ++index) {
            const auto members = *symbol(ids[index]).memberScope;
            for (const auto& constraint : parameters[index]->constraints) {
                const auto protocol = resolveType(constraint, model->fileScope_);
                if (!protocol.has_value() || symbol(*protocol).kind != SymbolKind::protocol ||
                    !symbol(*protocol).memberScope.has_value()) {
                    continue;
                }
                for (const auto& [name, requirement] : scope(*symbol(*protocol).memberScope).values) {
                    scope(members).values.emplace(name, requirement);
                }
            }
        }
    }

    void rejectReservedTypeName(const Token::Ptr& name, SourceSpan fallback) {
        if (tokenText(name) != "Self") return;
        report(NameResolutionDiagnosticId::reservedName, tokenSpan(name, fallback),
               "'Self' is reserved and cannot name a declaration");
    }

    void collectStructure(const syntax::StructDeclSyntax::Ptr& declaration) {
        rejectReservedTypeName(declaration->name, declaration->span);
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
        symbol(type).typeParameters =
                declareTypeParameterSymbols(declaration->genericParameters, type, members);

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
        for (const auto& field : declaration->fields) {
            symbol(initializer).visibility = std::min(
                    symbol(initializer).visibility, field->visibility);
        }
        symbol(type).synthesizedInitializer = initializer;
        model->declarationSymbols_[nodeId(declaration)] = type;
    }

    void collectEnumeration(const syntax::EnumDeclSyntax::Ptr& declaration) {
        rejectReservedTypeName(declaration->name, declaration->span);
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
        symbol(type).typeParameters =
                declareTypeParameterSymbols(declaration->genericParameters, type, members);

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
            symbol(caseSymbol).visibility = symbol(type).visibility;
        }
    }

    void validateExposedType(const syntax::TypePtr& type, const Symbol& declaration) {
        if (type == nullptr) return;
        const auto referenced = referencedSymbol(type);
        if (referenced.has_value() &&
            symbol(*referenced).kind != SymbolKind::typeParameter &&
            symbol(*referenced).visibility < declaration.visibility) {
            report(NameResolutionDiagnosticId::inaccessibleExposedType, type->span,
                   "declaration '" + declaration.name + "' exposes less accessible type '" +
                   symbol(*referenced).name + "'");
        }
        switch (type->kind) {
            case syntax::Kind::nominalType:
                for (const auto& argument :
                     std::static_pointer_cast<syntax::NominalTypeSyntax>(type)->arguments) {
                    validateExposedType(argument, declaration);
                }
                break;
            case syntax::Kind::arrayType:
                validateExposedType(
                        std::static_pointer_cast<syntax::ArrayTypeSyntax>(type)->element,
                        declaration);
                break;
            case syntax::Kind::dictionaryType: {
                const auto dictionary = std::static_pointer_cast<syntax::DictionaryTypeSyntax>(type);
                validateExposedType(dictionary->key, declaration);
                validateExposedType(dictionary->value, declaration);
                break;
            }
            case syntax::Kind::optionalType:
                validateExposedType(
                        std::static_pointer_cast<syntax::OptionalTypeSyntax>(type)->wrapped,
                        declaration);
                break;
            case syntax::Kind::functionType: {
                const auto function = std::static_pointer_cast<syntax::FunctionTypeSyntax>(type);
                for (const auto& parameter : function->parameters) validateExposedType(parameter.type, declaration);
                validateExposedType(function->result, declaration);
                break;
            }
            default: break;
        }
    }

    void validateExposedTypes() {
        for (const auto& declaration : model->symbols_) {
            if (!declaration.declaration.has_value() ||
                declaration.visibility == syntax::Visibility::private_) continue;
            const auto& node = model->node(*declaration.declaration);
            if (declaration.kind == SymbolKind::function) {
                const auto function = std::static_pointer_cast<syntax::FunctionDeclSyntax>(node);
                validateExposedType(function->returnType, declaration);
                for (const auto& parameter : function->parameters) {
                    validateExposedType(parameter->type, declaration);
                }
                // Constraint protocols are exposed names of a generic signature (spec 12.2).
                for (const auto& parameter : function->genericParameters) {
                    for (const auto& constraint : parameter->constraints) {
                        validateExposedType(constraint, declaration);
                    }
                }
            } else if (declaration.kind == SymbolKind::method) {
                // A member is never more accessible than the type that owns it.
                auto exposed = declaration;
                if (declaration.containingSymbol.has_value()) {
                    exposed.visibility = std::min(exposed.visibility,
                            symbol(*declaration.containingSymbol).visibility);
                }
                const auto function = std::static_pointer_cast<syntax::FunctionDeclSyntax>(node);
                validateExposedType(function->returnType, exposed);
                for (const auto& parameter : function->parameters) {
                    validateExposedType(parameter->type, exposed);
                }
            } else if (declaration.kind == SymbolKind::structureField) {
                auto exposed = declaration;
                if (declaration.containingSymbol.has_value()) {
                    exposed.visibility = std::min(exposed.visibility,
                            symbol(*declaration.containingSymbol).visibility);
                }
                validateExposedType(
                        std::static_pointer_cast<syntax::StructFieldDeclSyntax>(node)->type,
                        exposed);
            } else if (declaration.kind == SymbolKind::enumCase) {
                for (const auto& payload :
                     std::static_pointer_cast<syntax::EnumCaseDeclSyntax>(node)->associatedTypes) {
                    validateExposedType(payload->type, declaration);
                }
            } else if (declaration.kind == SymbolKind::binding &&
                       scope(declaration.ownerScope).kind == ScopeKind::file) {
                validateExposedType(
                        std::static_pointer_cast<syntax::BindingDeclSyntax>(node)->annotation,
                        declaration);
            }
        }
    }

    bool requireAccessible(SymbolId target, SourceSpan use) {
        if (model->isAccessible(target, use)) return true;
        report(NameResolutionDiagnosticId::inaccessibleDeclaration, use,
               "declaration '" + symbol(target).name + "' is not accessible from this file");
        return false;
    }

    void reportLookupFailure(
            const NameLookupResult& lookup,
            const std::string& name,
            SourceSpan use,
            bool typeOnly) {
        if (!lookup.ambiguousCandidates.empty()) {
            std::ostringstream message;
            message << "ambiguous imported " << (typeOnly ? "type" : "name")
                    << " '" << name << "'; use a module-qualified name: ";
            for (size_t index = 0; index < lookup.ambiguousCandidates.size(); ++index) {
                if (index != 0) message << ", ";
                message << "'" << lookup.ambiguousCandidates[index] << "'";
            }
            report(NameResolutionDiagnosticId::ambiguousImport, use, message.str());
            return;
        }
        report(typeOnly ? NameResolutionDiagnosticId::undefinedType
                        : NameResolutionDiagnosticId::undefinedName,
               use, "cannot find " + std::string(typeOnly ? "type " : "") +
                       "'" + name + "' in scope");
    }

    std::optional<SymbolId> lookupQualified(
            ScopeId currentScope, const std::string& name, bool typeOnly) const {
        const auto separator = name.rfind('.');
        if (separator == std::string::npos) return std::nullopt;
        auto current = std::optional<ScopeId>(currentScope);
        while (current.has_value()) {
            const auto imports = fileImports.find(*current);
            if (imports != fileImports.end()) {
                const auto module = imports->second.find(name.substr(0, separator));
                if (module == imports->second.end()) return std::nullopt;
                const auto& exports = scope(module->second);
                const auto member = name.substr(separator + 1);
                const auto type = exports.types.find(member);
                if (type != exports.types.end()) return type->second;
                if (!typeOnly) {
                    const auto value = exports.values.find(member);
                    if (value != exports.values.end()) return value->second;
                }
                return std::nullopt;
            }
            current = scope(*current).parent;
        }
        return std::nullopt;
    }

    std::string qualifiedName(const syntax::ExprPtr& expression) const {
        if (expression->kind == syntax::Kind::nameExpr) {
            return tokenText(std::static_pointer_cast<syntax::NameExprSyntax>(expression)->name);
        }
        if (expression->kind == syntax::Kind::memberExpr) {
            const auto member = std::static_pointer_cast<syntax::MemberExprSyntax>(expression);
            const auto base = qualifiedName(member->base);
            if (!base.empty()) return base + "." + tokenText(member->member);
        }
        return {};
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
            case syntax::Kind::structDecl: {
                const auto declaration =
                        std::static_pointer_cast<syntax::StructDeclSyntax>(node);
                resolveStructureSignature(declaration);
                resolveMethodSignatures(declaredSymbolId(declaration), structureMembers(declaration));
                break;
            }
            case syntax::Kind::enumDecl: {
                const auto declaration =
                        std::static_pointer_cast<syntax::EnumDeclSyntax>(node);
                resolveEnumerationSignature(declaration);
                resolveMethodSignatures(declaredSymbolId(declaration), declaration->methods);
                break;
            }
            case syntax::Kind::extensionDecl: {
                const auto declaration =
                        std::static_pointer_cast<syntax::ExtensionDeclSyntax>(node);
                resolveMethodSignatures(
                        referencedSymbol(declaration->extendedType), declaration->methods,
                        &model->extensionTypeParameters_[nodeId(declaration)]);
                for (const auto& conformance : declaration->conformances) {
                    resolveType(conformance, model->fileScope_);
                }
                break;
            }
            case syntax::Kind::protocolDecl: {
                const auto declaration =
                        std::static_pointer_cast<syntax::ProtocolDeclSyntax>(node);
                resolveMethodSignatures(declaredSymbolId(declaration), declaration->requirements);
                break;
            }
            default:
                break;
        }
    }

    // `ambient` are the type parameters the methods may name: the owner's own, or those an
    // extension header declares.
    void resolveMethodSignatures(
            std::optional<SymbolId> owner,
            const std::vector<syntax::FunctionDeclSyntax::Ptr>& methods,
            const std::vector<SymbolId>* ambient = nullptr) {
        if (!owner.has_value()) return;
        const auto previous = ambientTypeParameters;
        ambientTypeParameters = ambient != nullptr ? *ambient : symbol(*owner).typeParameters;
        for (const auto& method : methods) resolveFunctionSignature(method, owner);
        ambientTypeParameters = previous;
    }

    void resolveMethodBodies(
            std::optional<SymbolId> owner,
            const std::vector<syntax::FunctionDeclSyntax::Ptr>& methods) {
        if (!owner.has_value()) return;
        const auto previous = selfType;
        selfType = owner;
        for (const auto& method : methods) {
            const auto functionScope = introducedScopeId(method);
            if (functionScope.has_value()) resolveBlock(method->body, *functionScope);
        }
        selfType = previous;
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
                resolveMethodBodies(structure, structureMembers(declaration));
                break;
            }
            case syntax::Kind::enumDecl: {
                const auto declaration =
                        std::static_pointer_cast<syntax::EnumDeclSyntax>(node);
                resolveMethodBodies(declaredSymbolId(declaration), declaration->methods);
                break;
            }
            case syntax::Kind::extensionDecl: {
                const auto declaration =
                        std::static_pointer_cast<syntax::ExtensionDeclSyntax>(node);
                resolveMethodBodies(
                        referencedSymbol(declaration->extendedType), declaration->methods);
                break;
            }
            case syntax::Kind::protocolDecl: {
                // A requirement has no body; one written anyway is resolved so the type checker can
                // report it without cascading errors.
                const auto declaration =
                        std::static_pointer_cast<syntax::ProtocolDeclSyntax>(node);
                resolveMethodBodies(declaredSymbolId(declaration), declaration->requirements);
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
            const syntax::FunctionDeclSyntax::Ptr& declaration,
            std::optional<SymbolId> owner = std::nullopt) {
        const auto previousSelf = selfType;
        selfType = owner;
        resolveFunctionSignatureInSelfContext(declaration);
        selfType = previousSelf;
    }

    void resolveFunctionSignatureInSelfContext(
            const syntax::FunctionDeclSyntax::Ptr& declaration) {
        setContainingScope(declaration, model->fileScope_);
        const auto functionSymbol = declaredSymbolId(declaration);
        if (!functionSymbol.has_value()) return;
        const auto functionScope = createScope(
                ScopeKind::function,
                model->fileScope_,
                nodeId(declaration));
        model->introducedScopes_[nodeId(declaration)] = functionScope;

        declareGenericParameters(declaration->genericParameters, *functionSymbol, functionScope);
        for (const auto parameter : ambientTypeParameters) {
            scope(functionScope).types.emplace(symbol(parameter).name, parameter);
        }

        // The receiver is not a call parameter; the rest keep their written order.
        size_t callableIndex = 0;
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
                    parameter->accessEffect() == syntax::AccessEffect::consuming ||
                    parameter->accessEffect() == syntax::AccessEffect::initializing;
            symbol(parameterSymbol).declaredType = parameterType;

            auto& callable = *symbol(*functionSymbol).callable;
            if (parameter->isReceiver) {
                if (callable.receiver.has_value()) callable.receiver->type = parameterType;
                continue;
            }
            if (callableIndex < callable.parameters.size()) {
                callable.parameters[callableIndex].type = parameterType;
            }
            ++callableIndex;
        }

        auto returnType = resolveType(declaration->returnType, functionScope);
        if (!returnType.has_value()) {
            returnType = lookupType(model->preludeScope_, "Void").symbol;
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
        resolveTypeParameterConstraints(
                declaration->genericParameters, symbol(*structure).typeParameters, memberScope);

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
        resolveTypeParameterConstraints(
                declaration->genericParameters, symbol(*enumeration).typeParameters, memberScope);

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
            case syntax::Kind::unitType:
                return bindBuiltinType(type, model->preludeScope_, "Void");
            case syntax::Kind::nominalType: {
                const auto nominal = std::static_pointer_cast<syntax::NominalTypeSyntax>(type);
                for (const auto& argument : nominal->arguments) {
                    resolveType(argument, currentScope);
                }
                if (tokenText(nominal->name) == "Self") {
                    if (!selfType.has_value()) {
                        report(NameResolutionDiagnosticId::selfOutsideType, type->span,
                               "'Self' is valid only inside a struct, enum, or extension");
                        return std::nullopt;
                    }
                    model->referenceSymbols_[nodeId(type)] = *selfType;
                    return selfType;
                }
                const auto lookup = lookupType(currentScope, tokenText(nominal->name));
                if (!lookup.symbol.has_value()) {
                    reportLookupFailure(lookup, tokenText(nominal->name),
                            tokenSpan(nominal->name, nominal->span), true);
                    return std::nullopt;
                }
                if (!requireAccessible(*lookup.symbol, type->span)) return std::nullopt;
                model->referenceSymbols_[nodeId(type)] = *lookup.symbol;
                return lookup.symbol;
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
            case syntax::Kind::functionType: {
                // A function type is structural (spec 2.3.1): it names no symbol of its own.
                const auto function = std::static_pointer_cast<syntax::FunctionTypeSyntax>(type);
                for (const auto& parameter : function->parameters) resolveType(parameter.type, currentScope);
                resolveType(function->result, currentScope);
                return std::nullopt;
            }
            default:
                return std::nullopt;
        }
    }

    std::optional<SymbolId> bindBuiltinType(
            const syntax::TypePtr& node,
            ScopeId currentScope,
            const std::string& name) {
        const auto resolved = lookupType(currentScope, name).symbol;
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
            case syntax::Kind::breakStmt:
            case syntax::Kind::continueStmt:
                break;
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
            case syntax::Kind::unitExpr:
            case syntax::Kind::parenthesizedExpr:
            case syntax::Kind::prefixExpr:
            case syntax::Kind::accessExpr:
            case syntax::Kind::propagateExpr:
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
            case syntax::Kind::unitExpr:
                break;
            case syntax::Kind::nameExpr:
                resolveName(std::static_pointer_cast<syntax::NameExprSyntax>(expression), currentScope);
                break;
            case syntax::Kind::parenthesizedExpr:
                resolveExpression(
                        std::static_pointer_cast<syntax::ParenthesizedExprSyntax>(expression)->expression,
                        currentScope);
                break;
            case syntax::Kind::prefixExpr: {
                const auto prefix = std::static_pointer_cast<syntax::PrefixExprSyntax>(expression);
                resolveExpression(prefix->operand, currentScope);
                if (prefix->castTarget != nullptr) resolveType(prefix->castTarget, currentScope);
                break;
            }
            case syntax::Kind::accessExpr:
                resolveExpression(
                        std::static_pointer_cast<syntax::AccessExprSyntax>(expression)->operand,
                        currentScope);
                break;
            case syntax::Kind::propagateExpr:
                resolveExpression(
                        std::static_pointer_cast<syntax::PropagateExprSyntax>(expression)->operand,
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
        if (name == "Self") {
            if (!selfType.has_value()) {
                report(NameResolutionDiagnosticId::selfOutsideType, expression->span,
                       "'Self' is valid only inside a struct, enum, or extension");
                return;
            }
            model->referenceSymbols_[nodeId(expression)] = *selfType;
            return;
        }
        const auto lookup = lookupUnqualified(currentScope, name, false);
        if (!lookup.symbol.has_value()) {
            reportLookupFailure(lookup, name,
                    tokenSpan(expression->name, expression->span), false);
            return;
        }
        if (requireAccessible(*lookup.symbol, expression->span)) {
            model->referenceSymbols_[nodeId(expression)] = *lookup.symbol;
        }
    }

    void resolveMember(const syntax::MemberExprSyntax::Ptr& expression, ScopeId currentScope) {
        const auto qualified = lookupQualified(currentScope, qualifiedName(expression), false);
        if (qualified.has_value()) {
            if (requireAccessible(*qualified, expression->span)) {
                model->referenceSymbols_[nodeId(expression)] = *qualified;
                model->moduleQualified_[nodeId(expression)] = true;
            }
            return;
        }
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
        const auto baseSymbol = referencedSymbol(expression->base);
        if (type.kind == SymbolKind::protocol &&
            (!baseSymbol.has_value() || symbol(*baseSymbol).kind != SymbolKind::protocol)) {
            addDeferred(expression, DeferredResolutionKind::memberNeedsBaseType, name,
                        tokenSpan(expression->member, expression->span));
            return;
        }
        if (!type.memberScope.has_value()) {
            report(
                    NameResolutionDiagnosticId::unknownMember,
                    tokenSpan(expression->member, expression->span),
                    "type '" + type.name + "' has no member named '" + name + "'");
            return;
        }
        const auto member = lookupMember(*baseType, name);
        if (!member.has_value() && type.kind == SymbolKind::typeParameter) {
            // The type checker reports the operation the constraint does not provide (spec 20.5).
            addDeferred(
                    expression,
                    DeferredResolutionKind::memberNeedsBaseType,
                    name,
                    tokenSpan(expression->member, expression->span));
            return;
        }
        if (!member.has_value()) {
            report(
                    NameResolutionDiagnosticId::unknownMember,
                    tokenSpan(expression->member, expression->span),
                    "type '" + type.name + "' has no member named '" + name + "'");
            return;
        }
        if (requireAccessible(*member, expression->span)) {
            model->referenceSymbols_[nodeId(expression)] = *member;
        }
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

        // A value may hold a function (spec 5.6.1); its type decides whether it can be called.
        if (callee.kind == SymbolKind::binding || callee.kind == SymbolKind::parameter ||
            callee.kind == SymbolKind::patternBinding || callee.kind == SymbolKind::structureField) {
            return;
        }
        if (!symbol(target).callable.has_value()) {
            report(
                    NameResolutionDiagnosticId::notCallable,
                    expression->callee->span,
                    "'" + callee.name + "' is not callable");
            return;
        }

        if (!requireAccessible(target, expression->span)) return;
        model->callTargets_[nodeId(expression)] = target;
        for (auto& diagnostic : validateCallArguments(*expression, symbol(target))) {
            diagnostics.push_back(std::move(diagnostic));
        }
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
            const auto lookup = lookupType(currentScope, tokenText(pattern->qualifier));
            const auto qualifier = lookup.symbol;
            if (!qualifier.has_value()) {
                reportLookupFailure(lookup, tokenText(pattern->qualifier),
                        pattern->qualifier->span, true);
            } else if (requireAccessible(*qualifier, pattern->span)) {
                model->qualifierSymbols_[nodeId(pattern)] = *qualifier;
                caseSymbol = lookupMember(*qualifier, tokenText(pattern->name));
                if (!caseSymbol.has_value() || !isEnumCaseSymbol(symbol(*caseSymbol).kind)) {
                    report(
                            NameResolutionDiagnosticId::unknownEnumCase,
                            tokenSpan(pattern->name, pattern->span),
                            "enum '" + symbol(*qualifier).name + "' has no case named '" +
                                    tokenText(pattern->name) + "'");
                    caseSymbol.reset();
                } else if (requireAccessible(*caseSymbol, pattern->span)) {
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
            validateArgumentLabel(
                    callable.parameters[index].label,
                    pattern->arguments[index]->label,
                    pattern->arguments[index]->span,
                    diagnostics);
        }
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
            case syntax::Kind::unitExpr:
                return lookupType(model->preludeScope_, "Void").symbol;
            case syntax::Kind::literalExpr: {
                const auto literal = std::static_pointer_cast<syntax::LiteralExprSyntax>(expression);
                switch (literal->literal->kind) {
                    case decimalLiteral: return lookupType(model->fileScope_, "Int").symbol;
                    case floatLiteral: return lookupType(model->fileScope_, "Double").symbol;
                    case stringLiteral: return lookupType(model->fileScope_, "String").symbol;
                    case byteLiteral: return lookupType(model->fileScope_, "UInt8").symbol;
                    case booleanLiteral: return lookupType(model->fileScope_, "Bool").symbol;
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
                return lookupType(model->fileScope_, "Array").symbol;
            case syntax::Kind::dictionaryExpr:
                return lookupType(model->fileScope_, "Dict").symbol;
            case syntax::Kind::subscriptExpr: {
                const auto subscript = std::static_pointer_cast<syntax::SubscriptExprSyntax>(expression);
                const auto base = expressionType(subscript->base);
                if (base.has_value() && symbol(*base).name == "String") {
                    return lookupType(model->fileScope_, "UInt8").symbol;
                }
                return std::nullopt;
            }
            default:
                return std::nullopt;
        }
    }

    std::optional<SymbolId> lookupInScope(
            const Scope& current,
            const std::string& name,
            bool typeOnly) const {
        if (!typeOnly) {
            const auto value = current.values.find(name);
            if (value != current.values.end()) return value->second;
        }
        const auto type = current.types.find(name);
        if (type != current.types.end()) return type->second;
        return std::nullopt;
    }

    NameLookupResult lookupUnqualified(
            ScopeId currentScope,
            const std::string& name,
            bool typeOnly) const {
        auto current = std::optional<ScopeId>(currentScope);
        std::optional<ScopeId> fileScope;
        while (current.has_value()) {
            const auto& lexicalScope = scope(*current);
            const auto found = lookupInScope(lexicalScope, name, typeOnly);
            if (found.has_value()) return {found, {}};
            if (lexicalScope.kind == ScopeKind::file) fileScope = current;
            current = lexicalScope.parent;
        }
        // Imports are a file-local fallback after existing lexical/prelude lookup.
        if (!fileScope.has_value()) return {};
        const auto imports = fileImports.find(*fileScope);
        if (imports == fileImports.end()) return {};
        std::optional<SymbolId> candidate;
        std::vector<std::string> names;
        for (const auto& [moduleName, moduleScope] : imports->second) {
            const auto found = lookupInScope(scope(moduleScope), name, typeOnly);
            if (!found.has_value() ||
                symbol(*found).visibility != syntax::Visibility::public_) {
                continue;
            }
            candidate = found;
            names.push_back(moduleName + "." + name);
        }
        if (names.size() > 1) {
            std::sort(names.begin(), names.end());
            return {std::nullopt, std::move(names)};
        }
        return {candidate, {}};
    }

    NameLookupResult lookupType(
            ScopeId currentScope,
            const std::string& name) const {
        if (name.find('.') != std::string::npos) {
            return {lookupQualified(currentScope, name, true), {}};
        }
        return lookupUnqualified(currentScope, name, true);
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

NameResolutionResult NameResolver::resolve(const std::vector<ModuleInput>& modules) const {
    return NameResolutionBuilder().build(modules);
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
        case NameResolutionDiagnosticId::inaccessibleDeclaration:
            return "name-resolution.inaccessible-declaration";
        case NameResolutionDiagnosticId::inaccessibleExposedType:
            return "name-resolution.inaccessible-exposed-type";
        case NameResolutionDiagnosticId::importCollision:
            return "name-resolution.import-collision";
        case NameResolutionDiagnosticId::unknownImport:
            return "name-resolution.unknown-import";
        case NameResolutionDiagnosticId::ambiguousImport:
            return "name-resolution.ambiguous-import";
        case NameResolutionDiagnosticId::invalidExtensionTarget:
            return "name-resolution.invalid-extension-target";
        case NameResolutionDiagnosticId::selfOutsideType:
            return "name-resolution.self-outside-type";
        case NameResolutionDiagnosticId::reservedName:
            return "name-resolution.reserved-name";
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