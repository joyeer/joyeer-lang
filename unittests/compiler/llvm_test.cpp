#include "joyeer/backend/llvm.h"
#include "joyeer/compiler/irlowering.h"
#include "joyeer/compiler/lexparser.h"
#include "joyeer/compiler/nameresolution.h"
#include "joyeer/compiler/parser.h"
#include "joyeer/compiler/sourcefile.h"
#include "joyeer/compiler/typechecking.h"
#include "joyeer/diagnostic/diagnostic.h"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <sstream>
#include <string>

namespace {

class LLVMBackendTest : public testing::Test {
protected:
    void emit(
            const std::string& text,
            bool carrySourceInfo = false,
            const joyeer::llvmbackend::EmitOptions& options = {}) {
        lexerDiagnostics.errors.clear();
        source = std::make_shared<SourceFile>(text);
        LexParser lexer(&lexerDiagnostics);
        lexer.parse(source);
        ASSERT_TRUE(lexerDiagnostics.errors.empty());

        joyeer::parser::Parser parser(source->tokens);
        const auto parsing = parser.parse();
        ASSERT_TRUE(parsing.succeeded()) << joyeer::parser::dump(parsing.diagnostics);

        const auto resolution = joyeer::semantic::NameResolver().resolve(parsing.root);
        ASSERT_TRUE(resolution.succeeded()) << joyeer::semantic::dump(resolution.diagnostics);

        const auto checking = joyeer::typing::TypeChecker().check(resolution.model);
        ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);

        const auto sourceInfo = carrySourceInfo
            ? std::optional<joyeer::ir::SourceInfo>(joyeer::ir::SourceInfo {
                "test.joyeer",
                "C:/joyeer-tests",
                static_cast<uint64_t>(source->content.size()),
                source->lineStarts,
            })
            : std::nullopt;
        const auto lowering = joyeer::lowering::Lowerer().lower(
            checking.model,
            "test.joyeer",
            sourceInfo);
        ASSERT_TRUE(lowering.succeeded()) << joyeer::lowering::dump(lowering.diagnostics);
        result = joyeer::llvmbackend::Emitter().emit(*lowering.module, options);
    }

    Diagnostics lexerDiagnostics;
    SourceFile::Ptr source;
    joyeer::llvmbackend::Result result;
};

TEST_F(LLVMBackendTest, SourceLocationTransportDoesNotChangeLLVMWithoutDebugEmission) {
    const std::string text = R"JOYEER(func run() {
print(value: 42)
}
)JOYEER";
    emit(text);
    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    const auto withoutLocations = result.text;

    emit(text, true);
    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    EXPECT_EQ(result.text, withoutLocations);
}

TEST_F(LLVMBackendTest, EmitsDwarfLineTablesForSourceFunctionsAndInstructions) {
    emit(
            R"JOYEER(func run(input: consuming String) {
print(value: input)
}
)JOYEER",
            true,
            joyeer::llvmbackend::EmitOptions {
                true,
                joyeer::DebugInfoFormat::dwarf,
                joyeer::OptimizationLevel::O0,
            });

    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    EXPECT_NE(result.text.find("define void @joyeer_fn_"), std::string::npos);
    EXPECT_NE(result.text.find(") !dbg !"), std::string::npos);
    EXPECT_NE(result.text.find(", !dbg !"), std::string::npos);
    EXPECT_NE(result.text.find("!llvm.dbg.cu = !{"), std::string::npos);
    EXPECT_NE(result.text.find("emissionKind: LineTablesOnly"), std::string::npos);
    EXPECT_NE(result.text.find("!\"Dwarf Version\", i32 4"), std::string::npos);
    EXPECT_EQ(result.text.find("!\"CodeView\""), std::string::npos);
    EXPECT_NE(
            result.text.find(
                    "!DIFile(filename: \"test.joyeer\", directory: \"C:/joyeer-tests\")"),
            std::string::npos);
    EXPECT_NE(result.text.find("isOptimized: false"), std::string::npos);
    EXPECT_EQ(result.text.find("DISPFlagOptimized"), std::string::npos);
    EXPECT_NE(result.text.find("!DILocation(line: 2, column: 1"), std::string::npos);
    EXPECT_EQ(result.text.find("isImplicitCode: true"), std::string::npos);
}

TEST_F(LLVMBackendTest, EmitsOptimizedCodeViewLineTables) {
    emit(
            "func run() { print(value: 42) }\n",
            true,
            joyeer::llvmbackend::EmitOptions {
                true,
                joyeer::DebugInfoFormat::codeView,
                joyeer::OptimizationLevel::O2,
            });

    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    EXPECT_NE(result.text.find("!\"CodeView\", i32 1"), std::string::npos);
    EXPECT_EQ(result.text.find("!\"Dwarf Version\""), std::string::npos);
    EXPECT_NE(result.text.find("isOptimized: true"), std::string::npos);
    EXPECT_NE(result.text.find("DISPFlagOptimized"), std::string::npos);
}

