#ifndef JOYEER_COMPILER_PRELUDE_H
#define JOYEER_COMPILER_PRELUDE_H

#include "sourcefile.h"
#include "syntax.h"

#include <string>

namespace joyeer::semantic {

struct Prelude {
    SourceFile::Ptr source;
    syntax::SourceFileSyntax::Ptr root;
    std::string problem;
};

enum class PreludeKind {
    concurrency,
    roles,
    collections,
    contracts,
};

// Parses a compiler-bundled prelude with every token carrying `sourceId`. `source` is set even when
// parsing fails, so the source table stays dense.
[[nodiscard]] Prelude parsePrelude(PreludeKind kind, uint32_t sourceId);

}

#endif
