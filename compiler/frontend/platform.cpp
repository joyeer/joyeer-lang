#include "platform.h"

#include "nameresolution.h"
#include "nativebinding.h"
#include "syntax.h"

#include <algorithm>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace joyeer::semantic {

TargetPlatform hostPlatform() {
#if defined(_WIN32)
    return TargetPlatform::windows;
#elif defined(__APPLE__)
    return TargetPlatform::macos;
#elif defined(__linux__)
    return TargetPlatform::linux;
#else
#error "Joyeer supports only Windows, Linux, and macOS hosts"
#endif
}

const char* platformName(TargetPlatform platform) {
    switch (platform) {
        case TargetPlatform::windows: return "windows";
        case TargetPlatform::linux: return "linux";
        case TargetPlatform::macos: return "macos";
    }
    return "unknown";
}

namespace {

constexpr unsigned bit(TargetPlatform platform) {
    return 1u << static_cast<unsigned>(platform);
}

constexpr unsigned posixPlatforms = bit(TargetPlatform::linux) | bit(TargetPlatform::macos);
constexpr unsigned everyPlatform = bit(TargetPlatform::windows) | posixPlatforms;

std::string describe(unsigned mask) {
    std::string result;
    for (const auto platform : {TargetPlatform::windows, TargetPlatform::linux, TargetPlatform::macos}) {
        if ((mask & bit(platform)) == 0) continue;
        if (!result.empty()) result += ", ";
        result += platformName(platform);
    }
    return result;
}

// Canonical spelling: the sugar forms and their nominal spellings are one
// type, and an omitted result is the unit type, as in the checker.
std::string typeText(const syntax::TypePtr& type) {
    if (type == nullptr) return "()";
    switch (type->kind) {
        case syntax::Kind::unitType: return "()";
        case syntax::Kind::nominalType: {
            const auto value = std::static_pointer_cast<syntax::NominalTypeSyntax>(type);
            const std::string name = value->name == nullptr ? "<missing>" : value->name->rawValue;
            const auto& arguments = value->arguments;
            if (name == "Void" && arguments.empty()) return "()";
            if (name == "Array" && arguments.size() == 1) return '[' + typeText(arguments[0]) + ']';
            if (name == "Optional" && arguments.size() == 1) return typeText(arguments[0]) + '?';
            if (name == "Dict" && arguments.size() == 2) {
                return '[' + typeText(arguments[0]) + ':' + typeText(arguments[1]) + ']';
            }
            std::string result = name;
            if (!arguments.empty()) {
                result += '<';
                for (size_t index = 0; index < arguments.size(); ++index) {
                    if (index != 0) result += ',';
                    result += typeText(arguments[index]);
                }
                result += '>';
            }
            return result;
        }
        case syntax::Kind::arrayType:
            return '[' + typeText(std::static_pointer_cast<syntax::ArrayTypeSyntax>(type)->element) + ']';
        case syntax::Kind::dictionaryType: {
            const auto value = std::static_pointer_cast<syntax::DictionaryTypeSyntax>(type);
            return '[' + typeText(value->key) + ':' + typeText(value->value) + ']';
        }
        case syntax::Kind::optionalType: {
            const auto wrapped = std::static_pointer_cast<syntax::OptionalTypeSyntax>(type)->wrapped;
            const bool function = wrapped != nullptr && wrapped->kind == syntax::Kind::functionType;
            return (function ? "(" : "") + typeText(wrapped) + (function ? ")?" : "?");
        }
        case syntax::Kind::functionType: {
            const auto value = std::static_pointer_cast<syntax::FunctionTypeSyntax>(type);
            std::string result = "(";
            for (size_t index = 0; index < value->parameters.size(); ++index) {
                if (index != 0) result += ',';
                const auto& keyword = value->parameters[index].accessKeyword;
                // A written-out `borrowing` is the default, the same type.
                if (keyword != nullptr && keyword->kind != kwBorrowing) result += keyword->rawValue + ' ';
                result += typeText(value->parameters[index].type);
            }
            return result + "):" + typeText(value->result);
        }
        default: return "<error>";
    }
}

// Everything a caller can observe: visibility, labels, access effects, types.
// Internal parameter names and bodies are deliberately excluded. The text is
// also shown to the author, so it reads as a signature.
std::string signatureText(const syntax::FunctionDeclSyntax& function) {
    std::ostringstream out;
    switch (function.visibility) {
        case syntax::Visibility::private_: out << "private "; break;
        case syntax::Visibility::public_: out << "public "; break;
        case syntax::Visibility::internal: break;
    }
    out << '(';
    for (size_t index = 0; index < function.parameters.size(); ++index) {
        const auto& parameter = function.parameters[index];
        if (index != 0) out << ", ";
        out << (parameter->label == nullptr ? "<missing>" : parameter->label->rawValue) << ": "
            << (parameter->accessKeyword == nullptr ? "borrowing" : parameter->accessKeyword->rawValue)
            << ' ' << typeText(parameter->type);
    }
    out << ") -> " << typeText(function.returnType);
    return out.str();
}

struct Variant {
    syntax::FunctionDeclSyntax::Ptr function;
    size_t file = 0;
    unsigned mask = everyPlatform;
    bool annotated = false;
    SourceSpan span;
};

// A top-level declaration that is not a function but shares the namespace.
struct OtherDeclaration {
    std::string name;
    std::string kind;
    size_t file = 0;
    bool isPrivate = false;
    SourceSpan span;
};

bool isPlatformAttribute(const syntax::AttributeSyntax& attribute) {
    return attribute.name != nullptr && attribute.name->rawValue == "platform";
}

bool isPrivate(const syntax::Node& node) {
    return node.visibility == syntax::Visibility::private_;
}

class Selector {
public:
    explicit Selector(TargetPlatform target): target(target) {}

