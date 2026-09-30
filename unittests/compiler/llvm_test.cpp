#include "joyeer/backend/llvm.h"
#include "joyeer/compiler/irlowering.h"
#include "joyeer/compiler/lexparser.h"
#include "joyeer/compiler/nameresolution.h"
#include "joyeer/compiler/parser.h"
#include "joyeer/compiler/sourcefile.h"
#include "joyeer/compiler/typechecking.h"
#include "joyeer/diagnostic/diagnostic.h"

#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>

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

TEST_F(LLVMBackendTest, UnitConstantsDoNotAllocateOrStorePayloads) {
    emit("func run(): Void { return () }\n"
         "func complete(): Result<Void, String> { return .Ok(()) }\n");
    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    EXPECT_NE(result.text.find("define void @joyeer_fn_"), std::string::npos);
    EXPECT_EQ(result.text.find("load void"), std::string::npos);
    EXPECT_EQ(result.text.find("store void"), std::string::npos);
    EXPECT_EQ(result.text.find("store {}"), std::string::npos);
    EXPECT_EQ(result.text.find("load {}"), std::string::npos);
    const auto sourceStart = result.text.find("define void @joyeer_fn_");
    ASSERT_NE(sourceStart, std::string::npos);
    const auto sourceBodies = result.text.substr(sourceStart);
    EXPECT_EQ(sourceBodies.find("call void @joyeer_string_clone"), std::string::npos);
    const auto unitBody = sourceBodies.substr(0, sourceBodies.find("\n}"));
    EXPECT_EQ(unitBody.find("alloca"), std::string::npos);
    EXPECT_EQ(unitBody.find("call "), std::string::npos);
}

TEST_F(LLVMBackendTest, PreservesParameterlessEntryWithZeroStatus) {
    emit("func main(): Void { return () }\n");
    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    EXPECT_TRUE(result.hasEntryPoint);
    EXPECT_NE(result.text.find("define i32 @joyeer_main_uses_arguments()"), std::string::npos);
    EXPECT_NE(result.text.find("  ret i32 0\n"), std::string::npos);
    EXPECT_NE(result.text.find("define i64 @joyeer_main(ptr %args)"), std::string::npos);
    EXPECT_NE(result.text.find("  ret i64 0\n"), std::string::npos);
}

TEST_F(LLVMBackendTest, PassesBorrowedStringArgumentsAndReturnsFullStatus) {
    emit("func main(args values: [String]): Int { return values.count }\n");
    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    EXPECT_TRUE(result.hasEntryPoint);
    EXPECT_NE(result.text.find("  ret i32 1\n"), std::string::npos);
    EXPECT_NE(result.text.find("  %arguments = load %joyeer.array, ptr %args\n"), std::string::npos);
    EXPECT_NE(result.text.find("  %status = call i64 @joyeer_fn_"), std::string::npos);
    EXPECT_NE(result.text.find("(%joyeer.array %arguments)\n"), std::string::npos);
    EXPECT_NE(result.text.find("  ret i64 %status\n"), std::string::npos);

    emit("func main(args: borrowing [String]): Int { return args.count }\n");
    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    EXPECT_TRUE(result.hasEntryPoint);
}

TEST_F(LLVMBackendTest, RejectsOtherEntrySignatures) {
    for (const auto* source : {
                 "func main(): Int { return 0 }\n",
                 "func main(args: [String]) {}\n",
                 "func main(values: [String]): Int { return 0 }\n",
                 "func main(args: [Int]): Int { return 0 }\n",
                 "func main(args: consuming [String]): Int { return 0 }\n",
                 "func main(args: inout [String]): Int { return 0 }\n",
             }) {
        ASSERT_NO_FATAL_FAILURE(emit(source));
        ASSERT_EQ(result.diagnostics.size(), 1u) << source;
        EXPECT_EQ(
                result.diagnostics.front().id,
                joyeer::llvmbackend::DiagnosticId::invalidEntryPoint) << source;
    }
}

TEST_F(LLVMBackendTest, EmitsUnitStorageAndDebugTypesWithoutChangingVoidReturns) {
    emit("struct Completed { var value: Void }\n"
         "func identity(value: Void): Void { return value }\n"
         "func main() {\nlet value = identity(value: ())\n"
         "let completion = Completed(value: value)\n"
         "match completion.value { () => print(value: 42) }\n}\n",
         true,
         joyeer::llvmbackend::EmitOptions {
             true, joyeer::DebugInfoFormat::dwarf, joyeer::OptimizationLevel::O0, true,
         });
    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    EXPECT_NE(result.text.find("name: \"Void\""), std::string::npos);
    EXPECT_NE(result.text.find("define i64 @joyeer_main(ptr %args)"), std::string::npos);
    EXPECT_EQ(result.text.find("load void"), std::string::npos);
    EXPECT_EQ(result.text.find("store void"), std::string::npos);
}

