#include "compiler/ir/irlowering.h"
#include "compiler/frontend/lexparser.h"
#include "compiler/frontend/nameresolution.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/sourcefile.h"
#include "compiler/frontend/typechecking.h"
#include "compiler/diagnostics/diagnostic.h"
#include "compiler/ir/ir.h"
#include "module_lowering_fixture.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

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

TEST_F(IRLoweringTest, LowersMethodsAsFunctionsWithALeadingReceiverParameter) {
    lower("struct Counter {\n"
          "    var count: Int\n"
          "    func read(): Int { return self.count }\n"
          "    mutating func bump(by amount: Int) { &self.count += amount }\n"
          "    consuming func finish(): Int { return self.count }\n"
          "}\n"
          "extension Counter {\n"
          "    func main(): Int { return self.count }\n"
          "}\n"
          "func run(): Int {\n"
          "    var c = Counter(count: 1)\n"
          "    &c.bump(by: 2)\n"
          "    let a = c.read()\n"
          "    let b = c.main()\n"
          "    return a + b + consume c.finish()\n"
          "}\n");
    ASSERT_NE(result.module, nullptr);
    ASSERT_TRUE(result.succeeded());

    const auto& read = function("Counter.read");
    ASSERT_EQ(read.parameters.size(), 1u);
    EXPECT_EQ(read.parameters[0].name, "self");
    EXPECT_EQ(read.parameters[0].value.category, joyeer::ir::ValueCategory::value);
    EXPECT_FALSE(read.parameters[0].isMutable);

    // A mutating receiver is an address projection, like any inout parameter.
    const auto& bump = function("Counter.bump");
    ASSERT_EQ(bump.parameters.size(), 2u);
    EXPECT_EQ(bump.parameters[0].value.category, joyeer::ir::ValueCategory::address);
    EXPECT_TRUE(bump.parameters[0].isMutable);
    EXPECT_EQ(bump.parameters[1].name, "amount");

    EXPECT_TRUE(function("Counter.finish").parameters[0].isConsuming);

    // The call passes the receiver first, then the written arguments.
    const auto& run = function("run");
    size_t bumpCalls = 0;
    for (const auto& block : run.blocks) {
        for (const auto& instruction : block.instructions) {
            if (instruction.opcode != joyeer::ir::Opcode::call || !instruction.callee) continue;
            if (result.module->functions[*instruction.callee].name == "Counter.bump") {
                ++bumpCalls;
                EXPECT_EQ(instruction.operands.size(), 2u);
            }
        }
    }
    EXPECT_EQ(bumpCalls, 1u);

    // A method named `main` is qualified and never becomes the program entry.
    EXPECT_EQ(function("Counter.main").isRootModule, true);
    const auto entries = std::count_if(
            result.module->functions.begin(), result.module->functions.end(),
            [](const auto& candidate) { return candidate.name == "main"; });
    EXPECT_EQ(entries, 0);
}

TEST_F(IRLoweringTest, GenericInstancesHaveConcreteTypesDirectWitnessCallsAndSourceScopes) {
    ASSERT_NO_FATAL_FAILURE(lower(readFixture("type-checking/generics.joyeer")));
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    EXPECT_EQ(std::count_if(result.module->functions.begin(), result.module->functions.end(),
            [](const auto& candidate) { return candidate.name == "read<Number>"; }), 1);
    EXPECT_EQ(opcodeCount(function("read<Number>"), joyeer::ir::Opcode::call), 1u);
    for (const auto& block : function("read<Number>").blocks) {
        for (const auto& instruction : block.instructions) {
            if (instruction.opcode != joyeer::ir::Opcode::call) continue;
            ASSERT_TRUE(instruction.callee.has_value());
            EXPECT_EQ(result.module->functions[*instruction.callee].name, "Number.read");
            ASSERT_TRUE(instruction.debugLocation.has_value());
            ASSERT_TRUE(instruction.debugLocation->scope.has_value());
            EXPECT_EQ(result.module->debugScopes[*instruction.debugLocation->scope].function,
                      function("read<Number>").id);
        }
    }
    EXPECT_TRUE(std::none_of(result.module->types.begin(), result.module->types.end(),
            [](const auto& type) { return type.kind == joyeer::typing::TypeKind::typeParameter; }));
}

TEST_F(IRLoweringTest, GenericConsumptionAndCopyableCopiesFollowTheCheckedBody) {
    ASSERT_NO_FATAL_FAILURE(lower(
            "struct Ticket { let n: Int\n    deinit() {} }\n"
            "func identity<T>(value: consuming T): T { return value }\n"
            "func duplicate<T: Copyable>(value: T): [T] { return [value, value] }\n"
            "func main() { let a = identity(value: consume Ticket(n: 1))\n"
            "let b = identity(value: consume 2)\nlet c = duplicate(value: \"x\") }\n"));
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    EXPECT_EQ(opcodeCount(function("identity<Ticket>"), joyeer::ir::Opcode::copyValue), 0u);
    EXPECT_EQ(opcodeCount(function("identity<Int>"), joyeer::ir::Opcode::copyValue), 0u);
    EXPECT_EQ(opcodeCount(function("duplicate<String>"), joyeer::ir::Opcode::copyValue), 2u);
    EXPECT_TRUE(joyeer::ir::Verifier().verify(*result.module).succeeded());
}

TEST_F(IRLoweringTest, VerifierRejectsATypeParameterEvenInAnUnusedTypeRecord) {
    ASSERT_NO_FATAL_FAILURE(lower("func main() {}\n"));
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    auto& type = result.module->types.emplace_back();
    type.id = static_cast<joyeer::ir::TypeId>(result.module->types.size() - 1);
    type.kind = joyeer::typing::TypeKind::typeParameter;
    type.name = "T";
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*result.module).succeeded());
}

TEST_F(IRLoweringTest, VerifierRejectsUsedGenericPlaceholdersAndTypesContainingThem) {
    for (const auto nested : {false, true}) {
        ASSERT_NO_FATAL_FAILURE(lower(
                "func sink<T>(value: T) {}\nfunc main() { sink(value: 1) }\n"));
        ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
        const auto placeholder = std::find_if(result.module->types.begin(), result.module->types.end(),
                [](const auto& type) {
                    return type.kind == joyeer::typing::TypeKind::error && type.name == "T";
                });
        ASSERT_NE(placeholder, result.module->types.end());
        auto type = placeholder->id;
        if (nested) {
            const auto id = static_cast<joyeer::ir::TypeId>(result.module->types.size());
            result.module->types.push_back({id, "[T]", joyeer::typing::TypeKind::array,
                                           joyeer::semantic::invalidSymbolId, {type}});
            type = id;
        }
        auto& main = result.module->functions[function("main").id];
        joyeer::ir::Instruction allocation {joyeer::ir::Opcode::stackAllocate};
        allocation.result = joyeer::ir::Value {1000000, type, joyeer::ir::ValueCategory::address};
        main.blocks[0].instructions.insert(main.blocks[0].instructions.begin(), std::move(allocation));
        const auto verification = joyeer::ir::Verifier().verify(*result.module);
        EXPECT_FALSE(verification.succeeded());
        EXPECT_TRUE(std::any_of(verification.errors.begin(), verification.errors.end(),
                [](const auto& error) { return error.message.find("non-concrete type") != std::string::npos; }))
                << joyeer::ir::dump(verification);
    }
}

TEST_F(IRLoweringTest, GenericDependencyUsesTheRootTypesConformanceAndSourceFile) {
    result = joyeer::testing::lowerGraph({
        { "app", "main.joyeer",
          "import library\nstruct Number { let value: Int }\n"
          "extension Number: library.Readable { func read(): Int { return self.value } }\n"
          "func main() { print(value: library.read(value: Number(value: 7))) }\n" },
        { "library", "library.joyeer",
          "public protocol Readable { func read(): Int }\n"
          "public func read<T: Readable>(value: T): Int { return value.read() }\n" },
    });
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto& instance = function("library.read<Number>");
    ASSERT_TRUE(instance.debugLocation.has_value());
    EXPECT_EQ(instance.debugLocation->span.sourceId, 1u);
    for (const auto& block : instance.blocks) {
        for (const auto& instruction : block.instructions) {
            if (instruction.opcode != joyeer::ir::Opcode::call) continue;
            ASSERT_TRUE(instruction.callee.has_value());
            EXPECT_EQ(result.module->functions[*instruction.callee].name, "Number.read");
        }
    }
    EXPECT_TRUE(joyeer::ir::Verifier().verify(*result.module).succeeded());
}

TEST_F(IRLoweringTest, DiscoversGenericCallsInMethodsAndDeinitializers) {
    ASSERT_NO_FATAL_FAILURE(lower(
            "func identity<T: Copyable>(value: T): T { return value }\n"
            "struct Ticket {\n    let n: Int\n"
            "    func read(): Int { return identity(value: self.n) }\n"
            "    deinit() { print(value: identity(value: self.n)) }\n}\n"));
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    EXPECT_EQ(opcodeCount(function("Ticket.read"), joyeer::ir::Opcode::call), 1u);
    EXPECT_EQ(opcodeCount(function("identity<Int>"), joyeer::ir::Opcode::copyValue), 0u);
}

