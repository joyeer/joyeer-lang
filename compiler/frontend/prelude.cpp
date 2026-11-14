#include "prelude.h"
#include "lexparser.h"
#include "parser.h"
#include "prelude_source.h"

namespace joyeer::semantic {

Prelude parsePrelude(uint32_t sourceId) {
    Prelude result;
    result.source = SourceFile::fromMemory(std::string(concurrencyPreludeSource),
            std::filesystem::path("lib") / "Runtime" / "concurrency.joyeer");
    Diagnostics diagnostics;
    LexParser(&diagnostics).parse(result.source);
    if (diagnostics.hasFailure()) {
        result.problem = "cannot tokenize the compiler-bundled runtime prelude";
        return result;
    }
    for (auto& token : result.source->tokens) token->span.sourceId = sourceId;
    auto parsed = parser::Parser(result.source->tokens, true, true).parse();
    if (!parsed.succeeded()) {
        result.problem = "invalid compiler-bundled runtime prelude:\n" + parser::dump(parsed.diagnostics);
        return result;
    }
    result.root = std::move(parsed.root);
    return result;
}

}