    void run(const ModuleInput& module) {
        runtimeContext = module.runtimeContext;
        std::vector<Variant> variants;
        std::vector<OtherDeclaration> others;
        for (size_t file = 0; file < module.files.size(); ++file) {
            for (const auto& item : module.files[file]->items) {
                if (item == nullptr) continue;
                if (item->kind == syntax::Kind::functionDecl) {
                    collect(std::static_pointer_cast<syntax::FunctionDeclSyntax>(item), file, variants);
                } else {
                    collectOther(item, file, others);
                }
            }
        }
        checkGroups(variants, others);
        std::stable_sort(diagnostics.begin(), diagnostics.end(), [](const auto& left, const auto& right) {
            return left.span.sourceId != right.span.sourceId
                    ? left.span.sourceId < right.span.sourceId
                    : left.span.offset < right.span.offset;
        });
        // Leave a module with problems untouched: the compilation stops here,
        // and a repeated call reports the same problems.
        if (diagnostics.empty()) apply(module, variants);
    }

    std::vector<PlatformDiagnostic> diagnostics;

private:
    void report(PlatformDiagnosticId id, SourceSpan span, std::string message, std::string help = {},
                std::optional<SourceSpan> related = std::nullopt, std::string relatedMessage = {}) {
        diagnostics.push_back({id, span, std::move(message), std::move(help), related,
                               std::move(relatedMessage)});
    }

    // Parses the annotation's arguments. Returns false after reporting.
    bool parseMask(const syntax::AttributeSyntax& attribute, unsigned& mask) {
        if (attribute.arguments.empty()) {
            report(PlatformDiagnosticId::invalidAnnotation, attribute.span,
                   "@platform requires at least one platform name",
                   "write @platform(windows), @platform(linux, macos), or @platform(posix)");
            return false;
        }
        bool valid = true;
        mask = 0;
        for (const auto& argument : attribute.arguments) {
            const auto name = argument->value != nullptr && argument->value->kind == syntax::Kind::nameExpr
                    ? std::static_pointer_cast<syntax::NameExprSyntax>(argument->value)
                    : nullptr;
            if (argument->label != nullptr || argument->accessMarker != nullptr || name == nullptr ||
                name->name == nullptr) {
                report(PlatformDiagnosticId::invalidAnnotation, argument->span,
                       "@platform arguments must be unlabelled platform names",
                       "write @platform(windows), not @platform(os: windows) or a string");
                valid = false;
                continue;
            }
            const std::string_view text = name->name->rawValue;
            if (text == "windows") mask |= bit(TargetPlatform::windows);
            else if (text == "linux") mask |= bit(TargetPlatform::linux);
            else if (text == "macos") mask |= bit(TargetPlatform::macos);
            else if (text == "posix") mask |= posixPlatforms;
            else {
                report(PlatformDiagnosticId::unknownName, argument->span,
                       "unknown platform '" + std::string(text) + "'",
                       "use windows, linux, macos, or posix");
                valid = false;
            }
        }
        return valid;
    }

