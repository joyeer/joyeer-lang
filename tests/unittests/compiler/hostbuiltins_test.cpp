#include "compiler/frontend/hostbuiltins.h"
#include "compiler/frontend/lexparser.h"
#include "compiler/frontend/nameresolution.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/sourcefile.h"
#include "compiler/frontend/typechecking.h"
#include "compiler/diagnostics/diagnostic.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace {

// The host operations are ordinary functions in the System standard module
// (spec 18); the compiler knows none of them. It still declares the error and
// status enumerations they return, so user code and System share one definition.
class HostBuiltinTest : public testing::Test {
protected:
    void check(const std::string& text) {
        diagnostics.errors.clear();
        source = std::make_shared<SourceFile>(text);
        LexParser(&diagnostics).parse(source);
        ASSERT_TRUE(diagnostics.errors.empty());
        const auto parsed = joyeer::parser::Parser(source->tokens).parse();
        ASSERT_TRUE(parsed.succeeded()) << joyeer::parser::dump(parsed.diagnostics);
        resolution = joyeer::semantic::NameResolver().resolve(parsed.root);
    }

    Diagnostics diagnostics;
    SourceFile::Ptr source;
    joyeer::semantic::NameResolutionResult resolution;
};

TEST_F(HostBuiltinTest, DeclaresTheHostEnumerationsAsPreludeTypes) {
    ASSERT_NO_FATAL_FAILURE(check("func main() {}\n"));
    ASSERT_TRUE(resolution.succeeded()) << joyeer::semantic::dump(resolution.diagnostics);
    const auto* prelude = resolution.model->scope(resolution.model->preludeScope());
    ASSERT_NE(prelude, nullptr);
    for (const auto& enumeration : joyeer::hostbuiltins::enumerations) {
        EXPECT_NE(prelude->types.find(std::string(enumeration.name)), prelude->types.end())
                << enumeration.name;
    }
    EXPECT_NE(prelude->types.find("IOError"), prelude->types.end());
}

TEST_F(HostBuiltinTest, KnowsNoHostFunctionWithoutImportingSystem) {
    for (const auto* name : { "readFile", "readFileUtf8", "readFilePrefix", "writeFileNew",
             "createDirectory", "listDirectory", "fileKind", "removeFile", "removeDirectory",
             "joinPath", "runProcess", "writeStderr", "readStdinLine" }) {
        ASSERT_NO_FATAL_FAILURE(check("func main() {}\n"));
        ASSERT_TRUE(resolution.succeeded()) << joyeer::semantic::dump(resolution.diagnostics);
        const auto* prelude = resolution.model->scope(resolution.model->preludeScope());
        ASSERT_NE(prelude, nullptr);
        EXPECT_EQ(prelude->values.find(name), prelude->values.end()) << name;
    }
}

TEST_F(HostBuiltinTest, RejectsACallToAFormerBuiltinWithoutAnImport) {
    ASSERT_NO_FATAL_FAILURE(check(
            "func main() { let text = readFileUtf8(path: \"x\") }\n"));
    EXPECT_FALSE(resolution.succeeded());
}

}