TEST_F(LLVMBackendTest, UnitContainersKeepChecksWithoutUnitPayloadLoadsOrStores) {
    emit("func run(): Void {\n"
         "var values: [Void] = [()]\n&values.append(element: ())\n"
         "match values[0] { () => () }\n"
         "var entries: [String: Void] = [\"key\": ()]\n"
         "&entries[\"key\"] = ()\nmatch entries[\"key\"] { () => () }\n}\n");
    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    EXPECT_EQ(result.text.find("load {}"), std::string::npos);
    EXPECT_EQ(result.text.find("store {}"), std::string::npos);
    EXPECT_NE(result.text.find("array index out of bounds"), std::string::npos);
    EXPECT_NE(result.text.find("call ptr @joyeer_dictionary_at_abi"), std::string::npos);
    EXPECT_NE(result.text.find("call void @joyeer_array_append_owned_abi"), std::string::npos);
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
    EXPECT_NE(result.text.find("define i64 @joyeer_main(ptr %args) {"), std::string::npos);
    EXPECT_EQ(result.text.find("define i64 @joyeer_main(ptr %args) !dbg"), std::string::npos);
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
    EXPECT_NE(result.text.find("call { i64, i1 } @llvm.sadd.with.overflow.i64"),
              std::string::npos);
    EXPECT_NE(result.text.find("call { i64, i1 } @llvm.smul.with.overflow.i64"),
              std::string::npos);
    EXPECT_NE(result.text.find("br i1 %"), std::string::npos);
    EXPECT_NE(result.text.find("ret i64"), std::string::npos);
}

TEST_F(LLVMBackendTest, EmitsCheckedArithmeticIntrinsicsAtEveryOptimizationLevel) {
    for (const auto level : { joyeer::OptimizationLevel::O0, joyeer::OptimizationLevel::O1,
                             joyeer::OptimizationLevel::O2, joyeer::OptimizationLevel::O3 }) {
        SCOPED_TRACE(static_cast<int>(level));
        joyeer::llvmbackend::EmitOptions options;
        options.optimizationLevel = level;
        ASSERT_NO_FATAL_FAILURE(emit(R"JOYEER(
func calculate(left: Int, right: Int): Int {
let sum = left + right
let difference = left - right
return sum * difference + left
}
)JOYEER", false, options));
        ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
        for (const auto* name : { "sadd", "ssub", "smul" }) {
            EXPECT_NE(result.text.find(
                    "call { i64, i1 } @llvm." + std::string(name) + ".with.overflow.i64"),
                      std::string::npos);
        }
        EXPECT_EQ(result.text.find("@joyeer_checked_"), std::string::npos);
        EXPECT_EQ(result.text.find("add nsw"), std::string::npos);
        EXPECT_NE(result.text.find("declare void @joyeer_panic(ptr) cold noreturn"),
                  std::string::npos);
        for (const auto* message : { "integer addition overflow\\00",
                                     "integer subtraction overflow\\00",
                                     "integer multiplication overflow\\00" }) {
            const auto first = result.text.find(message);
            ASSERT_NE(first, std::string::npos);
            EXPECT_EQ(result.text.find(message, first + 1), std::string::npos);
        }
        std::unordered_set<std::string> labels;
        std::istringstream lines(result.text);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.starts_with("trap.") || line.starts_with("checked.")) {
                EXPECT_TRUE(labels.insert(line).second) << line;
            }
        }
        EXPECT_EQ(labels.size(), 8u);
    }
}