    // Checks the annotations that sit beside @platform. A function the target
    // drops never reaches the type checker, so the syntactic and context rules
    // of the C annotations are checked here, on every target. Parameter and
    // result type admissibility needs resolved types and stays with the
    // selected variant.
    bool checkCompanions(const syntax::FunctionDeclSyntax& function) {
        bool valid = true;
        const syntax::AttributeSyntax* binding = nullptr;
        bool importing = false;
        for (const auto& attribute : function.attributes) {
            if (isPlatformAttribute(attribute)) continue;
            const auto name = attribute.name == nullptr ? std::string() : attribute.name->rawValue;
            if (name != "c_extern" && name != "c_export" && name != "c_data") {
                report(PlatformDiagnosticId::invalidAnnotation, attribute.span,
                       "unknown annotation '@" + name + "'");
                valid = false;
                continue;
            }
            if (binding != nullptr) {
                report(PlatformDiagnosticId::invalidAnnotation, attribute.span,
                       "a function may have only one C binding annotation");
                valid = false;
                continue;
            }
            binding = &attribute;
            importing = name != "c_export";
            valid = checkSymbolArgument(attribute) && valid;
        }
        if (binding == nullptr) {
            if (function.body == nullptr) {
                report(PlatformDiagnosticId::invalidAnnotation, function.span,
                       "a function without a body must carry @c_extern",
                       "add a body, or @c_extern(symbol: \"...\") to import a native function");
                valid = false;
            }
            return valid;
        }
        if (!runtimeContext) {
            report(PlatformDiagnosticId::invalidAnnotation, binding->span,
                   "C bindings require an explicitly selected runtime compilation unit");
            valid = false;
        }
        if ((importing && function.body != nullptr) || (!importing && function.body == nullptr)) {
            report(PlatformDiagnosticId::invalidAnnotation, function.span,
                   importing ? "@c_extern must not have a function body"
                             : "@c_export requires a function body");
            valid = false;
        }
        if (function.returnType == nullptr) {
            report(PlatformDiagnosticId::invalidAnnotation, function.span,
                   "C bindings require an explicit result type, including Void");
            valid = false;
        }
        return valid;
    }

    bool checkSymbolArgument(const syntax::AttributeSyntax& attribute) {
        if (attribute.arguments.empty()) {
            report(PlatformDiagnosticId::invalidAnnotation, attribute.span,
                   "C annotations require a 'symbol:' string-literal argument");
            return false;
        }
        bool valid = true;
        for (size_t position = 1; position < attribute.arguments.size(); ++position) {
            const auto& extra = attribute.arguments[position];
            const auto name = extra->value != nullptr && extra->value->kind == syntax::Kind::literalExpr
                    ? std::static_pointer_cast<syntax::LiteralExprSyntax>(extra->value)
                    : nullptr;
            if (extra->label == nullptr || extra->label->rawValue != "pointer" ||
                extra->accessMarker != nullptr || name == nullptr || name->literal == nullptr ||
                name->literal->kind != stringLiteral) {
                report(PlatformDiagnosticId::invalidAnnotation, extra->span,
                       "after 'symbol:', C annotations accept only 'pointer:' followed by a "
                       "parameter label string literal");
                valid = false;
            }
        }
        const auto& argument = attribute.arguments.front();
        const auto literal = argument->value != nullptr && argument->value->kind == syntax::Kind::literalExpr
                ? std::static_pointer_cast<syntax::LiteralExprSyntax>(argument->value)
                : nullptr;
        if (argument->label == nullptr || argument->label->rawValue != "symbol" ||
            argument->accessMarker != nullptr || literal == nullptr || literal->literal == nullptr ||
            literal->literal->kind != stringLiteral) {
            report(PlatformDiagnosticId::invalidAnnotation, argument->span,
                   "C annotations require 'symbol:' followed by a string literal");
            return false;
        }
        if (!isCIdentifier(literal->literal->rawValue)) {
            report(PlatformDiagnosticId::invalidAnnotation, argument->span,
                   "native symbol must be a nonempty ASCII C identifier without linker decoration");
            return false;
        }
        return valid;
    }