TEST_F(LLVMBackendTest, RejectsLineTablesWithoutSourceInfo) {
    emit(
            "func run() { print(value: 42) }\n",
            false,
            joyeer::llvmbackend::EmitOptions { true });

    ASSERT_FALSE(result.succeeded());
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(
            result.diagnostics[0].id,
            joyeer::llvmbackend::DiagnosticId::invalidModule);
}

TEST_F(LLVMBackendTest, OmitsDebugScopesFromHelpersAndEntryTrampoline) {
    emit(
            R"JOYEER(func main() {
let text = "value"
print(value: text)
}
)JOYEER",
            true,
            joyeer::llvmbackend::EmitOptions {
                true,
                joyeer::DebugInfoFormat::dwarf,
                joyeer::OptimizationLevel::O0,
            });

    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    EXPECT_NE(result.text.find("define void @joyeer_destroy_type_"), std::string::npos);
    EXPECT_NE(result.text.find("define void @joyeer_main() {"), std::string::npos);
    EXPECT_EQ(result.text.find("define void @joyeer_main() !dbg"), std::string::npos);
    const auto helper = result.text.find("define void @joyeer_destroy_type_");
    const auto helperHeaderEnd = result.text.find('\n', helper);
    ASSERT_NE(helperHeaderEnd, std::string::npos);
    EXPECT_EQ(
            result.text.substr(helper, helperHeaderEnd - helper).find("!dbg"),
            std::string::npos);
    const auto cleanup = result.text.find("  call void @joyeer_destroy_type_");
    const auto cleanupEnd = result.text.find('\n', cleanup);
    ASSERT_NE(cleanup, std::string::npos);
    ASSERT_NE(cleanupEnd, std::string::npos);
    EXPECT_EQ(
            result.text.substr(cleanup, cleanupEnd - cleanup).find("!dbg"),
            std::string::npos);
        const auto sourceFunction = result.text.find("define void @joyeer_fn_");
        const auto sourceFunctionEnd = result.text.find("\n}\n", sourceFunction);
        ASSERT_NE(sourceFunction, std::string::npos);
        ASSERT_NE(sourceFunctionEnd, std::string::npos);
        const auto functionText = result.text.substr(
            sourceFunction,
            sourceFunctionEnd - sourceFunction);
        EXPECT_NE(functionText.find("  ret void"), std::string::npos);
        EXPECT_EQ(functionText.find("  ret void, !dbg"), std::string::npos);
}

TEST_F(LLVMBackendTest, FallsBackToUnknownForOversizedColumns) {
    const std::string indent(65536, ' ');
    emit(
            "func run() {\n" + indent + "print(value: 42)\n}\n",
            true,
            joyeer::llvmbackend::EmitOptions {
                true,
                joyeer::DebugInfoFormat::dwarf,
                joyeer::OptimizationLevel::O0,
            });

    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    EXPECT_NE(result.text.find("!DILocation(line: 2, column: 0"), std::string::npos);
}

TEST_F(LLVMBackendTest, AnnotatesEveryInstructionExpandedFromAHighLevelOperation) {
    emit(
            R"JOYEER(func make(): [Int] {
return [1, 2]
}
)JOYEER",
            true,
            joyeer::llvmbackend::EmitOptions {
                true,
                joyeer::DebugInfoFormat::dwarf,
                joyeer::OptimizationLevel::O0,
            });

    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    const auto functionStart = result.text.find("define %joyeer.array @joyeer_fn_");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = result.text.find("\n}\n", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    std::istringstream functionText(
            result.text.substr(functionStart, functionEnd - functionStart));
    std::string line;
    size_t instructionLines = 0;
    while (std::getline(functionText, line)) {
        if (!line.starts_with("  ")) continue;
        ++instructionLines;
        EXPECT_NE(line.find(", !dbg !"), std::string::npos) << line;
    }
    EXPECT_GT(instructionLines, 5u);
}