TEST_F(IRLoweringTest, GenericInstanceNamesDisambiguatePrivateFunctionsAndTypeArguments) {
    result = joyeer::testing::lowerGraph({
        { "app", "main.joyeer",
          "private struct Box { let value: Int }\n"
          "func identity<T: Copyable>(value: T): T { return value }\n"
          "private func helper<T: Copyable>(value: T): T { return value }\n"
          "func main() { let box = identity(value: Box(value: 7))\n"
          "print(value: helper(value: box.value)) }\n" },
        { "app", "other.joyeer",
          "private struct Box { let value: String }\n"
          "private func helper<T: Copyable>(value: T): T { return value }\n"
          "func other() { let box = identity(value: Box(value: \"x\"))\n"
          "print(value: helper(value: box.value)) }\n" },
    });
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    EXPECT_EQ(function("app.identity<app.Box@0>").parameters.size(), 1u);
    EXPECT_EQ(function("app.identity<app.Box@1>").parameters.size(), 1u);
    EXPECT_EQ(function("app.helper@0<Int>").parameters.size(), 1u);
    EXPECT_EQ(function("app.helper@1<String>").parameters.size(), 1u);
}

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

TEST_F(IRLoweringTest, FoldsScalarConstantsAndSharesArrayConstantData) {
    lower(R"JOYEER(let mask: UInt32 = 0xFF << 8 | 1
let table: [UInt32] = [mask, 2, 3]
let name: String = "a" + "b"

func first(): UInt32 { return table[0] }
func second(): UInt32 { return table[1] + mask }
func label(): String { return name }
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    // The array is stored once however many functions use it; scalars fold into uses.
    ASSERT_EQ(result.module->constants.size(), 1u);
    EXPECT_EQ(result.module->constants[0].name, "table");
    EXPECT_EQ(opcodeCount(function("first"), joyeer::ir::Opcode::constantValue), 1u);
    EXPECT_EQ(opcodeCount(function("second"), joyeer::ir::Opcode::constantValue), 1u);
    EXPECT_EQ(opcodeCount(function("second"), joyeer::ir::Opcode::integerConstant), 2u);
    EXPECT_EQ(opcodeCount(function("label"), joyeer::ir::Opcode::stringConstant), 1u);
    // count (8) + 3 elements * 4 bytes
    EXPECT_EQ(result.module->constants[0].data.size(), 8u + 3u * 4u);
}

TEST_F(IRLoweringTest, SharesPublicConstantsAcrossModules) {
    result = joyeer::testing::lowerGraph({
        { "app", "main.joyeer",
          "import dep\nfunc main() {\n"
          "print(value: dep.polynomial)\nprint(value: dep.table[1])\n"
          "print(value: table.count)\n}\n" },
        { "dep", "constants.joyeer",
          "public let polynomial: UInt32 = 0xEDB8_8320\n"
          "public let table: [UInt32] = [1, 2, 3]\n" },
    });
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    ASSERT_EQ(result.module->constants.size(), 1u);
    EXPECT_EQ(result.module->constants[0].name, "table");
}

// Decision 0015, stage 6: a `deinit` is lowered like a method taking its struct's address, the
// structure records it, and destroying a live value runs it before the fields.
const joyeer::ir::StructureDefinition& structureNamed(
        const joyeer::ir::Module& module, const std::string& name) {
    const auto found = std::find_if(module.structures.begin(), module.structures.end(),
            [&name](const auto& structure) { return structure.name == name; });
    EXPECT_NE(found, module.structures.end());
    return *found;
}

TEST_F(IRLoweringTest, ADeinitIsLoweredAndRecordedOnItsStructure) {
    ASSERT_NO_FATAL_FAILURE(lower("struct Handle {\n    var fd: Int\n    deinit() { }\n}\n"));
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);

    const auto& handle = structureNamed(*result.module, "Handle");
    ASSERT_TRUE(handle.deinitializer.has_value());
    const auto& deinitializer = result.module->functions.at(*handle.deinitializer);
    EXPECT_EQ(deinitializer.name, "Handle.deinit");
    ASSERT_EQ(deinitializer.parameters.size(), 1u);
    EXPECT_EQ(deinitializer.parameters[0].value.category, joyeer::ir::ValueCategory::address);
    EXPECT_EQ(deinitializer.parameters[0].value.type, handle.type);
    EXPECT_FALSE(deinitializer.returnsValue);

    // The hidden live flag ends the layout, so a moved-from value never runs the `deinit`.
    ASSERT_EQ(handle.fields.size(), 2u);
    EXPECT_EQ(handle.fields.back().name, joyeer::ir::liveFieldName);
    EXPECT_TRUE(joyeer::ir::requiresDestruction(*result.module, handle.type));
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    EXPECT_TRUE(verification.succeeded());
}

TEST_F(IRLoweringTest, AStructWithoutDeinitKeepsItsLayoutAndNeedsNoDestruction) {
    ASSERT_NO_FATAL_FAILURE(lower("struct Plain { var fd: Int }\n"));
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto& plain = structureNamed(*result.module, "Plain");
    EXPECT_FALSE(plain.deinitializer.has_value());
    EXPECT_EQ(plain.fields.size(), 1u);
    EXPECT_FALSE(joyeer::ir::requiresDestruction(*result.module, plain.type));
}

TEST_F(IRLoweringTest, AHolderOfADeinitStructRequiresDestruction) {
    ASSERT_NO_FATAL_FAILURE(lower("struct Handle {\n    var fd: Int\n    deinit() { }\n}\n"
                                  "struct Holder { var handle: Handle }\n"));
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    EXPECT_TRUE(joyeer::ir::requiresDestruction(
            *result.module, structureNamed(*result.module, "Holder").type));
}

TEST_F(IRLoweringTest, ConstructingAStructWithDeinitSetsTheLiveFlag) {
    ASSERT_NO_FATAL_FAILURE(lower("struct Handle {\n    var fd: Int\n    deinit() { }\n}\n"
                                  "func open(fd: Int): Handle { return Handle(fd: fd) }\n"));
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    for (const auto& block : function("open").blocks) {
        for (const auto& instruction : block.instructions) {
            if (instruction.opcode != joyeer::ir::Opcode::constructStruct) continue;
            EXPECT_EQ(instruction.operands.size(), 2u);
            return;
        }
    }
    FAIL() << "no constructStruct in open";
}

TEST_F(IRLoweringTest, ReturningAnOwningLocalOfANoncopyableTypeMovesIt) {
    ASSERT_NO_FATAL_FAILURE(lower("struct Handle {\n    var fd: Int\n    deinit() { }\n}\n"
                                  "func make(fd: Int): Handle {\n"
                                  "    let handle = Handle(fd: fd)\n"
                                  "    return handle\n"
                                  "}\n"));
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto& make = function("make");
    EXPECT_EQ(opcodeCount(make, joyeer::ir::Opcode::copyValue), 0u);
    EXPECT_EQ(opcodeCount(make, joyeer::ir::Opcode::take), 1u);
}

TEST_F(IRLoweringTest, ArrayRemoveLastLowersToOneInstructionWithAnOwnedOptionalResult) {
    ASSERT_NO_FATAL_FAILURE(lower("func pop(): Int? {\n"
                                  "    var numbers: [Int] = [1, 2]\n"
                                  "    return &numbers.removeLast()\n"
                                  "}\n"));
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    EXPECT_EQ(opcodeCount(function("pop"), joyeer::ir::Opcode::arrayRemoveLast), 1u);
    EXPECT_TRUE(joyeer::ir::Verifier().verify(*result.module).succeeded());
}

TEST_F(IRLoweringTest, AMatchThatBindsANoncopyablePayloadMovesItOutOfTheScrutinee) {
    ASSERT_NO_FATAL_FAILURE(lower("struct Handle {\n    var fd: Int\n    deinit() { }\n}\n"
                                  "enum Slot {\n    Full(Handle),\n    Empty,\n}\n"
                                  "func peek(slot: consuming Slot): Int {\n"
                                  "    return match consume slot {\n"
                                  "        .Full(handle) => handle.fd,\n"
                                  "        .Empty => 0,\n"
                                  "    }\n"
                                  "}\n"));
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto& peek = function("peek");
    // The payload is addressed in place and taken out, never copied.
    EXPECT_EQ(opcodeCount(peek, joyeer::ir::Opcode::payloadAddress), 1u);
    EXPECT_EQ(opcodeCount(peek, joyeer::ir::Opcode::copyValue), 0u);
    EXPECT_GE(opcodeCount(peek, joyeer::ir::Opcode::take), 1u);
    EXPECT_TRUE(joyeer::ir::Verifier().verify(*result.module).succeeded());
}

TEST_F(IRLoweringTest, AMatchThatBindsOnlyCopyablePayloadsStillCopiesThem) {
    ASSERT_NO_FATAL_FAILURE(lower("enum Slot {\n    Full(String),\n    Empty,\n}\n"
                                  "func peek(slot: borrowing Slot): Int {\n"
                                  "    return match slot {\n"
                                  "        .Full(text) => text.count,\n"
                                  "        .Empty => 0,\n"
                                  "    }\n"
                                  "}\n"));
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    EXPECT_EQ(opcodeCount(function("peek"), joyeer::ir::Opcode::payloadAddress), 0u);
}

// The verifier rules of the array_remove_last and payload_address instructions, one malformed
// module per rule.
class NewOpcodeVerifierTest : public IRLoweringTest {
protected:
    void lowerPrograms() {
        ASSERT_NO_FATAL_FAILURE(lower(
                "struct Handle {\n    var fd: Int\n    deinit() { }\n}\n"
                "enum Slot {\n    Full(Handle),\n    Empty,\n}\n"
                "func pop(): Int? {\n    var numbers: [Int] = [1]\n    return &numbers.removeLast()\n}\n"
                "func peek(slot: consuming Slot): Int {\n"
                "    return match consume slot {\n        .Full(handle) => handle.fd,\n        .Empty => 0,\n    }\n}\n"));
        ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    }