    void collect(const syntax::FunctionDeclSyntax::Ptr& function, size_t file, std::vector<Variant>& variants) {
        Variant variant;
        variant.function = function;
        variant.file = file;
        variant.span = function->name == nullptr ? function->span : function->name->span;
        const auto first = std::find_if(
                function->attributes.begin(), function->attributes.end(), isPlatformAttribute);
        if (first != function->attributes.end()) {
            variant.annotated = true;
            variant.span = first->span;
            bool valid = true;
            const auto second = std::find_if(std::next(first), function->attributes.end(), isPlatformAttribute);
            if (second != function->attributes.end()) {
                report(PlatformDiagnosticId::invalidAnnotation, second->span,
                       "a function may have only one @platform annotation",
                       "list every platform in one annotation: @platform(linux, macos)");
                valid = false;
            }
            valid = checkCompanions(*function) && valid;
            valid = parseMask(*first, variant.mask) && valid;
            // A malformed annotation is already reported; keeping it out of the
            // groups avoids follow-on overlap and signature errors.
            if (!valid) return;
        }
        variants.push_back(std::move(variant));
    }

    void collectOther(const syntax::NodePtr& item, size_t file, std::vector<OtherDeclaration>& others) {
        std::string kind;
        Token::Ptr name;
        switch (item->kind) {
            case syntax::Kind::structDecl:
                kind = "struct";
                name = std::static_pointer_cast<syntax::StructDeclSyntax>(item)->name;
                break;
            case syntax::Kind::enumDecl:
                kind = "enum";
                name = std::static_pointer_cast<syntax::EnumDeclSyntax>(item)->name;
                break;
            case syntax::Kind::bindingDecl:
                kind = std::static_pointer_cast<syntax::BindingDeclSyntax>(item)->isMutable()
                        ? "variable" : "constant";
                name = std::static_pointer_cast<syntax::BindingDeclSyntax>(item)->name;
                break;
            default: return;
        }
        if (name == nullptr) return;
        others.push_back({name->rawValue, kind, file, isPrivate(*item), name->span});
    }

    void checkGroups(const std::vector<Variant>& variants, const std::vector<OtherDeclaration>& others) {
        // A private function is visible only in its own file (§12.2), so it
        // forms a group only with same-named functions of that file, unless a
        // non-private function of that name exists: then they are variants of
        // one function and must agree, visibility included.
        std::unordered_set<std::string> sharedNames;
        for (const auto& variant : variants) {
            if (variant.function->name != nullptr && !isPrivate(*variant.function)) {
                sharedNames.insert(variant.function->name->rawValue);
            }
        }
        const auto groupKey = [&](const Variant& variant) {
            const auto& name = variant.function->name->rawValue;
            return isPrivate(*variant.function) && !sharedNames.contains(name)
                    ? "#" + std::to_string(variant.file) + ":" + name
                    : name;
        };
        std::vector<std::string> order;
        std::unordered_map<std::string, std::vector<const Variant*>> groups;
        for (const auto& variant : variants) {
            if (variant.function->name == nullptr) continue;
            const auto key = groupKey(variant);
            if (!groups.contains(key)) order.push_back(key);
            groups[key].push_back(&variant);
        }
        for (const auto& key : order) {
            const auto& group = groups[key];
            if (std::none_of(group.begin(), group.end(), [](const auto* variant) { return variant->annotated; })) {
                continue;
            }
            const auto& name = group.front()->function->name->rawValue;
            const bool groupPrivate = isPrivate(*group.front()->function) &&
                    key.starts_with('#');
            std::unordered_set<size_t> files;
            for (const auto* variant : group) files.insert(variant->file);
            for (const auto& other : others) {
                // Two declarations of one name in a file always collide; across
                // files only module-scope declarations see each other (§12.2).
                const bool sameScope = files.contains(other.file) || (!groupPrivate && !other.isPrivate);
                if (other.name != name || !sameScope) continue;
                report(PlatformDiagnosticId::nameCollision, group.front()->span,
                       "platform variants of '" + name + "' collide with a " + other.kind +
                       " of the same name, which exists on every platform",
                       "rename the " + other.kind + " or the function",
                       other.span, "the " + other.kind + " is declared here");
            }
            const auto& first = *group.front();
            const auto expected = signatureText(*first.function);
            for (size_t later = 1; later < group.size(); ++later) {
                bool overlaps = false;
                for (size_t earlier = 0; earlier < later && !overlaps; ++earlier) {
                    const auto shared = group[earlier]->mask & group[later]->mask;
                    if (shared == 0) continue;
                    overlaps = true;
                    report(PlatformDiagnosticId::overlappingVariants, group[later]->span,
                           "'" + name + "' is declared more than once for " + describe(shared),
                           "give each variant a disjoint @platform list; a function without @platform applies to "
                           "every platform",
                           group[earlier]->span, "the other declaration is here");
                }
                if (overlaps) continue;
                const auto actual = signatureText(*group[later]->function);
                if (actual != expected) {
                    report(PlatformDiagnosticId::signatureMismatch, group[later]->span,
                           "platform variants of '" + name + "' must have the same signature; this one is " +
                           actual + ", the first is " + expected,
                           "labels, access effects, parameter and result types, and visibility must match",
                           first.span, "the first variant is here");
                }
            }
        }
    }