TEST(LLVMArithmeticBackendTest, GuardsDivisionAndRemainderAtEveryOptimizationLevel) {
    struct Operands {
        bool constants;
        int64_t left;
        int64_t right;
    };
    const auto minimum = std::numeric_limits<int64_t>::min();
    const auto maximum = std::numeric_limits<int64_t>::max();
    const Operands cases[] = {
        { false, 0, 0 },
        { true, 7, 3 },
        { true, -7, 3 },
        { true, 7, -3 },
        { true, -7, -3 },
        { true, 0, -1 },
        { true, minimum, 1 },
        { true, minimum, 3 },
        { true, minimum, minimum },
        { true, maximum, -1 },
        { true, maximum, minimum },
        { true, 0, 0 },
        { true, minimum, 0 },
        { true, maximum, 0 },
        { true, minimum, -1 },
    };
    for (const auto opcode : { joyeer::ir::Opcode::divide, joyeer::ir::Opcode::remainder }) {
        const std::string operation = opcode == joyeer::ir::Opcode::divide
            ? "division" : "remainder";
        const std::string llvmOpcode = opcode == joyeer::ir::Opcode::divide
            ? "sdiv" : "srem";
        SCOPED_TRACE(operation);
        for (const auto& values : cases) {
            SCOPED_TRACE(testing::Message() << values.constants << ": "
                                           << values.left << ", " << values.right);
            joyeer::ir::Module module;
            module.sourceName = "checked-arithmetic.joyeer";
            module.types = { { 0, "Int", joyeer::typing::TypeKind::integer } };
            joyeer::ir::Function function;
            function.id = 0;
            function.name = "calculate";
            function.resultType = 0;
            function.returnsValue = true;
            function.entry = 0;
            joyeer::ir::BasicBlock block { 0, "entry" };
            if (values.constants) {
                block.instructions = {
                    {
                        joyeer::ir::Opcode::integerConstant,
                        joyeer::ir::Value { 0, 0, joyeer::ir::ValueCategory::value },
                        {}, {}, std::nullopt, std::nullopt, values.left,
                    },
                    {
                        joyeer::ir::Opcode::integerConstant,
                        joyeer::ir::Value { 1, 0, joyeer::ir::ValueCategory::value },
                        {}, {}, std::nullopt, std::nullopt, values.right,
                    },
                };
            } else {
                function.parameters = {
                    { { 0, 0, joyeer::ir::ValueCategory::value }, std::nullopt, "left" },
                    { { 1, 0, joyeer::ir::ValueCategory::value }, std::nullopt, "right" },
                };
            }
            block.instructions.push_back({
                opcode,
                joyeer::ir::Value { 2, 0, joyeer::ir::ValueCategory::value },
                { 0, 1 },
            });
            block.instructions.push_back({
                joyeer::ir::Opcode::returnValue, std::nullopt, { 2 },
            });
            function.blocks.push_back(std::move(block));
            module.functions.push_back(std::move(function));
            const auto left = values.constants ? std::to_string(values.left) : "%v0";
            const auto right = values.constants ? std::to_string(values.right) : "%v1";
            for (const auto level : { joyeer::OptimizationLevel::O0,
                                     joyeer::OptimizationLevel::O1,
                                     joyeer::OptimizationLevel::O2,
                                     joyeer::OptimizationLevel::O3 }) {
                SCOPED_TRACE(static_cast<int>(level));
                joyeer::llvmbackend::EmitOptions options;
                options.optimizationLevel = level;
                const auto result = joyeer::llvmbackend::Emitter().emit(module, options);
                ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
                EXPECT_EQ(result.text.find("@joyeer_checked_"), std::string::npos);
                EXPECT_EQ(result.text.find("sdiv exact"), std::string::npos);
                EXPECT_NE(result.text.find("declare void @joyeer_panic(ptr) cold noreturn"),
                          std::string::npos);
                for (const auto* failure : { " by zero", " overflow" }) {
                    EXPECT_NE(result.text.find("integer " + operation + failure + "\\00"),
                              std::string::npos);
                }

                const auto zero = result.text.find("icmp eq i64 " + right + ", 0\n");
                const auto minimumCheck = result.text.find(
                        "icmp eq i64 " + left + ", -9223372036854775808\n");
                const auto negativeOne = result.text.find("icmp eq i64 " + right + ", -1\n");
                const auto overflow = result.text.find(" = and i1 ");
                const auto arithmetic = result.text.find(
                        " = " + llvmOpcode + " i64 " + left + ", " + right + "\n");
                const auto firstContinuation = result.text.find("\nchecked.");
                ASSERT_NE(firstContinuation, std::string::npos);
                const auto secondContinuation = result.text.find(
                        "\nchecked.", firstContinuation + 1);
                ASSERT_NE(secondContinuation, std::string::npos);
                ASSERT_NE(zero, std::string::npos);
                ASSERT_NE(minimumCheck, std::string::npos);
                ASSERT_NE(negativeOne, std::string::npos);
                ASSERT_NE(overflow, std::string::npos);
                ASSERT_NE(arithmetic, std::string::npos);
                EXPECT_LT(zero, firstContinuation);
                EXPECT_LT(firstContinuation, minimumCheck);
                EXPECT_LT(minimumCheck, negativeOne);
                EXPECT_LT(negativeOne, overflow);
                EXPECT_LT(overflow, secondContinuation);
                EXPECT_LT(secondContinuation, arithmetic);

                std::istringstream lines(result.text.substr(0, arithmetic));
                std::string line;
                size_t branches = 0;
                size_t panicCalls = 0;
                size_t unreachable = 0;
                std::unordered_set<std::string> labels;
                while (std::getline(lines, line)) {
                    if (line.starts_with("  br i1 ")) ++branches;
                    if (line.starts_with("  call void @joyeer_panic(")) ++panicCalls;
                    if (line == "  unreachable") ++unreachable;
                    if (line.starts_with("trap.") || line.starts_with("checked.")) {
                        EXPECT_TRUE(labels.insert(line).second) << line;
                    }
                }
                EXPECT_EQ(branches, 2u);
                EXPECT_EQ(panicCalls, 2u);
                EXPECT_EQ(unreachable, 2u);
                EXPECT_EQ(labels.size(), 4u);
            }
        }
    }
}