    // True when the verifier rejects the module after `mutate` changed the first `opcode`
    // instruction of `name`.
    template<class Mutate>
    bool rejects(const std::string& name, joyeer::ir::Opcode opcode, Mutate&& mutate) {
        auto module = *result.module;
        for (auto& candidate : module.functions) {
            if (candidate.name != name) continue;
            for (auto& block : candidate.blocks) {
                for (auto& instruction : block.instructions) {
                    if (instruction.opcode != opcode) continue;
                    mutate(instruction, module);
                    return !joyeer::ir::Verifier().verify(module).succeeded();
                }
            }
        }
        ADD_FAILURE() << "no " << name << " instruction to mutate";
        return false;
    }

    // True when the verifier rejects the module after `mutate` and one of its messages contains `text`,
    // so the rule under test is the one that fired and not a side effect of the mutation.
    template<class Mutate>
    bool rejectsWith(const std::string& name, joyeer::ir::Opcode opcode, Mutate&& mutate,
            const std::string& text) {
        auto module = *result.module;
        for (auto& candidate : module.functions) {
            if (candidate.name != name) continue;
            for (auto& block : candidate.blocks) {
                for (auto& instruction : block.instructions) {
                    if (instruction.opcode != opcode) continue;
                    mutate(instruction, module);
                    const auto verification = joyeer::ir::Verifier().verify(module);
                    return std::any_of(verification.errors.begin(), verification.errors.end(),
                            [&text](const auto& error) { return error.message.find(text) != std::string::npos; });
                }
            }
        }
        ADD_FAILURE() << "no " << name << " instruction to mutate";
        return false;
    }

    static joyeer::ir::TypeId typeNamed(const joyeer::ir::Module& module, const std::string& name) {
        return std::find_if(module.types.begin(), module.types.end(),
                [&name](const auto& type) { return type.name == name; })->id;
    }
};

TEST_F(NewOpcodeVerifierTest, TheLoweredModuleVerifies) {
    lowerPrograms();
    EXPECT_TRUE(joyeer::ir::Verifier().verify(*result.module).succeeded());
}

TEST_F(NewOpcodeVerifierTest, ArrayRemoveLastNeedsExactlyOneOperand) {
    lowerPrograms();
    using joyeer::ir::Opcode;
    EXPECT_TRUE(rejects("pop", Opcode::arrayRemoveLast, [](auto& instruction, auto&) {
        instruction.operands.clear();
    }));
    EXPECT_TRUE(rejects("pop", Opcode::arrayRemoveLast, [](auto& instruction, auto&) {
        instruction.operands.push_back(instruction.operands.front());
    }));
}

TEST_F(NewOpcodeVerifierTest, ArrayRemoveLastNeedsAnOptionalOfTheElementType) {
    lowerPrograms();
    using joyeer::ir::Opcode;
    EXPECT_TRUE(rejects("pop", Opcode::arrayRemoveLast, [](auto& instruction, auto& module) {
        instruction.result->type = typeNamed(module, "Int");
    }));
    EXPECT_TRUE(rejects("pop", Opcode::arrayRemoveLast, [](auto& instruction, auto& module) {
        instruction.result->type = typeNamed(module, "Handle");
    }));
    EXPECT_TRUE(rejects("pop", Opcode::arrayRemoveLast, [](auto& instruction, auto&) {
        instruction.result->category = joyeer::ir::ValueCategory::address;
    }));
}

TEST_F(NewOpcodeVerifierTest, PayloadAddressNeedsAKnownCaseAndAValidIndex) {
    lowerPrograms();
    using joyeer::ir::Opcode;
    EXPECT_TRUE(rejects("peek", Opcode::payloadAddress, [](auto& instruction, auto&) {
        instruction.integerValue = 7;
    }));
    EXPECT_TRUE(rejects("peek", Opcode::payloadAddress, [](auto& instruction, auto&) {
        instruction.integerValue = -1;
    }));
    EXPECT_TRUE(rejects("peek", Opcode::payloadAddress, [](auto& instruction, auto&) {
        instruction.symbol = 987654;
    }));
    EXPECT_TRUE(rejects("peek", Opcode::payloadAddress, [](auto& instruction, auto&) {
        instruction.symbol.reset();
    }));
}

TEST_F(NewOpcodeVerifierTest, PayloadAddressNeedsAnAddressOfTheEnumAndAnAddressResult) {
    lowerPrograms();
    using joyeer::ir::Opcode;
    EXPECT_TRUE(rejects("peek", Opcode::payloadAddress, [](auto& instruction, auto&) {
        instruction.result->category = joyeer::ir::ValueCategory::value;
    }));
    EXPECT_TRUE(rejects("peek", Opcode::payloadAddress, [](auto& instruction, auto& module) {
        instruction.result->type = typeNamed(module, "Int");
    }));
    EXPECT_TRUE(rejects("peek", Opcode::payloadAddress, [](auto& instruction, auto&) {
        instruction.operands.clear();
    }));
}

TEST_F(IRLoweringTest, TheVerifierRejectsADeinitWithTheWrongSignature) {
    ASSERT_NO_FATAL_FAILURE(lower("struct Handle {\n    var fd: Int\n    deinit() { }\n}\n"
                                  "func other(): Int { return 1 }\n"));
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    auto module = *result.module;
    for (auto& structure : module.structures) {
        if (structure.name == "Handle") structure.deinitializer = function("other").id;
    }
    EXPECT_FALSE(joyeer::ir::Verifier().verify(module).succeeded());
}

TEST_F(IRLoweringTest, TheVerifierRejectsADeinitWithoutTheHiddenLiveFlag) {
    ASSERT_NO_FATAL_FAILURE(lower("struct Handle {\n    var fd: Int\n    deinit() { }\n}\n"));
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    auto module = *result.module;
    for (auto& structure : module.structures) {
        if (structure.name == "Handle") structure.fields.pop_back();
    }
    EXPECT_FALSE(joyeer::ir::Verifier().verify(module).succeeded());
}

// Each rule of the structure `deinit` invariants, with the error it must report.
class DeinitVerifierTest : public IRLoweringTest {
protected:
    void lowerHandle(const std::string& extra = "") {
        ASSERT_NO_FATAL_FAILURE(lower("struct Handle {\n    var fd: Int\n    deinit() { }\n}\n"
                                      "struct Plain { var fd: Int }\n" + extra));
        ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    }

    bool rejects(joyeer::ir::Module module, joyeer::ir::VerificationErrorId id) const {
        const auto verification = joyeer::ir::Verifier().verify(module);
        return std::any_of(verification.errors.begin(), verification.errors.end(),
                [id](const auto& error) { return error.id == id; });
    }

    static joyeer::ir::StructureDefinition& named(joyeer::ir::Module& module, const std::string& name) {
        return *std::find_if(module.structures.begin(), module.structures.end(),
                [&name](const auto& structure) { return structure.name == name; });
    }
};

TEST_F(DeinitVerifierTest, AnUnknownDeinitFunctionIsAnInvalidReference) {
    lowerHandle();
    auto module = *result.module;
    named(module, "Handle").deinitializer = 9999;
    EXPECT_TRUE(rejects(module, joyeer::ir::VerificationErrorId::invalidReference));
}

TEST_F(DeinitVerifierTest, ADeinitMustTakeTheStructsAddress) {
    lowerHandle("func byValue(handle: Handle) { }\n");
    auto module = *result.module;
    named(module, "Handle").deinitializer = function("byValue").id;
    EXPECT_TRUE(rejects(module, joyeer::ir::VerificationErrorId::invalidInstruction));
}

TEST_F(DeinitVerifierTest, ADeinitMustNotReturnAValue) {
    lowerHandle("struct Other {\n    var fd: Int\n    mutating func fetch(): Int { return self.fd }\n}\n");
    auto module = *result.module;
    named(module, "Handle").deinitializer = function("Other.fetch").id;
    EXPECT_TRUE(rejects(module, joyeer::ir::VerificationErrorId::invalidInstruction));
}

TEST_F(DeinitVerifierTest, TheLiveFlagMustBeABoolean) {
    lowerHandle();
    auto module = *result.module;
    auto& handle = named(module, "Handle");
    handle.fields.back().type = handle.fields.front().type;
    EXPECT_TRUE(rejects(module, joyeer::ir::VerificationErrorId::invalidReference));
}

TEST_F(DeinitVerifierTest, TheLiveFlagMustBeLast) {
    lowerHandle();
    auto module = *result.module;
    auto& handle = named(module, "Handle");
    std::swap(handle.fields.front(), handle.fields.back());
    EXPECT_TRUE(rejects(module, joyeer::ir::VerificationErrorId::invalidReference));
}

TEST_F(DeinitVerifierTest, TheLiveFlagHasNoSourceSymbol) {
    lowerHandle();
    auto module = *result.module;
    named(module, "Handle").fields.back().symbol = 7;
    EXPECT_TRUE(rejects(module, joyeer::ir::VerificationErrorId::invalidReference));
}

TEST_F(DeinitVerifierTest, AStructWithoutADeinitHasNoLiveFlag) {
    lowerHandle();
    auto module = *result.module;
    auto& plain = named(module, "Plain");
    plain.fields.push_back(named(module, "Handle").fields.back());
    EXPECT_TRUE(rejects(module, joyeer::ir::VerificationErrorId::invalidReference));
}

TEST_F(DeinitVerifierTest, ALoweredModuleVerifiesCleanly) {
    lowerHandle();
    EXPECT_TRUE(joyeer::ir::Verifier().verify(*result.module).succeeded());
}