    void apply(const ModuleInput& module, const std::vector<Variant>& variants) {
        std::unordered_set<const syntax::FunctionDeclSyntax*> removed;
        for (const auto& variant : variants) {
            if (!variant.annotated) continue;
            if ((variant.mask & bit(target)) == 0) {
                removed.insert(variant.function.get());
                continue;
            }
            auto& function = *variant.function;
            function.attributes.erase(
                    std::remove_if(function.attributes.begin(), function.attributes.end(), isPlatformAttribute),
                    function.attributes.end());
            // The span began at the stripped annotation. Later diagnostics
            // should start at the next remaining annotation, or at the name.
            const auto start = !function.attributes.empty() ? function.attributes.front().span
                    : function.name != nullptr ? function.name->span : function.span;
            if (start.sourceId == function.span.sourceId && start.offset >= function.span.offset) {
                const auto end = function.span.offset + function.span.length;
                function.span.offset = start.offset;
                function.span.length = end - function.span.offset;
            }
        }
        if (removed.empty()) return;
        for (const auto& file : module.files) {
            auto& items = file->items;
            items.erase(std::remove_if(items.begin(), items.end(), [&](const syntax::NodePtr& item) {
                return item != nullptr && item->kind == syntax::Kind::functionDecl &&
                        removed.contains(static_cast<const syntax::FunctionDeclSyntax*>(item.get()));
            }), items.end());
        }
    }

    TargetPlatform target;
    bool runtimeContext = false;
};

} // namespace

std::vector<PlatformDiagnostic> selectPlatformDeclarations(
        const std::vector<ModuleInput>& modules,
        TargetPlatform target) {
    std::vector<PlatformDiagnostic> diagnostics;
    for (const auto& module : modules) {
        Selector selector(target);
        selector.run(module);
        diagnostics.insert(diagnostics.end(),
                           std::make_move_iterator(selector.diagnostics.begin()),
                           std::make_move_iterator(selector.diagnostics.end()));
    }
    return diagnostics;
}

const char* diagnosticName(PlatformDiagnosticId id) {
    switch (id) {
        case PlatformDiagnosticId::invalidAnnotation: return "platform.invalid-annotation";
        case PlatformDiagnosticId::unknownName: return "platform.unknown-name";
        case PlatformDiagnosticId::overlappingVariants: return "platform.overlapping-variants";
        case PlatformDiagnosticId::signatureMismatch: return "platform.signature-mismatch";
        case PlatformDiagnosticId::nameCollision: return "platform.name-collision";
    }
    return "platform.unknown";
}

std::string dump(const std::vector<PlatformDiagnostic>& diagnostics) {
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << diagnosticName(diagnostic.id)
            << '@' << diagnostic.span.offset << ':' << diagnostic.span.length
            << ' ' << diagnostic.message << '\n';
    }
    return out.str();
}

} // namespace joyeer::semantic