TEST_F(LLVMBackendTest, PreservesDebugLocationsOnDivisionAndRemainderChecks) {
    for (const auto level : { joyeer::OptimizationLevel::O0, joyeer::OptimizationLevel::O3 }) {
        ASSERT_NO_FATAL_FAILURE(emit(
                R"JOYEER(func calculate(left: Int, right: Int): Int {
return left / right % right
}
)JOYEER",
                true,
                joyeer::llvmbackend::EmitOptions {
                    true, joyeer::DebugInfoFormat::dwarf, level, true,
                }));
        ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
        std::istringstream lines(result.text);
        std::string line;
        size_t checkedInstructions = 0;
        size_t labels = 0;
        while (std::getline(lines, line)) {
            if (line.starts_with("trap.") || line.starts_with("checked.")) {
                ++labels;
                EXPECT_EQ(line.find("!dbg"), std::string::npos);
            }
            if (line.starts_with("  ") &&
                (line.find(" = icmp eq i64 ") != std::string::npos ||
                 line.find(" = and i1 ") != std::string::npos ||
                 line.find(" = sdiv i64 ") != std::string::npos ||
                 line.find(" = srem i64 ") != std::string::npos ||
                 line.find("@joyeer_panic") != std::string::npos ||
                 line.starts_with("  br i1 ") ||
                 line.starts_with("  unreachable"))) {
                ++checkedInstructions;
                EXPECT_NE(line.find(", !dbg !"), std::string::npos) << line;
            }
        }
        EXPECT_EQ(labels, 8u);
        EXPECT_EQ(checkedInstructions, 22u);
    }
}

TEST_F(LLVMBackendTest, PreservesDebugLocationsOnExpandedSafetyChecks) {
    for (const auto level : { joyeer::OptimizationLevel::O0, joyeer::OptimizationLevel::O3 }) {
        ASSERT_NO_FATAL_FAILURE(emit(
                R"JOYEER(func read(values: [Int], index: Int): Int {
return values[index] + 1
}
)JOYEER",
                true,
                joyeer::llvmbackend::EmitOptions {
                    true, joyeer::DebugInfoFormat::dwarf, level, true,
                }));
        ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
        std::istringstream lines(result.text);
        std::string line;
        size_t checkedInstructions = 0;
        size_t labels = 0;
        while (std::getline(lines, line)) {
            if (line.starts_with("trap.") || line.starts_with("checked.")) {
                ++labels;
                EXPECT_EQ(line.find("!dbg"), std::string::npos);
            }
            if (line.starts_with("  ") &&
                (line.find("@llvm.sadd.with.overflow") != std::string::npos ||
                 line.find("@joyeer_panic") != std::string::npos ||
                 line.find("getelementptr i64") != std::string::npos ||
                 line.starts_with("  br i1 ") ||
                 line.starts_with("  unreachable"))) {
                ++checkedInstructions;
                EXPECT_NE(line.find(", !dbg !"), std::string::npos) << line;
            }
        }
        EXPECT_EQ(labels, 4u);
        EXPECT_EQ(checkedInstructions, 8u);
    }
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

TEST_F(LLVMBackendTest, EmitsTypedArrayAccessAndDictionaryRuntimeAbi) {
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
    EXPECT_EQ(result.text.find("@joyeer_array_at"), std::string::npos);
    EXPECT_NE(result.text.find("getelementptr i64, ptr"), std::string::npos);
    EXPECT_NE(result.text.find("array index out of bounds\\00"), std::string::npos);
    EXPECT_NE(result.text.find("@joyeer_dictionary_at"), std::string::npos);
    EXPECT_NE(result.text.find("extractvalue %joyeer.array"), std::string::npos);
}