TEST_F(LLVMBackendTest, EmitsFullDebugScopesVariablesAndPhysicalTypes) {
    emit(
            R"JOYEER(func run(input: Int, target: inout Int) {
let value = input
if true {
let value = target
print(value: value)
}
&target = value
}
)JOYEER",
            true,
            joyeer::llvmbackend::EmitOptions {
                true,
                joyeer::DebugInfoFormat::dwarf,
                joyeer::OptimizationLevel::O0,
                true,
            });

    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    EXPECT_NE(result.text.find("emissionKind: FullDebug"), std::string::npos);
    EXPECT_NE(result.text.find("declare void @llvm.dbg.declare(metadata, metadata, metadata)"),
              std::string::npos);
    EXPECT_NE(result.text.find("call void @llvm.dbg.declare(metadata ptr %v"),
              std::string::npos);
    EXPECT_NE(result.text.find("!DILocalVariable(name: \"input\", arg: 1"),
              std::string::npos);
    EXPECT_NE(result.text.find("!DILocalVariable(name: \"target\", arg: 2"),
              std::string::npos);
    EXPECT_GE(std::count(
                      result.text.begin(),
                      result.text.end(),
                      '\n'),
              10);
    EXPECT_NE(result.text.find("!DILocalVariable(name: \"value\""),
              std::string::npos);
    EXPECT_NE(result.text.find("!DILexicalBlock("), std::string::npos);
    EXPECT_NE(result.text.find("!DIBasicType(name: \"Int\", size: 64"),
              std::string::npos);
    EXPECT_EQ(result.text.find("#dbg_declare"), std::string::npos);
}

TEST_F(LLVMBackendTest, KeepsLineTablesFreeOfVariableMetadata) {
    emit(
            "func run(value: Int) { print(value: value) }\n",
            true,
            joyeer::llvmbackend::EmitOptions {
                true,
                joyeer::DebugInfoFormat::dwarf,
                joyeer::OptimizationLevel::O0,
                false,
            });

    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    EXPECT_NE(result.text.find("emissionKind: LineTablesOnly"), std::string::npos);
    EXPECT_EQ(result.text.find("llvm.dbg.declare"), std::string::npos);
    EXPECT_EQ(result.text.find("DILocalVariable"), std::string::npos);
    EXPECT_EQ(result.text.find("DILexicalBlock"), std::string::npos);
}

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

TEST_F(LLVMBackendTest, EmitsStringUtf8AsOwnedByteArray) {
    emit(R"JOYEER(func bytes(text: String): [UInt8] {
return text.utf8()
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    EXPECT_NE(
            result.text.find(
                    "declare void @joyeer_array_create_owned_abi(ptr, ptr, i64, i64, ptr, ptr)"),
            std::string::npos);
    EXPECT_NE(result.text.find("extractvalue %joyeer.string"), std::string::npos);
    EXPECT_NE(
            result.text.find("i64 1, ptr null, ptr null)"),
            std::string::npos);
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

TEST_F(LLVMBackendTest, EmitsDictionaryInsertAndUpdateRuntimeAbi) {
    emit(R"JOYEER(func build(): [String: String] {
var lookup: [String: String] = [:]
&lookup["key"] = "first"
let key = "key"
let value = "second"
&lookup[key] = value
return lookup
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    EXPECT_NE(
            result.text.find(
                    "declare void @joyeer_dictionary_set_owned_abi(ptr, ptr, ptr)"),
            std::string::npos);
    EXPECT_NE(
            result.text.find("call void @joyeer_dictionary_set_owned_abi(ptr"),
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

        TEST_F(LLVMBackendTest, EmitsTypedReadFileResultFromLayoutIndependentABI) {
            emit(R"JOYEER(func load(path: String): Result<String, IOError> {
        return readFile(path: path)
        }
        )JOYEER");

            ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
            EXPECT_NE(
                result.text.find(
                    "declare i32 @joyeer_read_file_abi(ptr, ptr, ptr, i64)"),
                std::string::npos);
            EXPECT_NE(
                result.text.find("call i32 @joyeer_read_file_abi(ptr"),
                std::string::npos);
            EXPECT_NE(result.text.find("readfile.ok."), std::string::npos);
            EXPECT_NE(result.text.find("readfile.error."), std::string::npos);
            EXPECT_NE(result.text.find("select i1"), std::string::npos);
            EXPECT_NE(result.text.find("getelementptr inbounds %joyeer.enum."),
                  std::string::npos);
            EXPECT_NE(result.text.find("store %joyeer.enum."), std::string::npos);
        }

TEST_F(LLVMBackendTest, EmitsExplicitByteConversions) {
    emit(R"JOYEER(func convert(value: UInt8): String {
print(value: byteToInt(value: value))
return byteToString(value: value)
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    EXPECT_NE(result.text.find("zext i8"), std::string::npos);
    EXPECT_NE(
            result.text.find("declare void @joyeer_byte_to_string_abi(ptr, i8)"),
            std::string::npos);
    EXPECT_NE(
            result.text.find("call void @joyeer_byte_to_string_abi(ptr"),
            std::string::npos);
}

TEST_F(LLVMBackendTest, EmitsSourceLevelConsumingTransfers) {
    emit(R"JOYEER(func take(value: consuming String): String {
&value = value + "!"
return value
}
func run(): String {
var text = "first"
let moved = take(value: consume text)
text = "second"
return moved
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    EXPECT_NE(
            result.text.find("store %joyeer.string zeroinitializer, ptr"),
            std::string::npos);
    EXPECT_NE(
            result.text.find("call void @joyeer_destroy_type_"),
            std::string::npos);
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
