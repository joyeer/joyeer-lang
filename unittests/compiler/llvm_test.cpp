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
            result.text.find("declare void @joyeer_print_string_abi(ptr, i64)"),
            std::string::npos);
    EXPECT_NE(
            result.text.find("declare void @joyeer_string_concat_abi(ptr, ptr, i64, ptr, i64)"),
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

TEST_F(LLVMBackendTest, EmitsArrayDictionaryCountAndSubscriptRuntimeAbi) {
    emit(R"JOYEER(func read(): Int {
let values: [Int] = [1, 2]
let lookup: [String: Int] = ["answer": 42]
print(value: values.count)
return values[0] + lookup["answer"]
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    EXPECT_NE(result.text.find("%joyeer.array = type { ptr, i64, i64 }"),
              std::string::npos);
    EXPECT_NE(result.text.find("%joyeer.dictionary = type { ptr, i64, i64 }"),
              std::string::npos);
    EXPECT_NE(result.text.find("@joyeer_array_create"), std::string::npos);
    EXPECT_NE(result.text.find("@joyeer_dictionary_create"), std::string::npos);
    EXPECT_NE(result.text.find("@joyeer_array_at"), std::string::npos);
    EXPECT_NE(result.text.find("@joyeer_dictionary_at"), std::string::npos);
    EXPECT_NE(result.text.find("extractvalue %joyeer.array"), std::string::npos);
}

TEST_F(LLVMBackendTest, EmitsMutatingArrayAppendRuntimeAbi) {
    emit(R"JOYEER(func build(): [String] {
var values: [String] = []
&values.append(element: "first")
let second = "second"
&values.append(element: second)
return values
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    EXPECT_NE(
            result.text.find("declare void @joyeer_array_append_owned_abi(ptr, ptr)"),
            std::string::npos);
    EXPECT_NE(
            result.text.find("call void @joyeer_array_append_owned_abi(ptr"),
            std::string::npos);
    EXPECT_NE(result.text.find("call void @joyeer_clone_type_"), std::string::npos);
}

TEST_F(LLVMBackendTest, EmitsZeroInitializationForDeferredOwnedStorage) {
    emit(R"JOYEER(func value(): String {
var text: String
text = "ready"
return text
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    EXPECT_NE(
            result.text.find("store %joyeer.string zeroinitializer, ptr"),
            std::string::npos);
}

        TEST_F(LLVMBackendTest, EmitsReadFileThroughTagAndPayloadOutPointers) {
            emit(R"JOYEER(func load(path: String): Result<String, Int> {
        return readFile(path: path)
        }
        )JOYEER");

            ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
            EXPECT_NE(
                result.text.find(
                    "declare void @joyeer_read_file_abi(ptr, ptr, i32, i32, ptr, i64)"),
                std::string::npos);
            EXPECT_NE(
                result.text.find("call void @joyeer_read_file_abi(ptr"),
                std::string::npos);
            EXPECT_NE(result.text.find("getelementptr inbounds %joyeer.enum."),
                  std::string::npos);
            EXPECT_NE(result.text.find("store %joyeer.enum."), std::string::npos);
        }

TEST(LLVMOwnershipBackendTest, EmitsRecursiveOwnershipHelpersAndOperations) {
    joyeer::ir::Module module;
    module.sourceName = "ownership.joyeer";
    module.types = {
        joyeer::ir::TypeName {
            0,
            "Void",
            joyeer::typing::TypeKind::voidType,
        },
        joyeer::ir::TypeName {
            1,
            "String",
            joyeer::typing::TypeKind::string,
        },
    };
    joyeer::ir::Function function;
    function.id = 0;
    function.name = "ownership";
    function.resultType = 0;
    function.entry = 0;
    function.blocks = {
        joyeer::ir::BasicBlock {
            0,
            "entry",
            {
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::stringConstant,
                    joyeer::ir::Value { 0, 1, joyeer::ir::ValueCategory::value },
                    {}, {}, std::nullopt, std::nullopt, 0, "text",
                },
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::copyValue,
                    joyeer::ir::Value { 1, 1, joyeer::ir::ValueCategory::value },
                    { 0 },
                },
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::stackAllocate,
                    joyeer::ir::Value { 2, 1, joyeer::ir::ValueCategory::address },
                },
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::store,
                    std::nullopt,
                    { 1, 2 },
                },
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::take,
                    joyeer::ir::Value { 3, 1, joyeer::ir::ValueCategory::value },
                    { 2 },
                },
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::stackAllocate,
                    joyeer::ir::Value { 4, 1, joyeer::ir::ValueCategory::address },
                },
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::store,
                    std::nullopt,
                    { 3, 4 },
                },
                joyeer::ir::Instruction {
                    joyeer::ir::Opcode::destroy,
                    std::nullopt,
                    { 4 },
                },
                joyeer::ir::Instruction { joyeer::ir::Opcode::returnVoid },
            },
        },
    };
    module.functions.push_back(std::move(function));

    const auto result = joyeer::llvmbackend::Emitter().emit(module);
    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    EXPECT_NE(result.text.find("define void @joyeer_clone_type_1"), std::string::npos);
    EXPECT_NE(result.text.find("define void @joyeer_destroy_type_1"), std::string::npos);
    EXPECT_NE(result.text.find("call void @joyeer_clone_type_1"), std::string::npos);
    EXPECT_NE(result.text.find("call void @joyeer_destroy_type_1"), std::string::npos);
    EXPECT_NE(result.text.find("store %joyeer.string zeroinitializer"), std::string::npos);
}

} // namespace