TEST_F(IRLoweringTest, RejectsInvalidTopLevelConstants) {
    const std::vector<std::pair<std::string, std::string>> cases {
        { "var x: Int = 1\n", "top-level 'var'" },
        { "let x = 1\n", "explicit type" },
        { "let x: Int8 = 100 + 100\n", "integer overflow" },
        { "let x: Int = 1 / (1 - 1)\n", "division by zero" },
        { "let x: Int = 1 << 64\n", "shift count" },
        { "let x: UInt8 = (300 as Int) as UInt8\n", "conversion out of range" },
        { "let a: Int = b\nlet b: Int = a\n", "cycle" },
        { "func f(): Int { return 1 }\nlet x: Int = f()\n", "not a compile-time constant" },
        { "struct P { let x: Int }\nlet p: P = P(x: 1)\n", "must be a Bool, number, String" },
    };
    for (const auto& [text, expected] : cases) {
        SCOPED_TRACE(text);
        ASSERT_NO_FATAL_FAILURE(lower(text));
        ASSERT_FALSE(result.succeeded());
        const auto dumped = joyeer::lowering::dump(result.diagnostics);
        EXPECT_NE(dumped.find("ir-lowering.invalid-constant"), std::string::npos) << dumped;
        EXPECT_NE(dumped.find(expected), std::string::npos) << dumped;
    }
}

TEST_F(IRLoweringTest, PreservesGraphSymbolIdentityAndFileLocalDebugSpans) {
    result = joyeer::testing::lowerGraph(joyeer::testing::collidingGraphSources());
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    ASSERT_EQ(result.module->sourceFiles.size(), 4u);
    EXPECT_EQ(result.module->sourceFiles.back().fileName, "concurrency.joyeer");
    EXPECT_FALSE(result.module->sourceInfo.has_value());
    std::unordered_map<joyeer::semantic::SymbolId, joyeer::ir::FunctionId> symbols;
    size_t helpers = 0;
    size_t entries = 0;
    size_t dependencyMains = 0;
    for (const auto& item : result.module->functions) {
        if (item.isExternal) continue;
        ASSERT_TRUE(item.symbol.has_value());
        EXPECT_TRUE(symbols.emplace(*item.symbol, item.id).second);
        ASSERT_TRUE(item.debugLocation.has_value());
        if (item.name == "helper") ++helpers;
        if (item.name == "main") {
            if (item.isRootModule) ++entries;
            else {
                ++dependencyMains;
                EXPECT_EQ(item.debugLocation->span.sourceId, 2u);
            }
        }
        for (const auto& block : item.blocks) {
            for (const auto& instruction : block.instructions) {
                ASSERT_TRUE(instruction.debugLocation.has_value());
                EXPECT_EQ(instruction.debugLocation->span.sourceId,
                          item.debugLocation->span.sourceId);
                if (instruction.opcode == joyeer::ir::Opcode::call) {
                    ASSERT_TRUE(instruction.callee.has_value());
                    const auto& callee = result.module->functions.at(*instruction.callee);
                    if (callee.name == "helper") {
                        ASSERT_TRUE(callee.debugLocation.has_value());
                        EXPECT_EQ(callee.debugLocation->span.sourceId,
                                  item.debugLocation->span.sourceId);
                    }
                    if (callee.name == "produce") EXPECT_FALSE(callee.isRootModule);
                }
            }
        }
    }
    EXPECT_EQ(helpers, 3u);
    EXPECT_EQ(entries, 1u);
    EXPECT_EQ(dependencyMains, 1u);
    ASSERT_EQ(result.module->structures.size(), 3u);
    EXPECT_NE(result.module->structures[0].type, result.module->structures[1].type);
    EXPECT_NE(result.module->structures[0].type, result.module->structures[2].type);
    EXPECT_TRUE(joyeer::ir::Verifier().verify(*result.module).succeeded());
}

TEST_F(IRLoweringTest, LowersQualifiedConstructorsAndEnumCasesWithoutEvaluatingModulePrefixes) {
    result = joyeer::testing::lowerGraph({
        { "app", "main.joyeer",
          "import dep\nfunc main() {\n"
          "let record = dep.Record(value: 7)\n"
          "let empty = dep.Choice.Empty\n"
          "let payload = dep.Choice.Value(9)\n"
          "print(value: dep.answer())\n}\n" },
        { "dep", "types.joyeer",
          "public struct Record { public var value: Int }\n"
          "public enum Choice { Empty, Value(Int) }\n"
          "public func answer(): Int { return 42 }\n" },
    });
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    EXPECT_EQ(opcodeCount(function("main"), joyeer::ir::Opcode::constructStruct), 1u);
    EXPECT_EQ(opcodeCount(function("main"), joyeer::ir::Opcode::constructEnum), 2u);
    EXPECT_EQ(opcodeCount(function("main"), joyeer::ir::Opcode::call), 2u);
    EXPECT_TRUE(joyeer::ir::Verifier().verify(*result.module).succeeded());
}

TEST_F(IRLoweringTest, LowersUnitCallsAndReturnsWithoutLosingEffects) {
    lower("func effect() { print(value: 42) }\n"
          "func complete(): Result<Void, String> { return .Ok(effect()) }\n"
          "func forward(): Void { return effect() }\n");
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    EXPECT_EQ(opcodeCount(function("complete"), joyeer::ir::Opcode::call), 1u);
    EXPECT_EQ(opcodeCount(function("complete"), joyeer::ir::Opcode::unitConstant), 1u);
    EXPECT_EQ(opcodeCount(function("complete"), joyeer::ir::Opcode::constructEnum), 1u);
    EXPECT_EQ(opcodeCount(function("forward"), joyeer::ir::Opcode::returnVoid), 1u);
    EXPECT_EQ(opcodeCount(function("forward"), joyeer::ir::Opcode::returnValue), 0u);
    EXPECT_TRUE(joyeer::ir::Verifier().verify(*result.module).succeeded());
}

TEST_F(IRLoweringTest, LowersUnitNativeFixture) {
    lower(readFixture("e2e/unit_values.joyeer"));
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    EXPECT_TRUE(joyeer::ir::Verifier().verify(*result.module).succeeded());
}

TEST_F(IRLoweringTest, DictionaryGetBorrowsInputsAndOwnsItsResultWithoutExtraCopies) {
    lower("func lookup(values: [String: String], key: String): String? {\n"
          "return values.get(key: key)\n}\n"
          "func propagate(values: [String: String], key: String): String? {\n"
          "return values.get(key: key)?\n}\n");
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    EXPECT_TRUE(joyeer::ir::Verifier().verify(*result.module).succeeded());
    const auto& lookup = function("lookup");
    EXPECT_EQ(opcodeCount(lookup, joyeer::ir::Opcode::dictionaryGet), 1u);
    EXPECT_EQ(opcodeCount(lookup, joyeer::ir::Opcode::copyValue), 0u);
    EXPECT_EQ(opcodeCount(lookup, joyeer::ir::Opcode::constructDictionary), 0u);
    EXPECT_EQ(opcodeCount(function("propagate"), joyeer::ir::Opcode::dictionaryGet), 1u);
    EXPECT_EQ(opcodeCount(function("propagate"), joyeer::ir::Opcode::copyValue), 0u);
}

TEST_F(IRLoweringTest, LowersDictionaryGetNativeFixture) {
    ASSERT_NO_FATAL_FAILURE(lower(readFixture("e2e/dictionary_get.joyeer")));
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    EXPECT_TRUE(joyeer::ir::Verifier().verify(*result.module).succeeded());
}

TEST_F(IRLoweringTest, MovesPropagatedPayloadsAndCleansFailurePaths) {
    lower(R"JOYEER(func fetch(flag: Bool): Result<String, String> {
if flag { return .Ok("owned" + "!") }
return .Err("bad" + "!")
}
func forward(flag: Bool): Result<Void, String> {
let local = "local" + "!"
let data = fetch(flag: flag)?
print(value: data)
return .Ok(())
}
func borrowed(value: Result<Result<String, String>, String>): Result<String, String> {
return value?
}
func optional(value: Int?): Int? {
let number = value?
return number
}
)JOYEER");
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    ASSERT_TRUE(joyeer::ir::Verifier().verify(*result.module).succeeded());
    const auto& forward = function("forward");
    EXPECT_EQ(opcodeCount(forward, joyeer::ir::Opcode::call), 2u);
    EXPECT_EQ(opcodeCount(forward, joyeer::ir::Opcode::switchPattern), 1u);
    EXPECT_EQ(opcodeCount(forward, joyeer::ir::Opcode::extractPayload), 2u);
    EXPECT_EQ(opcodeCount(forward, joyeer::ir::Opcode::copyValue), 1u)
            << joyeer::ir::dump(*result.module);
    EXPECT_EQ(opcodeCount(forward, joyeer::ir::Opcode::returnValue), 2u);
    EXPECT_GT(opcodeCount(forward, joyeer::ir::Opcode::destroy), 0u);
    const auto& dispatch = forward.blocks[0].instructions.back();
    ASSERT_EQ(dispatch.opcode, joyeer::ir::Opcode::switchPattern);
    ASSERT_TRUE(dispatch.debugLocation.has_value());
    EXPECT_EQ(dispatch.span.offset, source->content.find("fetch(flag: flag)?"));
    EXPECT_FALSE(dispatch.debugLocation->implicitCode);
    EXPECT_EQ(opcodeCount(function("borrowed"), joyeer::ir::Opcode::copyValue), 1u);
    EXPECT_EQ(opcodeCount(function("optional"), joyeer::ir::Opcode::switchPattern), 1u);
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

