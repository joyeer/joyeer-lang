#include "prelude.h"
#include "lexparser.h"
#include "parser.h"
#include "prelude_source.h"

namespace joyeer::semantic {

Prelude parsePrelude(PreludeKind kind, uint32_t sourceId) {
    Prelude result;
    const bool roles = kind == PreludeKind::roles;
    result.source = SourceFile::fromMemory(
            std::string(roles ? rolePreludeSource : concurrencyPreludeSource),
            std::filesystem::path("lib") / "Runtime" / (roles ? "roles.joyeer" : "concurrency.joyeer"));
    const std::string name = roles ? "role prelude" : "runtime prelude";
    Diagnostics diagnostics;
    LexParser(&diagnostics).parse(result.source);
    if (diagnostics.hasFailure()) {
        result.problem = "cannot tokenize the compiler-bundled " + name;
        return result;
    }
    for (auto& token : result.source->tokens) token->span.sourceId = sourceId;
    auto parsed = parser::Parser(result.source->tokens, true, true).parse();
    if (!parsed.succeeded()) {
        result.problem = "invalid compiler-bundled " + name + ":\n" + parser::dump(parsed.diagnostics);
        return result;
    }
    result.root = std::move(parsed.root);
    return result;
}

}