TEST_F(LLVMBackendTest, EmitsTypedArrayAddressesForPrimitiveAndAggregateElements) {
    ASSERT_NO_FATAL_FAILURE(emit(R"JOYEER(
struct Item { var number: Int }
struct Empty {}
enum Choice { None, Some(Int, String) }
func access(ints: inout [Int], bytes: inout [UInt8], flags: inout [Bool],
            items: inout [Item], empty: inout [Empty], choices: inout [Choice],
            strings: inout [String], nested: inout [[Int]], index: Int) {
&ints[index] = ints[index] + 1
&bytes[index] = b'x'
&flags[index] = true
&items[index] = Item(number: 42)
&empty[index] = Empty()
&choices[index] = .Some(7, "value")
&strings[index] = "replacement"
&nested[index] = [1, 2]
}
)JOYEER"));
    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    EXPECT_EQ(result.text.find("@joyeer_array_at"), std::string::npos);
    for (const auto* elementType : { "i64", "i8", "i1", "%joyeer.string", "%joyeer.array" }) {
        EXPECT_NE(result.text.find("getelementptr " + std::string(elementType) + ", ptr"),
                  std::string::npos);
    }
    EXPECT_NE(result.text.find("getelementptr %joyeer.struct."), std::string::npos);
    EXPECT_NE(result.text.find("getelementptr %joyeer.enum."), std::string::npos);
    EXPECT_NE(result.text.find("icmp eq ptr"), std::string::npos);
    EXPECT_NE(result.text.find("icmp slt i64"), std::string::npos);
    EXPECT_NE(result.text.find("icmp sge i64"), std::string::npos);
    EXPECT_NE(result.text.find("call void @joyeer_destroy_type_"), std::string::npos);
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

TEST_F(LLVMBackendTest, EmitsNontrappingDictionaryGetForEveryKeyKind) {
    ASSERT_NO_FATAL_FAILURE(emit(R"JOYEER(
func integers(values: borrowing [Int: Int], key: Int): Int? {
return values.get(key: key)
}
func booleans(values: borrowing [Bool: Int], key: Bool): Int? {
return values.get(key: key)
}
func bytes(values: borrowing [UInt8: Int], key: UInt8): Int? {
return values.get(key: key)
}
func strings(values: borrowing [String: Int], key: borrowing String): Int? {
return values.get(key: key)
}
)JOYEER"));
    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    EXPECT_NE(result.text.find(
            "declare ptr @joyeer_dictionary_find_abi(ptr, i64, ptr, i64, i32)"),
            std::string::npos);
    EXPECT_EQ(result.text.find("@joyeer_dictionary_at_abi"), std::string::npos);
    const auto sourceStart = result.text.find("define %joyeer.enum.");
    ASSERT_NE(sourceStart, std::string::npos);
    const auto bodies = result.text.substr(sourceStart);
    EXPECT_EQ(bodies.find("call void @joyeer_clone_type_"), std::string::npos);
    EXPECT_EQ(bodies.find("call void @joyeer_destroy_type_"), std::string::npos);
    for (const auto* keyAbi : {
                 ", i64 8, i32 1)", ", i64 1, i32 2)",
                 ", i64 16, i32 3)", ", i64 1, i32 4)",
             }) {
        EXPECT_NE(bodies.find(keyAbi), std::string::npos);
    }
    std::istringstream lines(bodies);
    std::string line;
    size_t lookups = 0;
    size_t presenceChecks = 0;
    while (std::getline(lines, line)) {
        if (line.find("call ptr @joyeer_dictionary_find_abi") != std::string::npos) ++lookups;
        if (line.find("icmp ne ptr") != std::string::npos) ++presenceChecks;
    }
    EXPECT_EQ(lookups, 4u);
    EXPECT_EQ(presenceChecks, 4u);
}

TEST_F(LLVMBackendTest, DictionaryGetClonesOnlyTheFoundOwnedPayloadExactlyOnce) {
    struct Case {
        const char* declarations;
        const char* valueType;
        const char* cloneOperation;
    };
    for (const auto& testCase : {
                 Case { "", "String", "call void @joyeer_string_clone_abi" },
                 Case { "", "[String]", "call void @joyeer_array_clone_abi" },
                 Case { "", "[String: String]", "call void @joyeer_dictionary_clone_abi" },
                 Case { "struct Box { var text: String }\n", "Box",
                        "getelementptr inbounds %joyeer.struct." },
                 Case { "enum Choice { None, Text(String) }\n", "Choice", "switch i32" },
                 Case { "", "String?", "switch i32" },
             }) {
        SCOPED_TRACE(testCase.valueType);
        ASSERT_NO_FATAL_FAILURE(emit(
                std::string(testCase.declarations) +
                "func read(values: borrowing [Int: " + testCase.valueType +
                "], key: Int): Optional<" + testCase.valueType + "> {\n"
                "return values.get(key: key)\n}\n"));
        ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
        const auto sourceStart = result.text.find("define %joyeer.enum.");
        ASSERT_NE(sourceStart, std::string::npos);
        const auto body = result.text.substr(
                sourceStart, result.text.find("\n}", sourceStart) - sourceStart);
        EXPECT_EQ(body.find("@joyeer_dictionary_clone_abi"), std::string::npos);
        EXPECT_EQ(body.find("call void @joyeer_destroy_type_"), std::string::npos);
        EXPECT_EQ(body.find("@joyeer_dictionary_at_abi"), std::string::npos);
        std::istringstream lines(body);
        std::string line;
        std::string label;
        std::string storedValueAddress;
        size_t clones = 0;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == ':') label = line;
            if (line.find("call ptr @joyeer_dictionary_find_abi") != std::string::npos) {
                const auto addressStart = line.find('%');
                storedValueAddress = line.substr(
                        addressStart, line.find(" = ", addressStart) - addressStart);
            }
            const auto call = line.find("call void @joyeer_clone_type_");
            if (call == std::string::npos) continue;
            ++clones;
            EXPECT_TRUE(label.starts_with("dictionary.get.some.")) << line;
            ASSERT_FALSE(storedValueAddress.empty());
            EXPECT_TRUE(line.ends_with(", ptr " + storedValueAddress + ")")) << line;
            const auto helperStart = line.find('@', call);
            const auto helper = line.substr(helperStart, line.find('(', helperStart) - helperStart);
            const auto definitionStart = result.text.find("define void " + helper + "(");
            ASSERT_NE(definitionStart, std::string::npos);
            const auto definition = result.text.substr(
                    definitionStart, result.text.find("\n}", definitionStart) - definitionStart);
            EXPECT_NE(definition.find(testCase.cloneOperation), std::string::npos);
        }
        EXPECT_EQ(clones, 1u);
        EXPECT_NE(body.find("store %joyeer.enum."), std::string::npos);
        EXPECT_NE(body.find(" zeroinitializer, ptr "), std::string::npos);
        EXPECT_NE(body.find("label %dictionary.get.done."), std::string::npos);
    }
}

TEST_F(LLVMBackendTest, DictionaryGetCopiesTrivialAndNestedOptionalPayloadsWithoutCloning) {
    ASSERT_NO_FATAL_FAILURE(emit(R"JOYEER(
struct Empty {}
struct Point { var x: Int }
enum Choice { None, Value(Int) }
func empty(values: borrowing [Int: Empty]): Empty? {
return values.get(key: 1)
}
func point(values: borrowing [Int: Point]): Point? {
return values.get(key: 1)
}
func choice(values: borrowing [Int: Choice]): Choice? {
return values.get(key: 1)
}
func nested(values: borrowing [Int: Int?]): Optional<Int?> {
return values.get(key: 1)
}
func boolean(values: borrowing [Int: Bool]): Bool? {
return values.get(key: 1)
}
func byte(values: borrowing [Int: UInt8]): UInt8? {
return values.get(key: 1)
}
)JOYEER"));
    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    const auto sourceStart = result.text.find("define %joyeer.enum.");
    ASSERT_NE(sourceStart, std::string::npos);
    const auto bodies = result.text.substr(sourceStart);
    EXPECT_EQ(bodies.find("call void @joyeer_clone_type_"), std::string::npos);
    std::istringstream lines(bodies);
    std::string line;
    std::string label;
    size_t enumPayloadCopies = 0;
    size_t structurePayloadCopies = 0;
    size_t scalarPayloadCopies = 0;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == ':') label = line;
        if (!label.starts_with("dictionary.get.some.")) continue;
        if (line.find(" = load %joyeer.enum.") != std::string::npos) ++enumPayloadCopies;
        if (line.find(" = load %joyeer.struct.") != std::string::npos) ++structurePayloadCopies;
        if (line.find(" = load i1,") != std::string::npos ||
            line.find(" = load i8,") != std::string::npos) ++scalarPayloadCopies;
    }
    EXPECT_EQ(enumPayloadCopies, 2u);
    EXPECT_EQ(structurePayloadCopies, 2u);
    EXPECT_EQ(scalarPayloadCopies, 2u);
}