TEST_F(IRLoweringTest, CarriesLexicalScopesVariablesAndStorageBindings) {
    lower(R"JOYEER(func run(plain: Int, borrowed: borrowing Int, target: inout Int, output: initializing Int) {
let local = plain
if true {
let local = borrowed
print(value: local)
}
&target = local
&output = borrowed
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto& run = function("run");
    ASSERT_TRUE(run.debugScope.has_value());

    const auto variablesForRun = std::count_if(
            result.module->debugVariables.begin(),
            result.module->debugVariables.end(),
            [&run](const auto& variable) { return variable.function == run.id; });
    EXPECT_EQ(variablesForRun, 6u);
    EXPECT_EQ(run.entryDebugVariableBindings.size(), 2u);

    std::vector<const joyeer::ir::DebugVariable*> locals;
    for (const auto& variable : result.module->debugVariables) {
        if (variable.function != run.id) continue;
        if (variable.name == "local") locals.push_back(&variable);
        if (variable.kind == joyeer::ir::DebugVariableKind::parameter) {
            ASSERT_TRUE(variable.parameterIndex.has_value());
            EXPECT_GE(*variable.parameterIndex, 1u);
            EXPECT_LE(*variable.parameterIndex, 4u);
        }
    }
    ASSERT_EQ(locals.size(), 2u);
    EXPECT_NE(locals[0]->scope, locals[1]->scope);

    std::unordered_map<joyeer::ir::DebugVariableId, size_t> bindingCounts;
    for (const auto& binding : run.entryDebugVariableBindings) {
        ++bindingCounts[binding.variable];
    }
    for (const auto& block : run.blocks) {
        for (const auto& instruction : block.instructions) {
            ASSERT_TRUE(instruction.debugLocation.has_value());
            ASSERT_TRUE(instruction.debugLocation->scope.has_value());
            for (const auto& binding : instruction.debugVariableBindings) {
                ++bindingCounts[binding.variable];
            }
        }
    }
    for (const auto& variable : result.module->debugVariables) {
        if (variable.function == run.id) EXPECT_EQ(bindingCounts[variable.id], 1u);
    }

    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    EXPECT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
}

TEST_F(IRLoweringTest, CarriesMatchArmPatternVariables) {
    lower(R"JOYEER(enum Choice { None, Some(Int) }
func run(value: Choice) {
match value {
.Some(payload) => { print(value: payload) },
.None => { print(value: 0) },
}
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto& run = function("run");
    const auto pattern = std::find_if(
            result.module->debugVariables.begin(),
            result.module->debugVariables.end(),
            [&run](const auto& variable) {
                return variable.function == run.id &&
                        variable.kind == joyeer::ir::DebugVariableKind::patternBinding &&
                        variable.name == "payload";
            });
    ASSERT_NE(pattern, result.module->debugVariables.end());
    const auto scope = std::find_if(
            result.module->debugScopes.begin(),
            result.module->debugScopes.end(),
            [&pattern](const auto& candidate) { return candidate.id == pattern->scope; });
    ASSERT_NE(scope, result.module->debugScopes.end());
    EXPECT_EQ(scope->kind, joyeer::ir::DebugScopeKind::lexicalBlock);
    EXPECT_TRUE(scope->parent.has_value());

    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    EXPECT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
}

TEST_F(IRLoweringTest, OmitsVariablesWhoseInitializersDiverge) {
    lower(R"JOYEER(func run(): Int {
let value: Int = return 1
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto& run = function("run");
    EXPECT_TRUE(std::none_of(
            result.module->debugVariables.begin(),
            result.module->debugVariables.end(),
            [&run](const auto& variable) {
                return variable.function == run.id && variable.name == "value";
            }));
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    EXPECT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
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

TEST_F(IRLoweringTest, AcquiresOwnedReplacementBeforeDestroyingOverwrittenStorage) {
    lower(R"JOYEER(func run() {
var text = "before"
text = text
print(value: text)
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto& instructions = function("run").blocks[0].instructions;
    const auto initialStore = std::find_if(
            instructions.begin(),
            instructions.end(),
            [](const auto& instruction) {
                return instruction.opcode == joyeer::ir::Opcode::store;
            });
    ASSERT_NE(initialStore, instructions.end());
    const auto replacementCopy = std::find_if(
            initialStore + 1,
            instructions.end(),
            [](const auto& instruction) {
                return instruction.opcode == joyeer::ir::Opcode::copyValue;
            });
    ASSERT_NE(replacementCopy, instructions.end());
    const auto overwrittenDestroy = std::find_if(
            replacementCopy + 1,
            instructions.end(),
            [](const auto& instruction) {
                return instruction.opcode == joyeer::ir::Opcode::destroy;
            });
    ASSERT_NE(overwrittenDestroy, instructions.end());
    const auto replacementStore = std::find_if(
            overwrittenDestroy + 1,
            instructions.end(),
            [](const auto& instruction) {
                return instruction.opcode == joyeer::ir::Opcode::store;
            });
    EXPECT_NE(replacementStore, instructions.end());
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

TEST_F(IRLoweringTest, LowersLogicalAndWithConditionalRightEvaluation) {
    lower(R"JOYEER(func probe(): Bool { return true }
func run(flag: Bool): Bool {
return flag && probe()
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto& run = function("run");
    EXPECT_EQ(opcodeCount(run, joyeer::ir::Opcode::logicalAnd), 0u);
    EXPECT_EQ(opcodeCount(run, joyeer::ir::Opcode::conditionalBranch), 1u);
    const auto right = std::find_if(
            run.blocks.begin(), run.blocks.end(),
            [](const auto& block) { return block.name == "and.rhs"; });
    ASSERT_NE(right, run.blocks.end());
    ASSERT_EQ(run.blocks[0].instructions.back().opcode, joyeer::ir::Opcode::conditionalBranch);
    EXPECT_EQ(run.blocks[0].instructions.back().targets[0], right->id);
    EXPECT_TRUE(std::none_of(
            run.blocks[0].instructions.begin(), run.blocks[0].instructions.end(),
            [](const auto& instruction) { return instruction.opcode == joyeer::ir::Opcode::call; }));
    EXPECT_TRUE(std::any_of(
            right->instructions.begin(), right->instructions.end(),
            [](const auto& instruction) { return instruction.opcode == joyeer::ir::Opcode::call; }));
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    EXPECT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
}

TEST_F(IRLoweringTest, PreservesContinuationWhenLogicalAndRightReturns) {
    lower(R"JOYEER(func run(flag: Bool): Bool {
return flag && if true { return true } else { false }
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto& run = function("run");
    EXPECT_EQ(opcodeCount(run, joyeer::ir::Opcode::logicalAnd), 0u);
    EXPECT_EQ(opcodeCount(run, joyeer::ir::Opcode::conditionalBranch), 2u);
    EXPECT_EQ(opcodeCount(run, joyeer::ir::Opcode::returnValue), 2u);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    EXPECT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
}

TEST_F(IRLoweringTest, LowersLogicalOrWithConditionalRightEvaluation) {
    lower("func probe(): Bool { return false }\n"
          "func run(flag: Bool): Bool { return !flag || probe() }\n");
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto& run = function("run");
    const auto right = std::find_if(run.blocks.begin(), run.blocks.end(),
            [](const auto& block) { return block.name == "or.rhs"; });
    ASSERT_NE(right, run.blocks.end());
    const auto& branch = run.blocks[0].instructions.back();
    ASSERT_EQ(branch.opcode, joyeer::ir::Opcode::conditionalBranch);
    ASSERT_EQ(branch.targets.size(), 2u);
    EXPECT_EQ(branch.targets[1], right->id);
    EXPECT_EQ(opcodeCount(run, joyeer::ir::Opcode::equal), 1u);
    EXPECT_TRUE(std::any_of(right->instructions.begin(), right->instructions.end(),
            [](const auto& instruction) { return instruction.opcode == joyeer::ir::Opcode::call; }));
    EXPECT_TRUE(joyeer::ir::Verifier().verify(*result.module).succeeded());
}

TEST_F(IRLoweringTest, NeverCallsTerminateLogicalOperands) {
    lower(R"JOYEER(func stop(): Never { return stop() }
func rightOr(flag: Bool): Bool { return flag || stop() }
func rightAnd(flag: Bool): Bool { return flag && stop() }
func leftOr(): Bool { return stop() || true }
func leftAnd(): Bool { return stop() && false }
func negated(): Bool { return !stop() }
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    for (const auto* name : { "stop", "rightOr", "rightAnd", "leftOr",
                              "leftAnd", "negated" }) {
        const auto& lowered = function(name);
        bool terminatesAfterCall = false;
        for (const auto& block : lowered.blocks) {
            for (size_t index = 1; index < block.instructions.size(); ++index) {
                terminatesAfterCall |=
                        block.instructions[index - 1].opcode == joyeer::ir::Opcode::call &&
                        block.instructions[index].opcode == joyeer::ir::Opcode::unreachable;
            }
        }
        EXPECT_TRUE(terminatesAfterCall) << name << '\n' << joyeer::ir::dump(*result.module);
    }
    for (const auto* name : { "rightOr", "rightAnd" }) {
        EXPECT_EQ(opcodeCount(function(name), joyeer::ir::Opcode::returnValue), 1u);
    }
}

TEST_F(IRLoweringTest, LoopExitsCleanOnlyExitedScopesAndCarryJumpLocations) {
    const std::string source = R"JOYEER(func run(flag: Bool) {
let outer = "outer" + "!"
while flag {
    let inner = "inner" + "!"
    if flag { break } else { continue }
}
print(value: outer)
}
)JOYEER";
    lower(source);
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto& run = function("run");
    const auto header = std::find_if(run.blocks.begin(), run.blocks.end(),
            [](const auto& block) { return block.name == "while.header"; });
    const auto exit = std::find_if(run.blocks.begin(), run.blocks.end(),
            [](const auto& block) { return block.name == "while.exit"; });
    ASSERT_NE(header, run.blocks.end());
    ASSERT_NE(exit, run.blocks.end());
    std::optional<joyeer::ir::ValueId> outerAddress;
    std::optional<joyeer::ir::ValueId> innerAddress;
    for (const auto& block : run.blocks) {
        for (const auto& instruction : block.instructions) {
            for (const auto& binding : instruction.debugVariableBindings) {
                const auto& variable = result.module->debugVariables.at(binding.variable);
                if (variable.name == "outer") outerAddress = binding.address;
                if (variable.name == "inner") innerAddress = binding.address;
            }
        }
    }
    ASSERT_TRUE(outerAddress.has_value());
    ASSERT_TRUE(innerAddress.has_value());
    size_t jumps = 0;
    for (const auto& block : run.blocks) {
        if (block.instructions.empty()) continue;
        const auto& jump = block.instructions.back();
        if (jump.opcode != joyeer::ir::Opcode::branch) continue;
        const auto spelling = source.substr(jump.span.offset, jump.span.length);
        if (spelling != "break" && spelling != "continue") continue;
        ++jumps;
        ASSERT_EQ(jump.targets.size(), 1u);
        EXPECT_EQ(jump.targets[0], spelling == "break" ? exit->id : header->id);
        ASSERT_GE(block.instructions.size(), 2u);
        const auto& cleanup = block.instructions[block.instructions.size() - 2];
        EXPECT_EQ(cleanup.opcode, joyeer::ir::Opcode::destroy);
        ASSERT_TRUE(cleanup.debugLocation.has_value());
        EXPECT_TRUE(cleanup.debugLocation->implicitCode);
        ASSERT_TRUE(jump.debugLocation.has_value());
        EXPECT_FALSE(jump.debugLocation->implicitCode);
        EXPECT_EQ(jump.debugLocation->span.offset, jump.span.offset);
        bool destroysInner = false;
        for (const auto& instruction : block.instructions) {
            if (instruction.opcode != joyeer::ir::Opcode::destroy) continue;
            ASSERT_EQ(instruction.operands.size(), 1u);
            EXPECT_NE(instruction.operands[0], *outerAddress);
            if (instruction.operands[0] == *innerAddress) destroysInner = true;
        }
        EXPECT_TRUE(destroysInner);
    }
    EXPECT_EQ(jumps, 2u);
    EXPECT_TRUE(joyeer::ir::Verifier().verify(*result.module).succeeded());
}

TEST_F(IRLoweringTest, LowersM006NativeFixture) {
    ASSERT_NO_FATAL_FAILURE(lower(readFixture("e2e/control_flow_arithmetic.joyeer")));
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    EXPECT_TRUE(joyeer::ir::Verifier().verify(*result.module).succeeded());
    EXPECT_EQ(opcodeCount(function("quotient"), joyeer::ir::Opcode::divide), 1u);
    EXPECT_EQ(opcodeCount(function("residue"), joyeer::ir::Opcode::remainder), 1u);
}

TEST_F(IRLoweringTest, DoesNotAppendCleanupAfterDivergingLoopCondition) {
    lower(R"JOYEER(func run() {
let text = "kept"
while if true { return } else { return } {
print(value: text)
}
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    EXPECT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
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

TEST_F(IRLoweringTest, LowersStringUtf8ToOwnedByteArray) {
    lower(R"JOYEER(func bytes(text: String): [UInt8] {
return text.utf8()
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    ASSERT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    const auto& bytes = function("bytes");
    EXPECT_EQ(opcodeCount(bytes, joyeer::ir::Opcode::stringUtf8), 1u);
    EXPECT_EQ(opcodeCount(bytes, joyeer::ir::Opcode::returnValue), 1u);
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

TEST_F(IRLoweringTest, CapturesEnumPayloadBeforeLaterMutation) {
    lower(R"JOYEER(enum Pair { Values(String, Int) }
func change(value: inout String): Int {
&value = "replacement"
return 1
}
func run(): Pair {
var text = "before"
return .Values(text, change(value: &text))
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto& instructions = function("run").blocks[0].instructions;
    const auto construction = std::find_if(
            instructions.begin(), instructions.end(),
            [](const auto& instruction) {
                return instruction.opcode == joyeer::ir::Opcode::constructEnum;
            });
    ASSERT_NE(construction, instructions.end());
    ASSERT_EQ(construction->operands.size(), 2u);
    const auto capture = std::find_if(
            instructions.begin(), construction,
            [&construction](const auto& instruction) {
                return instruction.opcode == joyeer::ir::Opcode::copyValue &&
                        instruction.result->id == construction->operands[0];
            });
    const auto mutation = std::find_if(
            instructions.begin(), construction,
            [this](const auto& instruction) {
                return instruction.opcode == joyeer::ir::Opcode::call &&
                        instruction.callee == function("change").id;
            });
    ASSERT_NE(capture, construction);
    ASSERT_NE(mutation, construction);
    EXPECT_LT(capture, mutation);
}

TEST_F(IRLoweringTest, KeepsConsumedArgumentLiveAcrossLaterEarlyReturn) {
    lower(R"JOYEER(func accept(value: consuming String, flag: Bool) {}
func run(flag: Bool) {
var text = "a" + "b"
accept(value: consume text, flag: if flag { return } else { false })
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto& run = function("run");
    const auto take = std::find_if(
            run.blocks[0].instructions.begin(), run.blocks[0].instructions.end(),
            [](const auto& instruction) { return instruction.opcode == joyeer::ir::Opcode::take; });
    ASSERT_NE(take, run.blocks[0].instructions.end());
    const auto early = std::find_if(
            run.blocks.begin(), run.blocks.end(),
            [](const auto& block) { return block.name == "if.then"; });
    ASSERT_NE(early, run.blocks.end());
    EXPECT_EQ(early->instructions.back().opcode, joyeer::ir::Opcode::returnVoid);
    const auto savedTemporary = std::find_if(
            early->instructions.begin(), early->instructions.end(),
            [&take](const auto& instruction) {
                return instruction.opcode == joyeer::ir::Opcode::store &&
                        instruction.operands[0] == take->result->id;
            });
    ASSERT_NE(savedTemporary, early->instructions.end());
    EXPECT_TRUE(std::any_of(
            savedTemporary, early->instructions.end(),
            [&savedTemporary](const auto& instruction) {
                return instruction.opcode == joyeer::ir::Opcode::destroy &&
                        instruction.operands[0] == savedTemporary->operands[1];
            }));
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    EXPECT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
}

    TEST_F(IRLoweringTest, CapturesStringBinaryOperandBeforeLaterMutation) {
        for (const auto* operation : { "+", "==", "!=" }) {
        SCOPED_TRACE(operation);
        lower(std::string(R"JOYEER(func change(value: inout String): String {
    &value = "replacement"
    return "!"
    }
    func run(input: String) {
    var text = input
    print(value: text )JOYEER") + operation + R"JOYEER( change(value: &text))
    }
    )JOYEER");

        ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
        const auto& instructions = function("run").blocks[0].instructions;
        const auto binary = std::find_if(
            instructions.begin(), instructions.end(),
            [](const auto& instruction) {
                return instruction.opcode == joyeer::ir::Opcode::add ||
                    instruction.opcode == joyeer::ir::Opcode::equal ||
                    instruction.opcode == joyeer::ir::Opcode::notEqual;
            });
        ASSERT_NE(binary, instructions.end());
        const auto capture = std::find_if(
            instructions.begin(), binary,
            [&binary](const auto& instruction) {
                return instruction.opcode == joyeer::ir::Opcode::copyValue &&
                    instruction.result->id == binary->operands[0];
            });
        const auto mutation = std::find_if(
            instructions.begin(), binary,
            [this](const auto& instruction) {
                return instruction.opcode == joyeer::ir::Opcode::call &&
                    instruction.callee == function("change").id;
            });
        ASSERT_NE(capture, binary);
        ASSERT_NE(mutation, binary);
        EXPECT_LT(capture, mutation);
        }
    }

    TEST_F(IRLoweringTest, CleansCapturedBinaryOperandOnEarlyReturn) {
        lower(R"JOYEER(func run(text: String, flag: Bool): String {
    return text + if flag { return "early" } else { "!" }
    }
    )JOYEER");

        ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
        const auto& run = function("run");
        const auto& entry = run.blocks[0].instructions;
        const auto capture = std::find_if(
            entry.begin(), entry.end(),
            [](const auto& instruction) {
            return instruction.opcode == joyeer::ir::Opcode::copyValue;
            });
        ASSERT_NE(capture, entry.end());
        const auto early = std::find_if(
            run.blocks.begin(), run.blocks.end(),
            [](const auto& block) { return block.name == "if.then"; });
        ASSERT_NE(early, run.blocks.end());
        EXPECT_EQ(early->instructions.back().opcode, joyeer::ir::Opcode::returnValue);
        const auto savedTemporary = std::find_if(
            early->instructions.begin(), early->instructions.end(),
            [&capture](const auto& instruction) {
            return instruction.opcode == joyeer::ir::Opcode::store &&
                instruction.operands[0] == capture->result->id;
            });
        ASSERT_NE(savedTemporary, early->instructions.end());
        EXPECT_TRUE(std::any_of(
            savedTemporary, early->instructions.end(),
            [&savedTemporary](const auto& instruction) {
            return instruction.opcode == joyeer::ir::Opcode::destroy &&
                instruction.operands[0] == savedTemporary->operands[1];
            }));
    }

TEST_F(IRLoweringTest, CoercesOptionalBindingsBeforeStoring) {
    lower(R"JOYEER(func run(): Int? {
let value: Int? = 42
return value
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    EXPECT_EQ(opcodeCount(function("run"), joyeer::ir::Opcode::constructEnum), 1u);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    EXPECT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
}

TEST_F(IRLoweringTest, OwnsPatternPayloadAcrossScrutineeMutationAndEarlyReturn) {
    lower(R"JOYEER(func run(input: String?, flag: Bool) {
var message = input
match message {
    .Some(text) => {
        message = nil
        if flag { return }
        print(value: text)
    },
    .None => {}
}
}
)JOYEER");

    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    const auto& run = function("run");
    const auto arm = std::find_if(
            run.blocks.begin(), run.blocks.end(),
            [](const auto& block) { return block.name == "match.arm.0"; });
    ASSERT_NE(arm, run.blocks.end());
    const auto& instructions = arm->instructions;
    const auto payload = std::find_if(
            instructions.begin(), instructions.end(),
            [](const auto& instruction) {
                return instruction.opcode == joyeer::ir::Opcode::extractPayload;
            });
    ASSERT_NE(payload, instructions.end());
    const auto capture = std::find_if(
            payload, instructions.end(),
            [&payload](const auto& instruction) {
                return instruction.opcode == joyeer::ir::Opcode::copyValue &&
                        instruction.operands[0] == payload->result->id;
            });
    ASSERT_NE(capture, instructions.end());
    const auto binding = std::find_if(
            capture, instructions.end(),
            [&capture](const auto& instruction) {
                return instruction.opcode == joyeer::ir::Opcode::store &&
                        instruction.operands[0] == capture->result->id;
            });
    ASSERT_NE(binding, instructions.end());
    const auto mutation = std::find_if(
            instructions.begin(), instructions.end(),
            [](const auto& instruction) {
                return instruction.opcode == joyeer::ir::Opcode::destroy;
            });
    ASSERT_NE(mutation, instructions.end());
    EXPECT_LT(binding, mutation);
    size_t bindingCleanups = 0;
    for (const auto& block : run.blocks) {
        for (const auto& instruction : block.instructions) {
            if (instruction.opcode == joyeer::ir::Opcode::destroy &&
                instruction.operands[0] == binding->operands[1]) {
                ++bindingCleanups;
            }
        }
    }
    EXPECT_EQ(bindingCleanups, 2u);
}

TEST_F(IRLoweringTest, LowersPendingOperandAndCaptureRegressionFixtures) {
    for (const auto* fixture : {
                "e2e/regression_lowering_short_circuit.joyeer",
                "e2e/regression_lowering_capture.joyeer",
                "e2e/regression_lowering_pending_operands.joyeer" }) {
        SCOPED_TRACE(fixture);
        lower(readFixture(fixture));
        ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
        const auto verification = joyeer::ir::Verifier().verify(*result.module);
        EXPECT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    }
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
    EXPECT_EQ(opcodeCount(build, joyeer::ir::Opcode::copyValue), 2u);
    EXPECT_EQ(opcodeCount(build, joyeer::ir::Opcode::destroy), 2u);
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
    EXPECT_EQ(opcodeCount(function("text"), joyeer::ir::Opcode::copyValue), 1u);
    EXPECT_EQ(opcodeCount(function("text"), joyeer::ir::Opcode::destroy), 1u);
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

TEST_F(IRLoweringTest, ConsumeEmptyLowersToOneInstructionOnTheArrayAddress) {
    ASSERT_NO_FATAL_FAILURE(lower("func end() {\n"
                                  "    var numbers: [Int] = []\n"
                                  "    consume numbers.consumeEmpty()\n"
                                  "}\n"));
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    EXPECT_EQ(opcodeCount(function("end"), joyeer::ir::Opcode::arrayConsumeEmpty), 1u);
    EXPECT_TRUE(joyeer::ir::Verifier().verify(*result.module).succeeded());
}

TEST_F(NewOpcodeVerifierTest, ArrayConsumeEmptyNeedsExactlyOneArrayOperandAndNoResult) {
    ASSERT_NO_FATAL_FAILURE(lower("func end() {\n"
                                  "    var numbers: [Int] = []\n"
                                  "    consume numbers.consumeEmpty()\n"
                                  "}\n"));
    EXPECT_TRUE(rejects("end", joyeer::ir::Opcode::arrayConsumeEmpty, [](auto& instruction, auto&) {
        instruction.operands.clear();
    }));
    EXPECT_TRUE(rejects("end", joyeer::ir::Opcode::arrayConsumeEmpty, [](auto& instruction, auto&) {
        instruction.operands.push_back(instruction.operands.at(0));
    }));
}


TEST_F(NewOpcodeVerifierTest, ArrayConsumeEmptyRejectsANonArrayATargetAndAnUnknownOperand) {
    ASSERT_NO_FATAL_FAILURE(lower("func end() {\n"
                                  "    var numbers: [Int] = []\n"
                                  "    consume numbers.consumeEmpty()\n"
                                  "}\n"));
    using joyeer::ir::Opcode;
    EXPECT_TRUE(rejects("end", Opcode::arrayConsumeEmpty, [](auto& instruction, auto& module) {
        const auto intType = typeNamed(module, "Int");
        for (auto& candidate : module.functions) {
            for (auto& block : candidate.blocks) {
                for (auto& other : block.instructions) {
                    if (other.result.has_value() && other.result->id == instruction.operands.at(0)) {
                        other.result->type = intType;
                    }
                }
            }
        }
    }));
    EXPECT_TRUE(rejects("end", Opcode::arrayConsumeEmpty, [](auto& instruction, auto&) {
        instruction.targets.push_back(0);
    }));
    EXPECT_TRUE(rejects("end", Opcode::arrayConsumeEmpty, [](auto& instruction, auto&) {
        instruction.operands.at(0) = 987654;
    }));
}


// Section 21 operations are ordinary source-library calls, including the typed target reference.
TEST_F(IRLoweringTest, SpawnAndJoinLowerToLibraryCalls) {
    ASSERT_NO_FATAL_FAILURE(lower("func square(value: consuming Int): Int { return value * value }\n"
                                  "func run(): Int {\n"
                                  "    let handle = spawn(function: square, input: consume 4)\n"
                                  "    return consume handle.join()\n"
                                  "}\n"));
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    EXPECT_EQ(opcodeCount(function("run"), joyeer::ir::Opcode::call), 2u);
    EXPECT_EQ(opcodeCount(function("run"), joyeer::ir::Opcode::functionAddress), 1u);
    EXPECT_TRUE(joyeer::ir::Verifier().verify(*result.module).succeeded());
}

TEST_F(NewOpcodeVerifierTest, TaskLibraryCallsAreCheckedAgainstOrdinarySignatures) {
    ASSERT_NO_FATAL_FAILURE(lower("func square(value: consuming Int): Int { return value * value }\n"
                                  "func run(): Int {\n"
                                  "    let handle = spawn(function: square, input: consume 4)\n"
                                  "    return consume handle.join()\n"
                                  "}\n"));
    using joyeer::ir::Opcode;
    EXPECT_TRUE(rejects("run", Opcode::call, [](auto& instruction, auto&) {
        instruction.operands.clear();
    }));
    EXPECT_TRUE(rejects("run", Opcode::call, [](auto& instruction, auto&) {
        instruction.callee = 987654;
    }));
    EXPECT_TRUE(rejects("run", Opcode::call, [](auto& instruction, auto& module) {
        instruction.result->type = typeNamed(module, "Int");
    }));
    EXPECT_TRUE(rejects("run", Opcode::functionAddress, [](auto& instruction, auto& module) {
        module.functions[*instruction.callee].parameters[0].isConsuming = false;
    }));
}


TEST_F(NewOpcodeVerifierTest, TaskReferencesRejectMalformedTargetsOrResults) {
    ASSERT_NO_FATAL_FAILURE(lower("func square(value: consuming Int): Int { return value * value }\n"
                                  "func run(): Int {\n"
                                  "    let handle = spawn(function: square, input: consume 4)\n"
                                  "    return consume handle.join()\n"
                                  "}\n"));
    using joyeer::ir::Opcode;
    using joyeer::ir::ValueCategory;
    EXPECT_TRUE(rejects("run", Opcode::call, [](auto& instruction, auto&) {
        instruction.result->category = ValueCategory::address;
    }));
    EXPECT_TRUE(rejects("run", Opcode::functionAddress, [](auto& instruction, auto& module) {
        module.functions[*instruction.callee].isExternal = true;
    }));
    EXPECT_TRUE(rejects("run", Opcode::functionAddress, [](auto& instruction, auto& module) {
        module.functions[*instruction.callee].parameters.clear();
    }));
    EXPECT_TRUE(rejects("run", Opcode::functionAddress, [](auto& instruction, auto& module) {
        module.functions[*instruction.callee].resultType = typeNamed(module, "String");
    }));
    EXPECT_TRUE(rejects("run", Opcode::call, [](auto& instruction, auto&) {
        instruction.result.reset();
    }));
}


TEST_F(IRLoweringTest, ChannelOperationsLowerToLibraryCalls) {
    ASSERT_NO_FATAL_FAILURE(lower("func run(): Int {\n"
                                  "    var sender: ChannelSender<Int>\n"
                                  "    var receiver: ChannelReceiver<Int>\n"
                                  "    channel(capacity: 2, sender: &sender, receiver: &receiver)\n"
                                  "    let sent = sender.send(value: consume 4)\n"
                                  "    let got = receiver.receive()\n"
                                  "    consume sender.close()\n"
                                  "    return 0\n"
                                  "}\n"));
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    using joyeer::ir::Opcode;
    EXPECT_EQ(opcodeCount(function("run"), Opcode::call), 4u);
    EXPECT_TRUE(joyeer::ir::Verifier().verify(*result.module).succeeded());
}

TEST_F(NewOpcodeVerifierTest, ChannelLibraryCallsAreCheckedAgainstTheirEnds) {
    ASSERT_NO_FATAL_FAILURE(lower("func run(): Int {\n"
                                  "    var sender: ChannelSender<Int>\n"
                                  "    var receiver: ChannelReceiver<Int>\n"
                                  "    channel(capacity: 2, sender: &sender, receiver: &receiver)\n"
                                  "    let sent = sender.send(value: consume 4)\n"
                                  "    let got = receiver.receive()\n"
                                  "    consume sender.close()\n"
                                  "    return 0\n"
                                  "}\n"));
    using joyeer::ir::Opcode;
    using joyeer::ir::ValueCategory;
    EXPECT_TRUE(rejects("run", Opcode::call, [](auto& instruction, auto&) {
        instruction.operands.pop_back();
    }));
    EXPECT_TRUE(rejects("run", Opcode::call, [](auto& instruction, auto&) {
        std::swap(instruction.operands[1], instruction.operands[2]);
    }));
}

// Function values (decision 0020): a function named as a value lowers to one function_address, and a
// call through a value to one call_indirect whose operands follow the type's effects.
constexpr const char* functionValueProgram =
        "func square(value: Int): Int { return value * value }\n"
        "func bump(count: inout Int) { &count = count + 1 }\n"
        "func stop(code: Int): Never { return stop(code: code) }\n"
        "func run(): Int {\n"
        "    let f = square\n"
        "    let g = bump\n"
        "    var n = f(3)\n"
        "    g(&n)\n"
        "    return n\n"
        "}\n"
        "func halt(): Int {\n"
        "    let s = stop\n"
        "    s(1)\n"
        "}\n";

TEST_F(IRLoweringTest, FunctionValuesLowerToAnAddressAndAnIndirectCall) {
    ASSERT_NO_FATAL_FAILURE(lower(functionValueProgram));
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    EXPECT_EQ(opcodeCount(function("run"), joyeer::ir::Opcode::functionAddress), 2u);
    EXPECT_EQ(opcodeCount(function("run"), joyeer::ir::Opcode::callIndirect), 2u);
    EXPECT_EQ(opcodeCount(function("run"), joyeer::ir::Opcode::call), 0u);
    // A call through a Never value ends its block.
    EXPECT_EQ(opcodeCount(function("halt"), joyeer::ir::Opcode::callIndirect), 1u);
    EXPECT_GE(opcodeCount(function("halt"), joyeer::ir::Opcode::unreachable), 1u);
    const auto verification = joyeer::ir::Verifier().verify(*result.module);
    EXPECT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
}

TEST_F(NewOpcodeVerifierTest, FunctionAddressAndIndirectCallsAreCheckedAgainstTheFunctionType) {
    ASSERT_NO_FATAL_FAILURE(lower(functionValueProgram));
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    using joyeer::ir::Opcode;
    // function_address: an unknown callee, an operand, a result of another type.
    EXPECT_TRUE(rejects("run", Opcode::functionAddress, [](auto& instruction, auto&) {
        instruction.callee = 987654;
    }));
    EXPECT_TRUE(rejects("run", Opcode::functionAddress, [](auto& instruction, auto&) {
        instruction.operands.push_back(0);
    }));
    EXPECT_TRUE(rejects("run", Opcode::functionAddress, [](auto& instruction, auto& module) {
        instruction.result->type = typeNamed(module, "Int");
    }));
    EXPECT_TRUE(rejects("run", Opcode::functionAddress, [](auto& instruction, auto& module) {
        instruction.result->type = typeNamed(module, "(inout Int): ()");
    }));
    // call_indirect: a callee that is not a function value, a wrong count, a wrong category, a result.
    EXPECT_TRUE(rejectsWith("run", Opcode::callIndirect, [](auto& instruction, auto&) {
        instruction.operands.erase(instruction.operands.begin());
    }, "call_indirect"));
    EXPECT_TRUE(rejectsWith("run", Opcode::callIndirect, [](auto& instruction, auto&) {
        instruction.operands.push_back(instruction.operands.back());
    }, "call_indirect"));
    EXPECT_TRUE(rejectsWith("run", Opcode::callIndirect, [](auto& instruction, auto&) {
        instruction.result.reset();
    }, "call_indirect"));
    EXPECT_TRUE(rejectsWith("run", Opcode::callIndirect, [](auto& instruction, auto&) {
        instruction.callee = 0;
    }, "call_indirect"));
    // A Never call must be followed by unreachable.
    EXPECT_TRUE(rejectsWith("halt", Opcode::callIndirect, [](auto& instruction, auto& module) {
        for (auto& candidate : module.functions) {
            if (candidate.name != "halt") continue;
            for (auto& block : candidate.blocks) {
                for (size_t index = 0; index + 1 < block.instructions.size(); ++index) {
                    if (block.instructions[index].opcode == Opcode::callIndirect) {
                        block.instructions[index + 1].opcode = Opcode::returnVoid;
                    }
                }
            }
        }
    }, "unreachable"));
}

// Malformed wire input must be rejected, not crash; and the native-binding, Never and entry-point rules
// hold for function values as for direct calls (review of decision 0020).
TEST_F(NewOpcodeVerifierTest, FunctionValueRulesRejectMalformedModules) {
    ASSERT_NO_FATAL_FAILURE(lower(functionValueProgram));
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    using joyeer::ir::Opcode;
    EXPECT_TRUE(rejects("run", Opcode::functionAddress, [](auto& instruction, auto&) {
        instruction.result.reset();
    }));
    EXPECT_TRUE(rejects("run", Opcode::functionAddress, [](auto& instruction, auto&) {
        instruction.result->category = joyeer::ir::ValueCategory::address;
    }));
    // A C binding's address needs a runtime unit, like a direct call to it.
    EXPECT_TRUE(rejectsWith("run", Opcode::functionAddress, [](auto& instruction, auto& module) {
        for (auto& candidate : module.functions) {
            if (candidate.id != *instruction.callee) continue;
            candidate.nativeBinding = joyeer::NativeBinding { joyeer::NativeBindingKind::cExport, "exported" };
        }
    }, "runtime authorization"));
    // The root `main` has no address.
    EXPECT_TRUE(rejects("run", Opcode::functionAddress, [](auto& instruction, auto& module) {
        for (auto& candidate : module.functions) {
            if (candidate.id == *instruction.callee) candidate.name = "main";
        }
    }));
    // call_indirect takes no block targets.
    EXPECT_TRUE(rejectsWith("run", Opcode::callIndirect, [](auto& instruction, auto&) {
        instruction.targets.push_back(0);
    }, "call_indirect"));
    // A direct call returning Never must be followed by unreachable too.
    EXPECT_TRUE(rejectsWith("stop", Opcode::call, [](auto&, auto& module) {
        for (auto& candidate : module.functions) {
            if (candidate.name != "stop") continue;
            for (auto& block : candidate.blocks) {
                for (size_t index = 0; index + 1 < block.instructions.size(); ++index) {
                    if (block.instructions[index].opcode == Opcode::call) {
                        block.instructions[index + 1].opcode = Opcode::returnVoid;
                    }
                }
            }
        }
    }, "unreachable"));
}

TEST_F(NewOpcodeVerifierTest, FunctionTypesNeedOneValidEffectPerParameter) {
    ASSERT_NO_FATAL_FAILURE(lower(functionValueProgram));
    ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
    auto module = *result.module;
    auto found = std::find_if(module.types.begin(), module.types.end(),
            [](const auto& type) { return type.kind == joyeer::typing::TypeKind::function; });
    ASSERT_NE(found, module.types.end());
    auto missing = module;
    missing.types[found - module.types.begin()].effects.clear();
    EXPECT_FALSE(joyeer::ir::Verifier().verify(missing).succeeded());
    auto outOfRange = module;
    outOfRange.types[found - module.types.begin()].effects.assign(
            found->effects.size(), static_cast<joyeer::syntax::AccessEffect>(99));
    EXPECT_FALSE(joyeer::ir::Verifier().verify(outOfRange).succeeded());
    // `Never` is not a parameter type.
    auto neverParameter = module;
    const auto never = std::find_if(module.types.begin(), module.types.end(),
            [](const auto& type) { return type.kind == joyeer::typing::TypeKind::never; });
    ASSERT_NE(never, module.types.end());
    auto& changed = neverParameter.types[found - module.types.begin()];
    ASSERT_GE(changed.arguments.size(), 2u);
    changed.arguments[0] = never->id;
    EXPECT_FALSE(joyeer::ir::Verifier().verify(neverParameter).succeeded());
    // Effects belong only to function types.
    auto strayEffects = module;
    const auto integer = std::find_if(strayEffects.types.begin(), strayEffects.types.end(),
            [](const auto& type) { return type.kind == joyeer::typing::TypeKind::integer; });
    ASSERT_NE(integer, strayEffects.types.end());
    integer->effects.push_back(joyeer::syntax::AccessEffect::inout);
    EXPECT_FALSE(joyeer::ir::Verifier().verify(strayEffects).succeeded());
}
} // namespace
