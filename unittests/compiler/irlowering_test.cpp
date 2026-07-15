#include "joyeer/compiler/irlowering.h"
#include "joyeer/compiler/lexparser.h"
#include "joyeer/compiler/nameresolution.h"
#include "joyeer/compiler/parser.h"
#include "joyeer/compiler/sourcefile.h"
#include "joyeer/compiler/symtable.h"
#include "joyeer/compiler/typechecking.h"
#include "joyeer/diagnostic/diagnostic.h"
#include "joyeer/ir/ir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

std::string readFixture(const std::string& relativePath) {
    const std::string path = std::string(JOYEER_TESTS_DIR) + "/" + relativePath;
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open IR lowering fixture: " + path);
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

class IRLoweringTest : public testing::Test {
protected:
    void lower(const std::string& text) {
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
        result = joyeer::lowering::Lowerer().lower(
            checking.model,
            "test.joyeer",
            joyeer::ir::SourceInfo {
                "test.joyeer",
                "C:/joyeer-tests",
                static_cast<uint64_t>(source->content.size()),
                source->lineStarts,
            });
    }

    const joyeer::ir::Function& function(const std::string& name) const {
        const auto found = std::find_if(
                result.module->functions.begin(),
                result.module->functions.end(),
                [&name](const auto& function) { return function.name == name; });
        EXPECT_NE(found, result.module->functions.end());
        return *found;
    }

    size_t opcodeCount(
            const joyeer::ir::Function& function,
            joyeer::ir::Opcode opcode) const {
        size_t count = 0;
        for (const auto& block : function.blocks) {
            count += static_cast<size_t>(std::count_if(
                    block.instructions.begin(),
                    block.instructions.end(),
                    [opcode](const auto& instruction) {
                        return instruction.opcode == opcode;
                    }));
        }
        return count;
    }

    Diagnostics lexerDiagnostics;
    SourceFile::Ptr source;
    joyeer::lowering::Result result;
};

TEST_F(IRLoweringTest, LowersParametersOperatorsAndReturns) {
    lower(R"JOYEER(func add(left: Int, right: Int): Int {
return left + right
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    ASSERT_NE(result.module, nullptr);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);

    const auto& add = function("add");
    ASSERT_EQ(add.blocks.size(), 1u);
    EXPECT_EQ(opcodeCount(add, joyeer::ir::Opcode::stackAllocate), 2u);
    EXPECT_EQ(opcodeCount(add, joyeer::ir::Opcode::store), 2u);
    EXPECT_EQ(opcodeCount(add, joyeer::ir::Opcode::load), 2u);
    EXPECT_EQ(opcodeCount(add, joyeer::ir::Opcode::add), 1u);
    EXPECT_EQ(opcodeCount(add, joyeer::ir::Opcode::returnValue), 1u);
}

TEST_F(IRLoweringTest, CarriesSourceLocationsAndMarksCleanupImplicit) {
    lower(R"JOYEER(func run(input: consuming String) {
let text = "value"
print(value: input)
print(value: text)
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    ASSERT_TRUE(result.module->sourceInfo.has_value());
    EXPECT_EQ(result.module->sourceInfo->fileName, "test.joyeer");
    EXPECT_EQ(result.module->sourceInfo->directory, "C:/joyeer-tests");
    EXPECT_EQ(result.module->sourceInfo->byteLength, source->content.size());
    EXPECT_EQ(result.module->sourceInfo->lineStarts, source->lineStarts);

    const auto& run = function("run");
    ASSERT_TRUE(run.debugLocation.has_value());
    EXPECT_FALSE(run.debugLocation->implicitCode);
    bool foundImplicitCleanup = false;
    bool foundImplicitParameterStorage = false;
    for (const auto& block : run.blocks) {
        for (const auto& instruction : block.instructions) {
            ASSERT_TRUE(instruction.debugLocation.has_value());
            if (instruction.opcode == joyeer::ir::Opcode::stackAllocate &&
                instruction.symbol == run.parameters[0].symbol) {
                foundImplicitParameterStorage = instruction.debugLocation->implicitCode;
            }
            if (instruction.opcode == joyeer::ir::Opcode::destroy) {
                foundImplicitCleanup |= instruction.debugLocation->implicitCode;
            }
        }
    }
    EXPECT_TRUE(foundImplicitParameterStorage);
    EXPECT_TRUE(foundImplicitCleanup);
}

TEST_F(IRLoweringTest, LowersBindingsAndMutableAssignmentsThroughStackSlots) {
    lower(R"JOYEER(func increment(value: Int): Int {
var current = value
current = current + 1
return current
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto& increment = function("increment");
    EXPECT_EQ(opcodeCount(increment, joyeer::ir::Opcode::stackAllocate), 2u);
    EXPECT_EQ(opcodeCount(increment, joyeer::ir::Opcode::store), 3u);
    EXPECT_EQ(opcodeCount(increment, joyeer::ir::Opcode::integerConstant), 1u);
    EXPECT_EQ(opcodeCount(increment, joyeer::ir::Opcode::add), 1u);
    EXPECT_EQ(opcodeCount(increment, joyeer::ir::Opcode::returnValue), 1u);
}

TEST_F(IRLoweringTest, LowersDeferredInitializationForTrivialAndOwnedStorage) {
    lower(R"JOYEER(func run(flag: Bool): String {
var number: Int
if flag { number = 1 } else { number = 2 }
var text: String
text = "ready"
print(value: number)
return text
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    const auto& run = function("run");
    EXPECT_EQ(opcodeCount(run, joyeer::ir::Opcode::stackAllocate), 3u);
    EXPECT_EQ(opcodeCount(run, joyeer::ir::Opcode::zeroInitialize), 1u);
    EXPECT_GE(opcodeCount(run, joyeer::ir::Opcode::destroy), 1u);
}

TEST_F(IRLoweringTest, LowersUserCallsAndTypeErasedPrintCalls) {
    lower(R"JOYEER(func identity(value: Int): Int { return value }
func run() {
let number = identity(value: 42)
print(value: number)
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);

    const auto& print = function("print");
    ASSERT_TRUE(print.isExternal);
    ASSERT_EQ(print.parameters.size(), 1u);
    EXPECT_TRUE(print.parameters[0].acceptsAnyType);

    const auto& run = function("run");
    EXPECT_EQ(opcodeCount(run, joyeer::ir::Opcode::call), 2u);
    EXPECT_EQ(opcodeCount(run, joyeer::ir::Opcode::returnVoid), 1u);
}

TEST_F(IRLoweringTest, LowersReadFileAsAnOwnedExternalResult) {
    lower(R"JOYEER(func load(path: String): Result<String, Int> {
return readFile(path: path)
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);

    const auto& readFile = function("readFile");
    EXPECT_TRUE(readFile.isExternal);
    EXPECT_TRUE(readFile.returnsValue);
    ASSERT_EQ(readFile.parameters.size(), 1u);
    EXPECT_EQ(
            result.module->types[readFile.parameters[0].value.type].kind,
            joyeer::typing::TypeKind::string);
    EXPECT_EQ(
            result.module->types[readFile.resultType].kind,
            joyeer::typing::TypeKind::result);
    EXPECT_EQ(opcodeCount(function("load"), joyeer::ir::Opcode::call), 1u);
}

TEST_F(IRLoweringTest, LowersExplicitByteConversionCalls) {
    lower(R"JOYEER(func convert(value: UInt8): String {
print(value: byteToInt(value: value))
return byteToString(value: value)
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    EXPECT_TRUE(function("byteToInt").isExternal);
    EXPECT_TRUE(function("byteToString").isExternal);
    EXPECT_EQ(opcodeCount(function("convert"), joyeer::ir::Opcode::call), 3u);
    EXPECT_EQ(opcodeCount(function("convert"), joyeer::ir::Opcode::returnValue), 1u);
}

TEST_F(IRLoweringTest, LowersIfExpressionValuesThroughMergeSlots) {
    lower(R"JOYEER(func choose(flag: Bool): Int {
return if flag { 1 } else { 2 }
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    const auto& choose = function("choose");
    EXPECT_EQ(choose.blocks.size(), 4u);
    EXPECT_EQ(opcodeCount(choose, joyeer::ir::Opcode::conditionalBranch), 1u);
    EXPECT_EQ(opcodeCount(choose, joyeer::ir::Opcode::branch), 2u);
    EXPECT_EQ(opcodeCount(choose, joyeer::ir::Opcode::stackAllocate), 2u);
    EXPECT_EQ(opcodeCount(choose, joyeer::ir::Opcode::returnValue), 1u);
}

TEST_F(IRLoweringTest, LowersIfBranchesThatReturnEarly) {
    lower(R"JOYEER(func choose(flag: Bool): Int {
if flag { return 1 }
return 2
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    const auto& choose = function("choose");
    EXPECT_EQ(choose.blocks.size(), 4u);
    EXPECT_EQ(opcodeCount(choose, joyeer::ir::Opcode::conditionalBranch), 1u);
    EXPECT_EQ(opcodeCount(choose, joyeer::ir::Opcode::returnValue), 2u);
}

TEST_F(IRLoweringTest, LowersWhileHeadersBodiesExitsAndBackEdges) {
    lower(R"JOYEER(func count(limit: Int): Int {
var value = 0
while value < limit {
value = value + 1
}
return value
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    const auto& count = function("count");
    EXPECT_EQ(count.blocks.size(), 4u);
    EXPECT_EQ(opcodeCount(count, joyeer::ir::Opcode::conditionalBranch), 1u);
    EXPECT_EQ(opcodeCount(count, joyeer::ir::Opcode::branch), 2u);
    EXPECT_EQ(opcodeCount(count, joyeer::ir::Opcode::less), 1u);
    EXPECT_EQ(opcodeCount(count, joyeer::ir::Opcode::returnValue), 1u);
}

TEST_F(IRLoweringTest, LowersInoutParametersAndArgumentsAsAddresses) {
    lower(R"JOYEER(func increment(value: inout Int) {
&value = value + 1
}
func run(): Int {
var number = 0
increment(value: &number)
return number
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);

    const auto& increment = function("increment");
    ASSERT_EQ(increment.parameters.size(), 1u);
    EXPECT_TRUE(increment.parameters[0].isMutable);
    EXPECT_EQ(
            increment.parameters[0].value.category,
            joyeer::ir::ValueCategory::address);
    EXPECT_EQ(opcodeCount(increment, joyeer::ir::Opcode::stackAllocate), 0u);
    EXPECT_EQ(opcodeCount(increment, joyeer::ir::Opcode::store), 1u);

    const auto& run = function("run");
    const auto call = std::find_if(
            run.blocks[0].instructions.begin(),
            run.blocks[0].instructions.end(),
            [](const auto& instruction) {
                return instruction.opcode == joyeer::ir::Opcode::call;
            });
    ASSERT_NE(call, run.blocks[0].instructions.end());
    ASSERT_EQ(call->operands.size(), 1u);
    const auto slot = std::find_if(
            run.blocks[0].instructions.begin(),
            run.blocks[0].instructions.end(),
            [&call](const auto& instruction) {
                return instruction.result.has_value() &&
                        instruction.result->id == call->operands[0];
            });
    ASSERT_NE(slot, run.blocks[0].instructions.end());
    EXPECT_EQ(slot->result->category, joyeer::ir::ValueCategory::address);
}

TEST_F(IRLoweringTest, LowersStructConstructionFieldReadsAndWrites) {
    lower(R"JOYEER(struct Box {
var value: Int
let fixed: Int = 7
}
func make(): Int {
var box = Box(value: 1)
box.value = box.value + box.fixed
return box.value
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    ASSERT_EQ(result.module->structures.size(), 1u);
    EXPECT_EQ(result.module->structures[0].fields.size(), 2u);

    const auto& make = function("make");
    EXPECT_EQ(opcodeCount(make, joyeer::ir::Opcode::constructStruct), 1u);
    EXPECT_GE(opcodeCount(make, joyeer::ir::Opcode::fieldAddress), 3u);
    EXPECT_EQ(opcodeCount(make, joyeer::ir::Opcode::add), 1u);
    EXPECT_EQ(opcodeCount(make, joyeer::ir::Opcode::returnValue), 1u);
}

TEST_F(IRLoweringTest, LowersUserOptionalAndNilEnumConstruction) {
    lower(R"JOYEER(enum Value { Empty, Number(Int), }
func qualified(): Value { return Value.Number(1) }
func contextual(): Value { return .Empty }
func some(value: Int): Int? { return .Some(value) }
func none(): Int? { return nil }
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    EXPECT_GE(result.module->enumerations.size(), 2u);
    EXPECT_EQ(opcodeCount(function("qualified"), joyeer::ir::Opcode::constructEnum), 1u);
    EXPECT_EQ(opcodeCount(function("contextual"), joyeer::ir::Opcode::constructEnum), 1u);
    EXPECT_EQ(opcodeCount(function("some"), joyeer::ir::Opcode::constructEnum), 1u);
    EXPECT_EQ(opcodeCount(function("none"), joyeer::ir::Opcode::constructEnum), 1u);
}

TEST_F(IRLoweringTest, LowersStringCountAndSubscript) {
    lower(R"JOYEER(func first(text: String): UInt8 {
let length = text.count
print(value: length)
return text[0]
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    const auto& first = function("first");
    EXPECT_EQ(opcodeCount(first, joyeer::ir::Opcode::count), 1u);
    EXPECT_EQ(opcodeCount(first, joyeer::ir::Opcode::subscript), 1u);
}

TEST_F(IRLoweringTest, LowersArrayAndDictionaryLiteralsWithStructuredTypes) {
    lower(R"JOYEER(func build(): Int {
let values: [Int] = [1, 2, 3]
let lookup: [String: Int] = ["answer": 42]
print(value: values.count)
return values[0] + lookup["answer"]
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    const auto& build = function("build");
    EXPECT_EQ(opcodeCount(build, joyeer::ir::Opcode::constructArray), 1u);
    EXPECT_EQ(opcodeCount(build, joyeer::ir::Opcode::constructDictionary), 1u);
    EXPECT_EQ(opcodeCount(build, joyeer::ir::Opcode::subscript), 2u);

    const auto arrayType = std::find_if(
            result.module->types.begin(),
            result.module->types.end(),
            [](const auto& type) { return type.name == "[Int]"; });
    ASSERT_NE(arrayType, result.module->types.end());
    EXPECT_EQ(arrayType->kind, joyeer::typing::TypeKind::array);
    ASSERT_EQ(arrayType->arguments.size(), 1u);
    const auto elementType = std::find_if(
            result.module->types.begin(),
            result.module->types.end(),
            [&arrayType](const auto& type) { return type.id == arrayType->arguments[0]; });
    ASSERT_NE(elementType, result.module->types.end());
    EXPECT_EQ(elementType->kind, joyeer::typing::TypeKind::integer);
}

TEST_F(IRLoweringTest, LowersMutatingArrayAppendWithOwnedElements) {
    lower(R"JOYEER(func build(): [String] {
var values: [String] = []
&values.append(element: "first" + "!")
let second = "second"
&values.append(element: second)
return values
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    const auto& build = function("build");
    EXPECT_EQ(opcodeCount(build, joyeer::ir::Opcode::arrayAppend), 2u);
    EXPECT_GE(opcodeCount(build, joyeer::ir::Opcode::copyValue), 1u);
    EXPECT_EQ(opcodeCount(build, joyeer::ir::Opcode::returnValue), 1u);
}

TEST_F(IRLoweringTest, LowersDictionaryInsertionAndUpdateWithOwnedEntries) {
    lower(R"JOYEER(func build(): [String: String] {
var lookup: [String: String] = [:]
&lookup["key"] = "first" + "!"
let key = "key"
let value = "second"
&lookup[key] = value
print(value: lookup.count)
return lookup
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    const auto& build = function("build");
    EXPECT_EQ(opcodeCount(build, joyeer::ir::Opcode::dictionarySet), 2u);
    EXPECT_GE(opcodeCount(build, joyeer::ir::Opcode::copyValue), 2u);
    EXPECT_EQ(opcodeCount(build, joyeer::ir::Opcode::count), 1u);
    EXPECT_EQ(opcodeCount(build, joyeer::ir::Opcode::returnValue), 1u);
}

TEST_F(IRLoweringTest, LowersMutableArrayElementsAsAddressProjections) {
    lower(R"JOYEER(func incrementFirst(values: inout [Int]): Int {
&values[0] = values[0] + 1
return values[0]
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    const auto& increment = function("incrementFirst");
    EXPECT_EQ(opcodeCount(increment, joyeer::ir::Opcode::subscriptAddress), 1u);
    EXPECT_EQ(opcodeCount(increment, joyeer::ir::Opcode::subscript), 2u);
}

TEST_F(IRLoweringTest, LowersEnumMatchesAndPayloadBindings) {
    lower(R"JOYEER(enum Value { Empty, Number(Int), }
func read(value: Value): Int {
return match value {
.Empty => 0,
.Number(number) => number,
}
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    const auto& read = function("read");
    EXPECT_EQ(read.blocks.size(), 4u);
    EXPECT_EQ(opcodeCount(read, joyeer::ir::Opcode::switchPattern), 1u);
    EXPECT_EQ(opcodeCount(read, joyeer::ir::Opcode::extractPayload), 1u);
    EXPECT_EQ(opcodeCount(read, joyeer::ir::Opcode::returnValue), 1u);
}

TEST_F(IRLoweringTest, LowersOptionalMatchesToResultConstruction) {
    lower(R"JOYEER(enum Failure { Missing, }
func unwrap(value: Int?): Result<Int, Failure> {
return match value {
.Some(number) => .Ok(number),
.None => .Err(.Missing),
}
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    const auto& unwrap = function("unwrap");
    EXPECT_EQ(opcodeCount(unwrap, joyeer::ir::Opcode::switchPattern), 1u);
    EXPECT_EQ(opcodeCount(unwrap, joyeer::ir::Opcode::extractPayload), 1u);
    EXPECT_EQ(opcodeCount(unwrap, joyeer::ir::Opcode::constructEnum), 3u);
}

TEST_F(IRLoweringTest, LowersLiteralWildcardAndNestedPayloadPatterns) {
    lower(R"JOYEER(enum Flag { Bool(Bool), }
func classify(value: UInt8): Int {
return match value { b'a' => 1, _ => 0, }
}
func fromFlag(flag: Flag): Int {
return match flag { .Bool(true) => 1, .Bool(false) => 0, }
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    EXPECT_EQ(opcodeCount(function("classify"), joyeer::ir::Opcode::switchPattern), 1u);
    EXPECT_EQ(opcodeCount(function("fromFlag"), joyeer::ir::Opcode::switchPattern), 1u);

    const auto text = joyeer::ir::dump(*result.module);
    EXPECT_NE(text.find("byte(97):UInt8"), std::string::npos);
    EXPECT_NE(text.find("case#"), std::string::npos);
    EXPECT_NE(text.find("true:Bool"), std::string::npos);
}

TEST_F(IRLoweringTest, LowersTheCompleteJsonParserMvpFixture) {
    lower(readFixture("parser/ok/json_mvp.joyeer"));

    ASSERT_TRUE(result.succeeded())
            << joyeer::lowering::dump(result.diagnostics)
            << (result.module == nullptr ? std::string() : joyeer::ir::dump(*result.module));
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    EXPECT_EQ(opcodeCount(function("peek"), joyeer::ir::Opcode::subscript), 1u);
    EXPECT_EQ(opcodeCount(function("parseValue"), joyeer::ir::Opcode::switchPattern), 2u);
    EXPECT_GE(opcodeCount(function("parseValue"), joyeer::ir::Opcode::constructEnum), 7u);
}

TEST_F(IRLoweringTest, ClonesBorrowedValuesAndDestroysOwningLocals) {
    lower(R"JOYEER(func main() {
let first = "static"
let second = first
let values: [String] = [second]
print(value: values.count)
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto& main = function("main");
    EXPECT_EQ(opcodeCount(main, joyeer::ir::Opcode::copyValue), 3u);
    EXPECT_EQ(opcodeCount(main, joyeer::ir::Opcode::destroy), 3u);

    const auto& instructions = main.blocks[0].instructions;
    const auto returnInstruction = std::find_if(
            instructions.begin(),
            instructions.end(),
            [](const auto& instruction) {
                return instruction.opcode == joyeer::ir::Opcode::returnVoid;
            });
    ASSERT_NE(returnInstruction, instructions.end());
    ASSERT_NE(returnInstruction, instructions.begin());
    EXPECT_EQ((returnInstruction - 1)->opcode, joyeer::ir::Opcode::destroy);
}

TEST_F(IRLoweringTest, CleansAllActiveScopesBeforeEarlyReturn) {
    lower(R"JOYEER(func run(flag: Bool) {
let text = "owned"
if flag { return }
let values: [String] = [text]
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto& run = function("run");
    EXPECT_EQ(opcodeCount(run, joyeer::ir::Opcode::destroy), 3u);
    EXPECT_EQ(opcodeCount(run, joyeer::ir::Opcode::returnVoid), 2u);
    for (const auto& block : run.blocks) {
        for (size_t index = 0; index < block.instructions.size(); ++index) {
            if (block.instructions[index].opcode != joyeer::ir::Opcode::returnVoid) continue;
            ASSERT_GT(index, 0u);
            EXPECT_EQ(block.instructions[index - 1].opcode, joyeer::ir::Opcode::destroy);
        }
    }
}

TEST_F(IRLoweringTest, TransfersOwnedReturnValuesToTheCaller) {
    lower(R"JOYEER(func build(): String {
let text = "left" + "right"
return text
}
func main() {
let value = build()
print(value: value)
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto& build = function("build");
    const auto& main = function("main");
    EXPECT_EQ(opcodeCount(build, joyeer::ir::Opcode::copyValue), 1u);
    EXPECT_EQ(opcodeCount(build, joyeer::ir::Opcode::destroy), 1u);
    EXPECT_EQ(opcodeCount(main, joyeer::ir::Opcode::destroy), 1u);
}

TEST_F(IRLoweringTest, TransfersConsumingArgumentsAndCleansCalleeOwnership) {
    lower(R"JOYEER(func take(value: consuming String): String {
&value = value + "!"
return value
}
func run(): String {
var text = "first"
let moved = take(value: consume text)
text = "second"
print(value: text)
return moved
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    const auto& take = function("take");
    ASSERT_EQ(take.parameters.size(), 1u);
    EXPECT_TRUE(take.parameters[0].isConsuming);
    EXPECT_FALSE(take.parameters[0].isMutable);
    EXPECT_GE(opcodeCount(take, joyeer::ir::Opcode::destroy), 1u);

    const auto& run = function("run");
    EXPECT_EQ(opcodeCount(run, joyeer::ir::Opcode::take), 1u);
    EXPECT_EQ(opcodeCount(run, joyeer::ir::Opcode::call), 2u);
    EXPECT_EQ(opcodeCount(run, joyeer::ir::Opcode::returnValue), 1u);
}

TEST_F(IRLoweringTest, KeepsExplicitBorrowingParametersAsNonOwningValues) {
    lower(R"JOYEER(func inspect(value: borrowing String) {
print(value: value)
}
func run() {
let text = "retained"
inspect(value: text)
print(value: text)
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    const auto& inspect = function("inspect");
    ASSERT_EQ(inspect.parameters.size(), 1u);
    EXPECT_FALSE(inspect.parameters[0].isMutable);
    EXPECT_FALSE(inspect.parameters[0].isConsuming);
    EXPECT_EQ(opcodeCount(inspect, joyeer::ir::Opcode::destroy), 0u);
}

TEST_F(IRLoweringTest, LowersInitializingParametersAsCallerOwnedAddresses) {
    lower(R"JOYEER(func initialize(out: initializing String) {
&out = "value"
}
func run(): String {
var text: String
initialize(out: &text)
return text
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    const auto& initialize = function("initialize");
    ASSERT_EQ(initialize.parameters.size(), 1u);
    EXPECT_TRUE(initialize.parameters[0].isMutable);
    EXPECT_TRUE(initialize.parameters[0].isInitializing);
    EXPECT_FALSE(initialize.parameters[0].isConsuming);
    EXPECT_EQ(
            initialize.parameters[0].value.category,
            joyeer::ir::ValueCategory::address);
    EXPECT_EQ(opcodeCount(initialize, joyeer::ir::Opcode::stackAllocate), 0u);
    EXPECT_EQ(opcodeCount(initialize, joyeer::ir::Opcode::store), 1u);
    EXPECT_EQ(opcodeCount(function("run"), joyeer::ir::Opcode::call), 1u);
}

TEST_F(IRLoweringTest, TakesAndReinitializesOwnedProjections) {
    lower(R"JOYEER(struct Pair { var left: String
var right: String
}
func take(value: consuming String) { print(value: value) }
func run(): Pair {
var pair = Pair(left: "left", right: "right")
take(value: consume pair.left)
print(value: pair.right)
pair.left = "new"
return pair
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    const auto& run = function("run");
    EXPECT_EQ(opcodeCount(run, joyeer::ir::Opcode::take), 1u);
    EXPECT_GE(opcodeCount(run, joyeer::ir::Opcode::fieldAddress), 2u);
    EXPECT_EQ(opcodeCount(run, joyeer::ir::Opcode::returnValue), 1u);
}

TEST_F(IRLoweringTest, LowersTrailingExpressionsAsImplicitReturns) {
    lower(R"JOYEER(func square(value: Int): Int { value * value }
func text(): String { "left" + "right" }
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    EXPECT_EQ(opcodeCount(function("square"), joyeer::ir::Opcode::returnValue), 1u);
    EXPECT_EQ(opcodeCount(function("text"), joyeer::ir::Opcode::returnValue), 1u);
    EXPECT_EQ(opcodeCount(function("text"), joyeer::ir::Opcode::destroy), 0u);
}

TEST_F(IRLoweringTest, AnchorsImplicitReturnAtTheTrailingExpression) {
    lower(R"JOYEER(func answer(): Int {
42
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto& answer = function("answer");
    const auto& instructions = answer.blocks[0].instructions;
    const auto returned = std::find_if(
            instructions.begin(),
            instructions.end(),
            [](const auto& instruction) {
                return instruction.opcode == joyeer::ir::Opcode::returnValue;
            });
    ASSERT_NE(returned, instructions.end());
    ASSERT_TRUE(returned->debugLocation.has_value());
    EXPECT_EQ(
            source->content.substr(
                    returned->debugLocation->span.offset,
                    returned->debugLocation->span.length),
            "42");
}

TEST_F(IRLoweringTest, AnchorsTrailingReturnPreparationAtTheExpression) {
    lower(R"JOYEER(func maybe(): String? {
"value"
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto& maybe = function("maybe");
    const auto expectedOffset = source->content.find("\"value\"");
    ASSERT_NE(expectedOffset, std::string::npos);
    bool foundCopy = false;
    bool foundConstruction = false;
    bool foundReturn = false;
    for (const auto& block : maybe.blocks) {
        for (const auto& instruction : block.instructions) {
            if (instruction.opcode != joyeer::ir::Opcode::copyValue &&
                instruction.opcode != joyeer::ir::Opcode::constructEnum &&
                instruction.opcode != joyeer::ir::Opcode::returnValue) {
                continue;
            }
            ASSERT_TRUE(instruction.debugLocation.has_value());
            EXPECT_EQ(instruction.debugLocation->span.offset, expectedOffset);
            foundCopy |= instruction.opcode == joyeer::ir::Opcode::copyValue;
            foundConstruction |= instruction.opcode == joyeer::ir::Opcode::constructEnum;
            foundReturn |= instruction.opcode == joyeer::ir::Opcode::returnValue;
        }
    }
    EXPECT_TRUE(foundCopy);
    EXPECT_TRUE(foundConstruction);
    EXPECT_TRUE(foundReturn);
}

TEST_F(IRLoweringTest, ReportsStraightLineFunctionsThatFallThrough) {
    lower(R"JOYEER(func incomplete(value: Int): Int {
let copy = value
}
)JOYEER");

    ASSERT_FALSE(result.succeeded());
    ASSERT_EQ(result.diagnostics.size(), 1u)
            << joyeer::lowering::dump(result.diagnostics);
    EXPECT_EQ(
            result.diagnostics[0].id,
            joyeer::lowering::DiagnosticId::missingReturn);
}

} // namespace