TEST_F(LLVMBackendTest, DictionaryGetHandlesEmptyAndUnitDictionariesWithoutPayloadAccess) {
    ASSERT_NO_FATAL_FAILURE(emit(R"JOYEER(
func empty(): Void? {
let values: [Int: Void] = [:]
return values.get(key: 1)
}
func present(): Void? {
let values: [Int: Void] = [1: ()]
return values.get(key: 1)
}
)JOYEER"));
    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    EXPECT_NE(result.text.find("call ptr @joyeer_dictionary_find_abi"), std::string::npos);
    EXPECT_NE(result.text.find("icmp ne ptr"), std::string::npos);
    EXPECT_EQ(result.text.find("load void"), std::string::npos);
    EXPECT_EQ(result.text.find("store void"), std::string::npos);
    EXPECT_EQ(result.text.find("load {}"), std::string::npos);
    EXPECT_EQ(result.text.find("store {}"), std::string::npos);
    const auto sourceStart = result.text.find("define %joyeer.enum.");
    ASSERT_NE(sourceStart, std::string::npos);
    const auto bodies = result.text.substr(sourceStart);
    EXPECT_EQ(bodies.find(", i32 0, i32 1, i32 0"), std::string::npos);
    EXPECT_EQ(bodies.find("call void @joyeer_clone_type_"), std::string::npos);
}

TEST_F(LLVMBackendTest, DictionaryGetCarriesDebugLocationsThroughLookupAndCloneBranches) {
    ASSERT_NO_FATAL_FAILURE(emit(
            "func read(values: borrowing [Int: String], key: Int): String? {\n"
            "return values.get(key: key)\n}\n",
            true,
            joyeer::llvmbackend::EmitOptions {
                true, joyeer::DebugInfoFormat::dwarf, joyeer::OptimizationLevel::O0, true,
            }));
    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    const auto sourceStart = result.text.find("define %joyeer.enum.");
    ASSERT_NE(sourceStart, std::string::npos);
    const auto body = result.text.substr(
            sourceStart, result.text.find("\n}", sourceStart) - sourceStart);
    std::istringstream lines(body);
    std::string line;
    std::string lookupLocation;
    size_t locatedOperations = 0;
    while (std::getline(lines, line)) {
        if (line.find("call ptr @joyeer_dictionary_find_abi") == std::string::npos &&
            line.find("call void @joyeer_clone_type_") == std::string::npos &&
            line.find("label %dictionary.get.") == std::string::npos) {
            continue;
        }
        const auto location = line.find(", !dbg !");
        ASSERT_NE(location, std::string::npos) << line;
        if (lookupLocation.empty()) lookupLocation = line.substr(location);
        EXPECT_EQ(line.substr(location), lookupLocation);
        ++locatedOperations;
    }
    EXPECT_EQ(locatedOperations, 4u);
}

TEST(LLVMOwnershipBackendTest, DictionaryGetUsesConcreteOptionalCaseTags) {
    for (const bool someFirst : { false, true }) {
        SCOPED_TRACE(someFirst);
        joyeer::ir::Module module;
        module.sourceName = "dictionary-get.joyeer";
        module.types = {
            { 0, "Int", joyeer::typing::TypeKind::integer },
            { 1, "[Int: Int]", joyeer::typing::TypeKind::dictionary,
              joyeer::semantic::invalidSymbolId, { 0, 0 } },
            { 2, "Int?", joyeer::typing::TypeKind::optional, 10, { 0 } },
        };
        const joyeer::ir::EnumCaseDefinition some { 11, "Some", { 0 } };
        const joyeer::ir::EnumCaseDefinition none { 12, "None", {} };
        module.enumerations = {
            { 10, 2, "Int?", { someFirst ? some : none, someFirst ? none : some } },
        };
        joyeer::ir::Function function;
        function.id = 0;
        function.name = "lookup";
        function.resultType = 2;
        function.returnsValue = true;
        function.entry = 0;
        function.parameters = {
            { { 0, 1, joyeer::ir::ValueCategory::value }, std::nullopt, "dictionary" },
            { { 1, 0, joyeer::ir::ValueCategory::value }, std::nullopt, "key" },
        };
        function.blocks = {
            {
                0, "entry",
                {
                    {
                        joyeer::ir::Opcode::dictionaryGet,
                        joyeer::ir::Value { 2, 2, joyeer::ir::ValueCategory::value },
                        { 0, 1 },
                    },
                    { joyeer::ir::Opcode::returnValue, std::nullopt, { 2 } },
                },
            },
        };
        module.functions.push_back(std::move(function));
        const auto result = joyeer::llvmbackend::Emitter().emit(module);
        ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
        const auto sourceStart = result.text.find("define %joyeer.enum.2");
        ASSERT_NE(sourceStart, std::string::npos);
        const auto body = result.text.substr(sourceStart);
        const auto someBranch = body.find("\ndictionary.get.some.");
        const auto doneBranch = body.find("\ndictionary.get.done.");
        ASSERT_NE(someBranch, std::string::npos);
        ASSERT_NE(doneBranch, std::string::npos);
        EXPECT_NE(body.substr(0, someBranch).find(
                "store i32 " + std::to_string(someFirst ? 1 : 0) + ", ptr "),
                std::string::npos);
        EXPECT_NE(body.substr(someBranch, doneBranch - someBranch).find(
                "store i32 " + std::to_string(someFirst ? 0 : 1) + ", ptr "),
                std::string::npos);
        EXPECT_EQ(body.find("call void @joyeer_clone_type_"), std::string::npos);
    }
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

TEST_F(LLVMBackendTest, GuardsInactivePayloadTestsWithTagBranches) {
    ASSERT_NO_FATAL_FAILURE(emit(R"JOYEER(
enum Value { Number(Int, Int), Text(String) }
enum Nested { Wrap(Value), Empty }
func classify(value: Nested): Int {
return match value {
.Wrap(.Text("x")) => 1,
.Wrap(.Text(_)) => 2,
.Wrap(.Number(_, _)) => 3,
.Empty => 4,
}
}
)JOYEER"));
    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    EXPECT_EQ(result.text.find(" = and i1 "), std::string::npos);
    size_t payloadBranches = 0;
    std::istringstream lines(result.text);
    std::string line;
    std::string label;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == ':') label = line;
        if (line.find("br i1 ") != std::string::npos &&
            line.find("label %pattern.payload.") != std::string::npos) {
            ++payloadBranches;
        }
        if (line.find("call i1 @joyeer_string_equal_abi") != std::string::npos) {
            EXPECT_TRUE(label.starts_with("pattern.payload.")) << line;
        }
    }
    EXPECT_GE(payloadBranches, 2u);
}

TEST_F(LLVMBackendTest, HoistsLocalAndScratchAllocationsButNotDebugDeclarations) {
    ASSERT_NO_FATAL_FAILURE(emit(
            R"JOYEER(func main() {
var index = 0
while index < 2 {
let text = "a" + "b"
let values = [text]
let lookup = ["key": values[0]]
let optional: String? = .Some(lookup["key"])
print(value: match optional { .Some(value) => value, .None => "" })
index = index + values.count
}
}
)JOYEER",
            true,
            joyeer::llvmbackend::EmitOptions {
                true, joyeer::DebugInfoFormat::dwarf, joyeer::OptimizationLevel::O0, true,
            }));
    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    std::istringstream lines(result.text);
    std::string line;
    std::string label;
    size_t allocations = 0;
    size_t debugDeclarations = 0;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == ':') label = line;
        if (line.find(" = alloca ") != std::string::npos) {
            ++allocations;
            EXPECT_EQ(label, "entry:") << line;
        }
        if (line.find("call void @llvm.dbg.declare") != std::string::npos) {
            ++debugDeclarations;
            EXPECT_NE(label, "entry:") << line;
        }
    }
    EXPECT_GT(allocations, 6u);
    EXPECT_GT(debugDeclarations, 0u);
}

TEST_F(LLVMBackendTest, SeparatesConcreteBuiltinEnumCaseIdentities) {
    ASSERT_NO_FATAL_FAILURE(emit(R"JOYEER(func main() {
let integer: Int? = .Some(42)
let text: String? = .Some("hello")
let first: Result<Int, String> = .Ok(7)
let second: Result<String, Int> = .Err(9)
print(value: match integer { .Some(value) => value, .None => 0 })
print(value: match text { .Some(value) => value, .None => "" })
print(value: match first { .Ok(value) => value, .Err(_) => 0 })
print(value: match second { .Ok(_) => 0, .Err(code) => code })
}
)JOYEER"));
    ASSERT_TRUE(result.succeeded()) << joyeer::llvmbackend::dump(result.diagnostics);
    EXPECT_TRUE(result.hasEntryPoint);
    EXPECT_NE(result.text.find("define void @joyeer_fn_"), std::string::npos);
    EXPECT_NE(result.text.find("define i64 @joyeer_main(ptr %args)"), std::string::npos);
    EXPECT_NE(result.text.find("call void @joyeer_print_int"), std::string::npos);
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
