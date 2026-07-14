#include "joyeer/backend/llvm.h"
#include "joyeer/compiler/irlowering.h"
#include "joyeer/compiler/lexparser.h"
#include "joyeer/compiler/nameresolution.h"
#include "joyeer/compiler/parser.h"
#include "joyeer/compiler/sourcefile.h"
#include "joyeer/compiler/symtable.h"
#include "joyeer/compiler/typechecking.h"
#include "joyeer/diagnostic/diagnostic.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace {

class LLVMBackendTest : public testing::Test {
protected:
    void emit(const std::string& text) {
        lexerDiagnostics.errors.clear();
        source = std::make_shared<SourceFile>(text);
        const auto context = std::make_shared<CompileContext>(
                &lexerDiagnostics,
                std::make_shared<SymbolTable>());
        LexParser lexer(context, LexerProfile::jsonParserMvp);
        lexer.parse(source);
        ASSERT_TRUE(lexerDiagnostics.errors.empty());

        joyeer::parser::Parser parser(source->tokens);
        const auto parsing = parser.parse();
        ASSERT_TRUE(parsing.succeeded()) << joyeer::parser::dump(parsing.diagnostics);

        const auto resolution = joyeer::semantic::NameResolver().resolve(parsing.root);
        ASSERT_TRUE(resolution.succeeded()) << joyeer::semantic::dump(resolution.diagnostics);

        const auto checking = joyeer::typing::TypeChecker().check(resolution.model);
        ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);

        const auto lowering = joyeer::lowering::Lowerer().lower(checking.model, "test.joyeer");
        ASSERT_TRUE(lowering.succeeded()) << joyeer::lowering::dump(lowering.diagnostics);
        result = joyeer::llvmbackend::Emitter().emit(*lowering.module);
    }

    Diagnostics lexerDiagnostics;
    SourceFile::Ptr source;
    joyeer::llvmbackend::Result result;
};

TEST_F(LLVMBackendTest, EmitsPrimitiveFunctionsStackSlotsCallsAndControlFlow) {
    emit(R"JOYEER(func add(left: Int, right: Int): Int {
return left + right
}
func choose(flag: Bool, left: Int, right: Int): Int {
return if flag { add(left: left, right: right) } else { left * right }
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    EXPECT_NE(result.text.find("define i64 @joyeer_fn_"), std::string::npos);
    EXPECT_NE(result.text.find("alloca i64"), std::string::npos);
    EXPECT_NE(result.text.find("call i64 @joyeer_checked_add_int"), std::string::npos);
    EXPECT_NE(result.text.find("call i64 @joyeer_checked_mul_int"), std::string::npos);
    EXPECT_NE(result.text.find("br i1 %"), std::string::npos);
    EXPECT_NE(result.text.find("ret i64"), std::string::npos);
}

TEST_F(LLVMBackendTest, SpecializesPrintAndStringRuntimeDeclarations) {
    emit(R"JOYEER(func run() {
print(value: 42)
print(value: true)
print(value: b'a')
print(value: "hello")
print(value: "left" + "right")
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    EXPECT_NE(result.text.find("%joyeer.string = type { ptr, i64 }"), std::string::npos);
    EXPECT_NE(result.text.find("declare void @joyeer_print_int(i64)"), std::string::npos);
    EXPECT_NE(result.text.find("declare void @joyeer_print_bool(i1)"), std::string::npos);
    EXPECT_NE(result.text.find("declare void @joyeer_print_byte(i8)"), std::string::npos);
    EXPECT_NE(
            result.text.find("declare void @joyeer_print_string(%joyeer.string)"),
            std::string::npos);
    EXPECT_NE(
            result.text.find("declare %joyeer.string @joyeer_string_concat"),
            std::string::npos);
    EXPECT_NE(result.text.find("private unnamed_addr constant [5 x i8] c\"hello\""),
              std::string::npos);
}

TEST_F(LLVMBackendTest, EmitsSignedAndUnsignedComparisons) {
    emit(R"JOYEER(func signed(left: Int, right: Int): Bool { return left < right }
func bytes(left: UInt8, right: UInt8): Bool { return left < right }
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    EXPECT_NE(result.text.find("icmp slt i64"), std::string::npos);
    EXPECT_NE(result.text.find("icmp ult i8"), std::string::npos);
}

TEST_F(LLVMBackendTest, EmitsStructuresEnumsPayloadsAndPatternControlFlow) {
    emit(R"JOYEER(struct Box { var value: Int }
enum Choice { None, Some(Box), }
func read(choice: Choice): Int {
return match choice {
.None => 0,
.Some(box) => box.value,
}
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    EXPECT_NE(result.text.find("%joyeer.struct."), std::string::npos);
    EXPECT_NE(result.text.find("%joyeer.enum."), std::string::npos);
    EXPECT_NE(result.text.find("extractvalue %joyeer.enum."), std::string::npos);
    EXPECT_NE(result.text.find("getelementptr inbounds %joyeer.struct."), std::string::npos);
    EXPECT_NE(result.text.find("pattern."), std::string::npos);
}

TEST_F(LLVMBackendTest, DiagnosesCollectionsUntilRuntimeAbiLands) {
    emit(R"JOYEER(func first(): Int {
let values: [Int] = [1]
return values[0]
}
)JOYEER");

    ASSERT_FALSE(result.succeeded());
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(
            result.diagnostics[0].id,
            joyeer::llvmbackend::DiagnosticId::unsupportedInstruction);
}

} // namespace
