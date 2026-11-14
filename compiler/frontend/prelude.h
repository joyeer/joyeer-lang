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

[[nodiscard]] Prelude parsePrelude(uint32_t sourceId);

}

#endif
