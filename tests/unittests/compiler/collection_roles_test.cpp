// Decision 0025: the compiler capabilities a library-written array needs. Every test here is written
// before the implementation and fails until the capability exists. The library under test is a small
// `Buffer<T>` bound by the role `array_type` and supplied through the role-prelude test seam of
// decision 0024 (the bundled prelude has no such declaration until Phase 3 of plan 0011).
#include "compiler/backend/ir_wire.h"
#include "compiler/codegen/llvm.h"
#include "compiler/frontend/lexparser.h"
#include "compiler/frontend/nameresolution.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/semanticanalysis.h"
#include "compiler/frontend/typechecking.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/irlowering.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <regex>
#include <set>
#include <string>

namespace {

constexpr auto roleEnums = R"(
@compiler_role(optional_type)
public enum Optional<T> { Some(T), None, }
@compiler_role(result_type)
public enum Result<T, E> { Ok(T), Err(E), }
)";

// A well-shaped array role declaration; each malformed test replaces one line of it.
constexpr auto bufferHead = R"(
@compiler_role(array_type)
public struct Buffer<T> {
    private var address: Int
    private var count: Int
    private var capacity: Int
)";
constexpr auto bufferElement = R"(
    @compiler_role(array_element)
    public borrowing func element(index: Int): Int {
        return self.address + index * joyeer_intrinsic_size_of<T>()
    }
)";
constexpr auto bufferCopy = R"(
    @compiler_role(array_copy)
    public borrowing func duplicate(): Buffer<T> {
        return joyeer_intrinsic_move_out<Buffer<T>>(address: self.address)
    }
)";
constexpr auto bufferEnd = R"(
    @compiler_role(container_end)
    public consuming func finish() {}
)";
constexpr auto bufferTail = R"(
    deinit() {
        joyeer_intrinsic_destroy_range<T>(address: self.address, count: self.count)
    }
}
)";

std::string buffer(const std::string& head = bufferHead, const std::string& element = bufferElement,
                   const std::string& copy = bufferCopy, const std::string& end = bufferEnd,
                   const std::string& tail = bufferTail) {
    return head + element + copy + end + tail;
}

class CollectionRoleTest : public testing::Test {
protected:
    // Resolves and type checks `user` against a role prelude made of the two enums and `library`.
    void load(const std::string& user, const std::string& library = buffer()) {
        Diagnostics lexerDiagnostics;
        LexParser lexer(&lexerDiagnostics);
        userSource = std::make_shared<SourceFile>(user);
        lexer.parse(userSource);
        ASSERT_TRUE(lexerDiagnostics.errors.empty());
        const auto userParse = joyeer::parser::Parser(userSource->tokens, true).parse();
        ASSERT_TRUE(userParse.succeeded()) << joyeer::parser::dump(userParse.diagnostics);

        roleSource = std::make_shared<SourceFile>(std::string(roleEnums) + library);
        lexer.parse(roleSource);
        ASSERT_TRUE(lexerDiagnostics.errors.empty());
        for (auto& token : roleSource->tokens) token->span.sourceId = 1;
        const auto roleParse = joyeer::parser::Parser(roleSource->tokens, true, true).parse();
        ASSERT_TRUE(roleParse.succeeded()) << joyeer::parser::dump(roleParse.diagnostics);

        resolution = joyeer::semantic::NameResolver().resolve({
                { "app", { userParse.root } },
                { "$role-prelude", { roleParse.root }, false, false, true, true } });
        checking = {};
        if (resolution.succeeded()) checking = joyeer::typing::TypeChecker().check(resolution.model);
    }

    bool checks() const { return resolution.succeeded() && checking.succeeded(); }

    std::string diagnostics() const {
        return joyeer::semantic::dump(resolution.diagnostics) + joyeer::typing::dump(checking.diagnostics);
    }

    bool rejectedAsInvalidPrelude(const std::string& library, const std::string& needle) {
        load("func main() {}\n", library);
        if (!resolution.succeeded()) return false;
        return std::any_of(checking.diagnostics.begin(), checking.diagnostics.end(), [&](const auto& diagnostic) {
            return diagnostic.id == joyeer::typing::TypeCheckingDiagnosticId::invalidPrelude &&
                   diagnostic.message.find(needle) != std::string::npos;
        });
    }

    // The program is rejected, with a diagnostic from any stage that mentions `needle`. The library
    // itself must load: a broken role declaration would otherwise satisfy every negative test.
    bool rejectedMentioning(const std::string& user, const std::string& needle) {
        load(user);
        if (diagnostics().find("invalid-prelude") != std::string::npos) return false;
        return diagnostics().find(needle) != std::string::npos;
    }

    bool hasTypeDiagnostic(joyeer::typing::TypeCheckingDiagnosticId id) const {
        return std::any_of(checking.diagnostics.begin(), checking.diagnostics.end(),
                           [id](const auto& diagnostic) { return diagnostic.id == id; });
    }

    // Runs semantic analysis on a program that type checked; returns its dumped diagnostics.
    std::string analyze() {
        EXPECT_TRUE(checks()) << diagnostics();
        if (checking.model == nullptr) return "no model";
        analysis = joyeer::analysis::Analyzer().analyze(checking.model);
        return joyeer::analysis::dump(analysis.diagnostics);
    }

    std::shared_ptr<joyeer::ir::Module> lower() {
        EXPECT_TRUE(checks()) << diagnostics();
        if (checking.model == nullptr) return nullptr;
        analysis = joyeer::analysis::Analyzer().analyze(checking.model);
        EXPECT_TRUE(analysis.succeeded()) << joyeer::analysis::dump(analysis.diagnostics);
        // The driver's source table: the user source, then the prelude sources by identity.
        const auto info = [](const SourceFile::Ptr& file) {
            return joyeer::ir::SourceInfo { "fixture.joyeer", ".", file->content.size(), file->lineStarts };
        };
        std::vector<joyeer::ir::SourceInfo> sources { info(userSource), info(roleSource) };
        for (const auto& prelude : checking.model->semanticModel()->preludeSources()) sources.push_back(info(prelude));
        auto result = joyeer::lowering::Lowerer().lower(
                checking.model, "fixture.joyeer", info(userSource), sources);
        EXPECT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
        return result.module;
    }

    SourceFile::Ptr userSource;
    SourceFile::Ptr roleSource;
    joyeer::semantic::NameResolutionResult resolution;
    joyeer::typing::TypeCheckingResult checking;
    joyeer::analysis::Result analysis;
};

// ---- Roles: shape validation ----

TEST_F(CollectionRoleTest, AWellShapedArrayRoleIsAccepted) {
    load("func main() {}\n");
    ASSERT_TRUE(resolution.succeeded()) << diagnostics();
    EXPECT_TRUE(checking.succeeded()) << diagnostics();
}

TEST_F(CollectionRoleTest, EachMalformedTypeShapeIsRejected) {
    // The fields must be private `address`, `count`, `capacity` of type Int, in that order.
    const auto element = std::string(bufferElement);
    const auto copy = std::string(bufferCopy);
    const auto end = std::string(bufferEnd);
    const auto tail = std::string(bufferTail);
    const auto withFields = [&](const std::string& fields) {
        return buffer("\n@compiler_role(array_type)\npublic struct Buffer<T> {\n" + fields, element, copy, end, tail);
    };
    EXPECT_TRUE(rejectedAsInvalidPrelude(withFields(
            "private var count: Int\nprivate var address: Int\nprivate var capacity: Int\n"), "array_type"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(withFields(
            "private var address: Int\nprivate var count: Int\n"), "array_type"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(withFields(
            "private var address: Int\nprivate var count: Int\nprivate var capacity: Int\nprivate var extra: Int\n"),
            "array_type"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(withFields(
            "private var address: Int\nprivate var count: String\nprivate var capacity: Int\n"), "array_type"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(withFields(
            "public var address: Int\nprivate var count: Int\nprivate var capacity: Int\n"), "array_type"));
    // Exactly one type parameter, with no constraints.
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            buffer("\n@compiler_role(array_type)\npublic struct Buffer<T, U> {\n"
                   "private var address: Int\nprivate var count: Int\nprivate var capacity: Int\n"),
            "array_type"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            buffer("\n@compiler_role(array_type)\npublic struct Buffer<T: Copyable> {\n"
                   "private var address: Int\nprivate var count: Int\nprivate var capacity: Int\n"),
            "array_type"));
    // The role belongs on a struct, not an enum or a function.
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            std::string(bufferHead) + element + copy + end + tail +
            "\n@compiler_role(array_type)\npublic func wrong(): Int { return 0 }\n", "array_type"));
}

TEST_F(CollectionRoleTest, EachMalformedMethodRoleIsRejected) {
    const auto head = std::string(bufferHead);
    const auto element = std::string(bufferElement);
    const auto copy = std::string(bufferCopy);
    const auto end = std::string(bufferEnd);
    const auto replaceAll = [](std::string text, const std::string& from, const std::string& to) {
        const auto at = text.find(from);
        EXPECT_NE(at, std::string::npos) << from;
        if (at != std::string::npos) text.replace(at, from.size(), to);
        return text;
    };
    // array_element: borrowing, one parameter named `index` with no separate label, result Int.
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            buffer(head, replaceAll(element, "borrowing func element(index: Int)", "consuming func element(index: Int)")),
            "array_element"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            buffer(head, replaceAll(element, "element(index: Int)", "element(at index: Int)")), "array_element"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            buffer(head, replaceAll(element, "(index: Int): Int", "(index: Int): String")), "array_element"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            buffer(head, replaceAll(element, "(index: Int)", "(index: Int, extra: Int)")), "array_element"));
    // array_copy: borrowing, no parameters, returns the declaring type.
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            buffer(head, element, replaceAll(copy, "duplicate(): Buffer<T>", "duplicate(): Int")), "array_copy"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            buffer(head, element, replaceAll(copy, "borrowing func", "consuming func")), "array_copy"));
    // container_end: consuming, no parameters, Void result.
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            buffer(head, element, copy, replaceAll(end, "consuming func finish()", "borrowing func finish()")),
            "container_end"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            buffer(head, element, copy, replaceAll(end, "finish() {}", "finish(extra: Int) {}")), "container_end"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            buffer(head, element, copy, replaceAll(end, "finish() {}", "finish(): Int { return 0 }")),
            "container_end"));
    // A method role on a free function.
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            buffer() + "\n@compiler_role(array_element)\npublic func loose(index: Int): Int { return index }\n",
            "array_element"));
}

TEST_F(CollectionRoleTest, ADuplicateRoleAndAnExtensionAreRejected) {
    // A second declaration of the type role, and a second method with the same role.
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            buffer() + "\n@compiler_role(array_type)\npublic struct Other<T> {\n"
            "private var address: Int\nprivate var count: Int\nprivate var capacity: Int\n}\n",
            "second"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            buffer(bufferHead, std::string(bufferElement) + R"(
    @compiler_role(array_element)
    public borrowing func again(index: Int): Int { return index }
)"), "array_element"));
    // The role type's methods live in its body; an extension is not allowed.
    EXPECT_TRUE(rejectedAsInvalidPrelude(buffer() + "extension Buffer<U> { }\n", "may not extend"));
}

TEST_F(CollectionRoleTest, IntrinsicsAndRolesAreRejectedOutsideTheCompilerPrelude) {
    for (const auto& call : {
            "joyeer_intrinsic_clone_at<Int>(destination: 0, source: 0)",
            "joyeer_intrinsic_destroy_range<Int>(address: 0, count: 0)",
            "let callback = joyeer_intrinsic_clone_callback<Int>()",
            "let callback = joyeer_intrinsic_destroy_callback<Int>()" }) {
        SCOPED_TRACE(call);
        load(std::string("func main() {\n    ") + call + "\n}\n");
        ASSERT_TRUE(resolution.succeeded()) << diagnostics();
        EXPECT_TRUE(hasTypeDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::unauthorizedIntrinsic))
                << diagnostics();
    }
}

// ---- Access: `a[i]` types as the owned type, and `count` is read-only ----

TEST_F(CollectionRoleTest, AnElementTypesAsTheFirstTypeArgument) {
    load("func first(items: Buffer<String>): String { return items[0] }\n"
         "func total(items: Buffer<Int>): Int { return items[0] + items[1] }\n"
         "func nested(items: Buffer<Buffer<Int>>): Int { return items[0][1] }\n"
         "func set(items: inout Buffer<Int>) { &items[0] = 4\n &items[1] += 2 }\n");
    EXPECT_TRUE(checks()) << diagnostics();
    EXPECT_TRUE(rejectedMentioning("func wrong(items: Buffer<String>): Int { return items[0] }\n", "String"))
            << diagnostics();
    EXPECT_TRUE(rejectedMentioning("func wrong(items: Buffer<Int>): Int { return items[\"zero\"] }\n", "Int"))
            << diagnostics();
}

TEST_F(CollectionRoleTest, CountIsReadableButNotWritableOutsideTheDeclaration) {
    load("func size(items: Buffer<Int>): Int { return items.count }\n"
         "func borrow(items: Buffer<Int>) { show(value: items.count) }\n"
         "func show(value: Int) {}\n");
    EXPECT_TRUE(checks()) << diagnostics();
    EXPECT_TRUE(rejectedMentioning("func bad(items: inout Buffer<Int>) { &items.count = 3 }\n", "count"))
            << diagnostics();
    EXPECT_TRUE(rejectedMentioning("func bad(items: inout Buffer<Int>) { &items.count += 1 }\n", "count"))
            << diagnostics();
    EXPECT_TRUE(rejectedMentioning(
            "func bad(items: inout Buffer<Int>) { touch(value: &items.count) }\n"
            "func touch(value: inout Int) {}\n", "mutable storage")) << diagnostics();
    // The other two fields stay private.
    EXPECT_TRUE(rejectedMentioning("func bad(items: Buffer<Int>): Int { return items.address }\n", "address"))
            << diagnostics();
    EXPECT_TRUE(rejectedMentioning("func bad(items: Buffer<Int>): Int { return items.capacity }\n", "capacity"))
            << diagnostics();
}

TEST_F(CollectionRoleTest, SubscriptsKeepTheExclusivityRulesOfAnArray) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    // Two held projections that differ only by index conflict, as for a builtin array.
    load("func swap(first: inout Int, second: inout Int) {}\n"
         "func bad(items: inout Buffer<Int>) { swap(first: &items[0], second: &items[1]) }\n");
    EXPECT_TRUE(hasTypeDiagnostic(Id::overlappingAccess)) << diagnostics();
    // An assignment evaluates its right-hand side before the access starts.
    load("func read(items: Buffer<Int>): Int { return items[0] }\n"
         "func ok(items: inout Buffer<Int>) { &items[0] = items[1] }\n");
    EXPECT_TRUE(checks()) << diagnostics();
}

// ---- Ownership: the role owns its first type parameter ----

TEST_F(CollectionRoleTest, CopyabilityFollowsTheOwnedTypeAlthoughDeinitExists) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    load("func clone(items: Buffer<Int>): Buffer<Int> { let again = items\n return again }\n"
         "func cloneNested(items: Buffer<Buffer<String>>): Buffer<Buffer<String>> { return items }\n");
    EXPECT_TRUE(checks()) << diagnostics();
    // A noncopyable owned type makes the container noncopyable, and a linear one makes it linear.
    load("struct Resource { var value: Int\n deinit() {} }\n"
         "func bad(items: Buffer<Resource>): Buffer<Resource> { let again = items\n return again }\n");
    EXPECT_TRUE(hasTypeDiagnostic(Id::copyOfNoncopyable)) << diagnostics();
    load("func bad(items: Buffer<JoinHandle<Int>>): Buffer<JoinHandle<Int>> { let again = items\n return again }\n");
    EXPECT_TRUE(hasTypeDiagnostic(Id::copyOfNoncopyable)) << diagnostics();
}

TEST_F(CollectionRoleTest, ANoncopyableOwnerStillWorksWhereACopyIsNotNeeded) {
    load("struct Resource { var value: Int\n deinit() {} }\n"
         "func use(items: borrowing Buffer<Resource>): Int { return items.count }\n"
         "func pass(items: consuming Buffer<Resource>): Int { return use(items: items) }\n");
    EXPECT_TRUE(checks()) << diagnostics();
}

TEST_F(CollectionRoleTest, LinearElementsAreAllowedInTheRoleAndStillRejectedInUserGenerics) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    load("func work(value: consuming Int): Int { return value }\n"
         "func hold(items: borrowing Buffer<JoinHandle<Int>>): Int { return items.count }\n");
    EXPECT_TRUE(checks()) << diagnostics();
    // A user generic still may not take a linear argument, directly or inside the role type.
    load("struct Box<T> { var value: Int }\n"
         "func keep<T>(value: T) {}\n"
         "func bad(items: consuming Buffer<JoinHandle<Int>>) { keep(value: items) }\n");
    EXPECT_TRUE(hasTypeDiagnostic(Id::linearTypeArgument)) << diagnostics();
}

// ---- Semantic analysis: ending a linear container ----

TEST_F(CollectionRoleTest, ALinearContainerMustBeEndedOnEveryPath) {
    // The container's own ending method is the way to end it.
    load("func end(items: consuming Buffer<JoinHandle<Int>>) { consume items.finish() }\n");
    EXPECT_EQ(analyze(), "") << "consume items.finish() ends a linear container";
    load("func drop(items: consuming Buffer<JoinHandle<Int>>) {}\n");
    EXPECT_NE(analyze().find("linear-not-consumed"), std::string::npos)
            << "a linear container may not be dropped implicitly";
    load("func maybe(items: consuming Buffer<JoinHandle<Int>>, flag: Bool) {\n"
         "    if flag { consume items.finish() }\n}\n");
    EXPECT_NE(analyze().find("linear-not-consumed"), std::string::npos);
    // The same code on a container of a plain type is an ordinary drop.
    load("func drop(items: consuming Buffer<Int>) {}\n");
    EXPECT_EQ(analyze(), "");
}

TEST_F(CollectionRoleTest, OnlyTheEndingMethodMayLeaveItsSelfUnconsumed) {
    // `container_end` exempts `self`; another consuming method of the declaration does not get it.
    const auto extra = std::string(R"(
    public consuming func leak() {}
)");
    load("func main() {}\n", buffer(bufferHead, bufferElement, bufferCopy, std::string(bufferEnd) + extra));
    EXPECT_NE(analyze().find("linear-not-consumed"), std::string::npos)
            << "a second consuming method must still consume self";
    load("func main() {}\n");
    EXPECT_EQ(analyze(), "") << "the container_end method itself is exempt";
}

TEST_F(CollectionRoleTest, ConsumingALinearElementIsRejectedAtTheConsumeSite) {
    load("func work(value: consuming Int): Int { return value }\n"
         "func bad(items: consuming Buffer<JoinHandle<Int>>) {\n"
         "    let handle = consume items[0]\n"
         "    consume items.finish()\n"
         "    consume handle.join()\n"
         "}\n");
    EXPECT_NE(analyze().find("consume-of-linear-element"), std::string::npos);
    // The same rule for the builtin array: today the failure shows up later, on the array itself.
    load("func bad(items: consuming [JoinHandle<Int>]) {\n"
         "    let handle = consume items[0]\n"
         "    consume items.consumeEmpty()\n"
         "    consume handle.join()\n"
         "}\n",
         buffer());
    EXPECT_NE(analyze().find("consume-of-linear-element"), std::string::npos);
    // Consuming a copyable or merely noncopyable element is unchanged.
    load("struct Resource { var value: Int\n deinit() {} }\n"
         "func use(value: consuming Resource): Int { return value.value }\n"
         "func ok(items: consuming Buffer<Resource>): Int { return use(value: consume items[0]) }\n");
    EXPECT_EQ(analyze(), "") << "a noncopyable element may be consumed";
}

// ---- IR: layout, roles and the wire ----

const joyeer::ir::StructureDefinition* bufferDefinition(const joyeer::ir::Module& module) {
    for (const auto& definition : module.structures) {
        if (definition.name.find("Buffer") != std::string::npos) return &definition;
    }
    return nullptr;
}

TEST_F(CollectionRoleTest, TheRoleStructKeepsTheThreeWordLayoutWithoutALiveFlag) {
    load("func size(items: Buffer<Int>): Int { return items.count }\n");
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    const auto* definition = bufferDefinition(*module);
    ASSERT_NE(definition, nullptr);
    ASSERT_EQ(definition->fields.size(), 3u) << "no hidden $live field";
    EXPECT_EQ(definition->fields[0].name, "address");
    EXPECT_EQ(definition->fields[1].name, "count");
    EXPECT_EQ(definition->fields[2].name, "capacity");
    EXPECT_TRUE(definition->deinitializer.has_value()) << "the library deinit still runs";
    for (const auto& field : definition->fields) EXPECT_NE(field.name, joyeer::ir::liveFieldName);
}

TEST_F(CollectionRoleTest, TheWireCarriesTheCollectionRolesAndAdvancesItsVersion) {
    EXPECT_EQ(joyeer::llvmbackend::wire::version, 21u);
    load("func first(items: Buffer<Int>): Int { return items[0] }\n");
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    joyeer::llvmbackend::wire::Writer writer;
    writer(*module);
    joyeer::ir::Module decoded;
    joyeer::llvmbackend::wire::Reader reader(writer.bytes);
    reader(decoded);
    ASSERT_TRUE(reader.succeeded());
    const auto* before = bufferDefinition(*module);
    const auto* after = bufferDefinition(decoded);
    ASSERT_NE(before, nullptr);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->fields.size(), before->fields.size());
    EXPECT_EQ(after->deinitializer.has_value(), before->deinitializer.has_value());
    EXPECT_TRUE(joyeer::ir::Verifier().verify(decoded).succeeded());
}

TEST_F(CollectionRoleTest, ElementAccessLowersToTheAddressorAndLeavesNoIntegerAddressEscape) {
    load("func first(items: Buffer<Int>): Int { return items[0] }\n"
         "func set(items: inout Buffer<Int>) { &items[0] = 4 }\n");
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    const auto verification = joyeer::ir::Verifier().verify(*module);
    EXPECT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    const auto dumped = joyeer::ir::dump(*module);
    // The addressor is a real function of the module, called from each access.
    EXPECT_NE(dumped.find("element"), std::string::npos) << dumped;
    EXPECT_NE(dumped.find("integer_address"), std::string::npos) << dumped;
}

// ---- IR: malformed forms of each new rule (decision 0025) ----

std::string verifierMessages(const joyeer::ir::Module& module) {
    return joyeer::ir::dump(joyeer::ir::Verifier().verify(module));
}

joyeer::ir::Function& named(joyeer::ir::Module& module, const std::string& name) {
    const auto found = std::find_if(module.functions.begin(), module.functions.end(),
                                    [&](const auto& function) { return function.name == name; });
    EXPECT_NE(found, module.functions.end()) << name;
    return module.functions.at(static_cast<size_t>(std::distance(module.functions.begin(), found)));
}

// Inserts `added` right after the first integer constant of `function`, in that constant's own block, with
// `operands` copies of the constant as operands and, when `yieldsInt`, a fresh Int result.
bool insertAfterIntConstant(joyeer::ir::Function& function, joyeer::ir::Instruction added, unsigned operands,
                            bool yieldsInt) {
    for (auto& block : function.blocks) {
        for (auto it = block.instructions.begin(); it != block.instructions.end(); ++it) {
            if (it->opcode != joyeer::ir::Opcode::integerConstant || !it->result.has_value()) continue;
            const auto constant = *it->result;
            if (added.integerValue == 0) added.integerValue = static_cast<int64_t>(constant.type);
            for (unsigned index = 0; index < operands; ++index) added.operands.push_back(constant.id);
            if (yieldsInt) added.result = joyeer::ir::Value { 9000, constant.type, joyeer::ir::ValueCategory::value };
            block.instructions.insert(it + 1, std::move(added));
            return true;
        }
    }
    return false;
}

TEST_F(CollectionRoleTest, TheVerifierAcceptsTheLoweredModuleAndRejectsEachMalformedForm) {
    using joyeer::ir::Opcode;
    load("func first(items: Buffer<Int>): Int { return items[0] }\n");
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(verifierMessages(*module).find("ir."), std::string::npos) << verifierMessages(*module);
    const auto find = [](joyeer::ir::Function& function, Opcode opcode) -> joyeer::ir::Instruction* {
        for (auto& block : function.blocks) {
            for (auto& instruction : block.instructions) {
                if (instruction.opcode == opcode) return &instruction;
            }
        }
        return nullptr;
    };
    // An element place whose operand does not come from an addressor is a user-code integer address.
    {
        auto copy = std::make_shared<joyeer::ir::Module>(*module);
        auto& function = named(*copy, "first");
        auto* address = find(function, Opcode::integerAddress);
        ASSERT_NE(address, nullptr);
        auto* constant = find(function, Opcode::integerConstant);
        ASSERT_NE(constant, nullptr);
        address->operands = { constant->result->id };
        EXPECT_NE(verifierMessages(*copy).find("runtime authorization"), std::string::npos)
                << verifierMessages(*copy);
    }
    // An inert-zero struct must not end with a live flag, and a copier must borrow and return the struct.
    {
        auto copy = std::make_shared<joyeer::ir::Module>(*module);
        auto* definition = const_cast<joyeer::ir::StructureDefinition*>(bufferDefinition(*copy));
        ASSERT_NE(definition, nullptr);
        ASSERT_TRUE(definition->inertZero);
        auto flagged = *definition;
        const auto boolType = std::find_if(copy->types.begin(), copy->types.end(),
                                           [](const auto& type) { return type.name == "Bool"; });
        ASSERT_NE(boolType, copy->types.end());
        flagged.fields.push_back({ joyeer::semantic::invalidSymbolId, joyeer::ir::liveFieldName,
                                   boolType->id, false });
        *definition = flagged;
        EXPECT_NE(verifierMessages(*copy).find("live flag"), std::string::npos) << verifierMessages(*copy);
    }
    {
        auto copy = std::make_shared<joyeer::ir::Module>(*module);
        auto* definition = const_cast<joyeer::ir::StructureDefinition*>(bufferDefinition(*copy));
        ASSERT_NE(definition, nullptr);
        definition->copier = definition->deinitializer;  // the deinit returns nothing and takes an address
        EXPECT_NE(verifierMessages(*copy).find("copier"), std::string::npos) << verifierMessages(*copy);
    }
    {
        auto copy = std::make_shared<joyeer::ir::Module>(*module);
        auto* definition = const_cast<joyeer::ir::StructureDefinition*>(bufferDefinition(*copy));
        ASSERT_NE(definition, nullptr);
        definition->deinitializer.reset();
        EXPECT_NE(verifierMessages(*copy).find("needs a deinit"), std::string::npos) << verifierMessages(*copy);
    }
    // Each new opcode is runtime-only: a user function that carries one is rejected.
    for (const auto opcode : { Opcode::destroyRange, Opcode::cloneAt, Opcode::linearRangeGuard,
                               Opcode::cloneCallback, Opcode::destroyCallback, Opcode::slotAddress }) {
        auto copy = std::make_shared<joyeer::ir::Module>(*module);
        auto& function = named(*copy, "first");
        joyeer::ir::Instruction added { opcode };
        const auto count = opcode == Opcode::cloneCallback || opcode == Opcode::destroyCallback ? 0u
                         : opcode == Opcode::linearRangeGuard ? 1u : 2u;
        const bool yieldsInt = opcode == Opcode::cloneCallback || opcode == Opcode::destroyCallback ||
                               opcode == Opcode::slotAddress;
        ASSERT_TRUE(insertAfterIntConstant(function, added, count, yieldsInt));
        EXPECT_NE(verifierMessages(*copy).find("a collection capability requires runtime authorization"),
                  std::string::npos) << static_cast<int>(opcode) << "\n" << verifierMessages(*copy);
    }
}

TEST_F(CollectionRoleTest, TheWireRoundTripKeepsTheInertZeroFlagTheCopierAndTheAddressorMark) {
    load("func first(items: Buffer<Int>): Int { return items[0] }\n");
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    const auto* before = bufferDefinition(*module);
    ASSERT_NE(before, nullptr);
    ASSERT_TRUE(before->inertZero);
    ASSERT_TRUE(before->copier.has_value()) << "Buffer<Int> is copyable, so it has a copier";
    joyeer::llvmbackend::wire::Writer writer;
    writer(*module);
    joyeer::ir::Module decoded;
    joyeer::llvmbackend::wire::Reader reader(writer.bytes);
    reader(decoded);
    ASSERT_TRUE(reader.succeeded());
    const auto* after = bufferDefinition(decoded);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->inertZero, before->inertZero);
    EXPECT_EQ(after->copier, before->copier);
    const auto addressors = [](const joyeer::ir::Module& value) {
        std::vector<std::string> names;
        for (const auto& function : value.functions) {
            if (function.elementAddressor) names.push_back(function.name);
        }
        return names;
    };
    EXPECT_EQ(addressors(decoded), addressors(*module));
    EXPECT_EQ(addressors(*module).size(), 1u);
}

TEST_F(CollectionRoleTest, AnOwnedTypeThatIsNotCopyableHasNoCopier) {
    load("struct Resource { var value: Int\n deinit() {} }\n"
         "func use(items: borrowing Buffer<Resource>): Int { return items.count }\n"
         "func lent(items: borrowing Buffer<Buffer<Int>>): Int { return items.count }\n");
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    bool sawResource = false;
    bool sawNested = false;
    for (const auto& definition : module->structures) {
        if (definition.name == "Buffer<Resource>") {
            sawResource = true;
            EXPECT_TRUE(definition.inertZero);
            EXPECT_FALSE(definition.copier.has_value());
        }
        if (definition.name == "Buffer<Buffer<Int>>") {
            sawNested = true;
            EXPECT_TRUE(definition.copier.has_value());
        }
    }
    EXPECT_TRUE(sawResource);
    EXPECT_TRUE(sawNested);
    EXPECT_EQ(verifierMessages(*module).find("ir."), std::string::npos) << verifierMessages(*module);
}

TEST_F(CollectionRoleTest, TheFourIntrinsicsLowerPerConcreteTypeInTheLibraryBodies) {
    // The library's own bodies use each intrinsic; the lowered module names them per instance.
    const auto library = buffer(bufferHead, bufferElement, bufferCopy, bufferEnd, R"(
    deinit() {
        joyeer_intrinsic_destroy_range<T>(address: self.address, count: self.count)
    }
    public borrowing func callbacks(): Int {
        joyeer_intrinsic_clone_at<T>(destination: self.address, source: self.capacity)
        return joyeer_intrinsic_clone_callback<T>() + joyeer_intrinsic_destroy_callback<T>()
    }
}
)");
    load("func run(items: Buffer<String>): Int { return items.callbacks() }\n"
         "func runInt(items: Buffer<Int>): Int { return items.callbacks() }\n", library);
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    const auto dumped = joyeer::ir::dump(*module);
    EXPECT_NE(dumped.find("destroy_range"), std::string::npos) << dumped;
    EXPECT_NE(dumped.find("clone_at"), std::string::npos) << dumped;
    EXPECT_NE(dumped.find("clone_callback"), std::string::npos) << dumped;
    EXPECT_NE(dumped.find("destroy_callback"), std::string::npos) << dumped;
    EXPECT_EQ(verifierMessages(*module).find("ir."), std::string::npos) << verifierMessages(*module);
}

TEST_F(CollectionRoleTest, ALinearOwnedTypeDropsTheRangeDestroyForAGuardAndHasNoCallbacks) {
    // For a linear element, destroy_range only checks that nothing is left, and both callbacks are 0.
    const auto library = buffer(bufferHead, bufferElement, bufferCopy, bufferEnd, R"(
    deinit() {
        joyeer_intrinsic_destroy_range<T>(address: self.address, count: self.count)
    }
    public borrowing func callbacks(): Int {
        return joyeer_intrinsic_clone_callback<T>() + joyeer_intrinsic_destroy_callback<T>()
    }
}
)");
    load("func run(items: borrowing Buffer<JoinHandle<Int>>): Int { return items.callbacks() }\n", library);
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    const auto dumped = joyeer::ir::dump(*module);
    EXPECT_NE(dumped.find("linear_range_guard"), std::string::npos) << dumped;
    EXPECT_EQ(dumped.find("destroy_range"), std::string::npos) << dumped;
    EXPECT_EQ(dumped.find("clone_callback"), std::string::npos) << dumped;
    EXPECT_EQ(dumped.find("destroy_callback"), std::string::npos) << dumped;
    EXPECT_EQ(verifierMessages(*module).find("ir."), std::string::npos) << verifierMessages(*module);
}

// ---- Backend: the new opcodes and the role struct reach LLVM and pass its verifier ----

TEST_F(CollectionRoleTest, TheBackendEmitsTheRoleStructWithoutALiveFlagAndTheCapabilities) {
    const auto library = buffer(bufferHead, bufferElement, bufferCopy, bufferEnd, R"(
    deinit() {
        joyeer_intrinsic_destroy_range<T>(address: self.address, count: self.count)
    }
    public borrowing func callbacks(): Int {
        joyeer_intrinsic_clone_at<T>(destination: self.address, source: self.capacity)
        return joyeer_intrinsic_clone_callback<T>() + joyeer_intrinsic_destroy_callback<T>()
    }
}
)");
    load("func text(items: Buffer<String>): Int { return items.callbacks() + items[1].count }\n"
         "func number(items: Buffer<Int>): Int { return items.callbacks() + items[0] }\n"
         "func twice(items: Buffer<Int>): Buffer<Int> { let again = items\n return again }\n"
         "func main() {}\n", library);
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    for (const auto level : { joyeer::OptimizationLevel::O0, joyeer::OptimizationLevel::O2 }) {
        SCOPED_TRACE(static_cast<int>(level));
        joyeer::llvmbackend::EmitOptions options;
        options.optimizationLevel = level;
        const auto emitted = joyeer::llvmbackend::Emitter().emit(*module, options);
        ASSERT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
        // The struct keeps its three 64-bit words, with no fourth live flag.
        EXPECT_NE(emitted.text.find("{ i64, i64, i64 }"), std::string::npos) << emitted.text;
        EXPECT_EQ(emitted.text.find("{ i64, i64, i64, i1 }"), std::string::npos);
    }
}

TEST_F(CollectionRoleTest, TheBackendTrapsWhenALinearRangeIsNotEmpty) {
    const auto library = buffer(bufferHead, bufferElement, bufferCopy, bufferEnd, R"(
    deinit() {
        joyeer_intrinsic_destroy_range<T>(address: self.address, count: self.count)
    }
}
)");
    load("func use(items: consuming Buffer<JoinHandle<Int>>) { consume items.finish() }\n"
         "func main() {}\n", library);
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    const auto emitted = joyeer::llvmbackend::Emitter().emit(*module, {});
    ASSERT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
    EXPECT_NE(emitted.text.find("linear container dropped while not empty"), std::string::npos);
}

TEST_F(CollectionRoleTest, TheEscapeRuleRejectsEachWayAnElementAddressCouldOutliveItsAccess) {
    using joyeer::ir::Opcode;
    using joyeer::ir::ValueCategory;
    load("func first(items: Buffer<Int>): Int { return items[0] }\n"
         "func set(items: inout Buffer<Int>) { &items[0] = 4 }\n");
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(verifierMessages(*module).find("ir."), std::string::npos) << verifierMessages(*module);
    const auto place = [](joyeer::ir::Function& function) -> const joyeer::ir::Instruction* {
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (instruction.opcode == Opcode::integerAddress) return &instruction;
            }
        }
        return nullptr;
    };
    const auto insertAfterPlace = [](joyeer::ir::Function& function, joyeer::ir::Instruction added) {
        for (auto& block : function.blocks) {
            for (auto it = block.instructions.begin(); it != block.instructions.end(); ++it) {
                if (it->opcode == Opcode::integerAddress) {
                    block.instructions.insert(it + 1, std::move(added));
                    return;
                }
            }
        }
    };
    const auto intType = module->types.end() != std::find_if(module->types.begin(), module->types.end(),
            [](const auto& type) { return type.name == "Int"; })
            ? std::find_if(module->types.begin(), module->types.end(),
                           [](const auto& type) { return type.name == "Int"; })->id : 0;
    // Converting the address back to an Int.
    {
        auto copy = std::make_shared<joyeer::ir::Module>(*module);
        auto& function = named(*copy, "first");
        joyeer::ir::Instruction added { Opcode::pointerToInteger };
        added.operands = { place(function)->result->id };
        added.result = joyeer::ir::Value { 9000, intType, ValueCategory::value };
        insertAfterPlace(function, added);
        EXPECT_NE(verifierMessages(*copy).find("element address may not be used by pointer_to_integer"),
                  std::string::npos) << verifierMessages(*copy);
    }
    // Storing the address itself as a value.
    {
        auto copy = std::make_shared<joyeer::ir::Module>(*module);
        auto& function = named(*copy, "set");
        const auto address = place(function)->result->id;
        joyeer::ir::Instruction added { Opcode::store };
        added.operands = { address, address };
        insertAfterPlace(function, added);
        EXPECT_NE(verifierMessages(*copy).find("element address may not be used by store"), std::string::npos)
                << verifierMessages(*copy);
    }
    // Returning it.
    {
        auto copy = std::make_shared<joyeer::ir::Module>(*module);
        auto& function = named(*copy, "first");
        const auto address = place(function)->result->id;
        auto& last = function.blocks.back().instructions.back();
        ASSERT_EQ(last.opcode, Opcode::returnValue);
        last.operands = { address };
        EXPECT_NE(verifierMessages(*copy).find("element address may not be used by ret"), std::string::npos)
                << verifierMessages(*copy);
    }
    // A use in a block the definition does not dominate: a second entry-less block reading it.
    {
        auto copy = std::make_shared<joyeer::ir::Module>(*module);
        auto& function = named(*copy, "first");
        const auto address = place(function)->result->id;
        const auto loaded = [&] {
            for (const auto& block : function.blocks) {
                for (const auto& instruction : block.instructions) {
                    if (instruction.opcode == Opcode::load && !instruction.operands.empty() &&
                        instruction.operands[0] == address) {
                        return instruction.result->type;
                    }
                }
            }
            return intType;
        }();
        joyeer::ir::BasicBlock stray;
        stray.id = 900;
        stray.name = "stray";
        joyeer::ir::Instruction load { Opcode::load };
        load.operands = { address };
        load.result = joyeer::ir::Value { 9001, loaded, ValueCategory::value };
        joyeer::ir::Instruction done { Opcode::returnValue };
        done.operands = { 9001 };
        stray.instructions = { load, done };
        function.blocks.push_back(stray);
        EXPECT_NE(verifierMessages(*copy).find("does not dominate"), std::string::npos) << verifierMessages(*copy);
    }
}

TEST_F(CollectionRoleTest, AnElementOfAnArrayOrDictionaryTypeCanBeMutatedInPlace) {
    // The element address is the receiver of the builtin collection operations, which the escape rule allows.
    load("func appended(items: inout Buffer<[Int]>) { &items[0].append(element: 1) }\n"
         "func removed(items: inout Buffer<[Int]>): Int? { return &items[0].removeLast() }\n"
         "func inserted(items: inout Buffer<[String: Int]>) { &items[0][\"k\"] = 1 }\n"
         "func looked(items: Buffer<[String: Int]>): Int? { return items[0].get(key: \"k\") }\n"
         "func lookedUp(items: Buffer<[Int]>): Int { return items[0].count }\n");
    EXPECT_TRUE(checks()) << diagnostics();
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    EXPECT_EQ(verifierMessages(*module).find("ir."), std::string::npos) << verifierMessages(*module);
}

TEST_F(CollectionRoleTest, TheVerifierRejectsAMalformedCopierAddressorOrElementType) {
    load("func first(items: Buffer<Int>): Int { return items[0] }\n");
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(verifierMessages(*module).find("ir."), std::string::npos) << verifierMessages(*module);
    const auto addressor = [](joyeer::ir::Module& value) -> joyeer::ir::Function& {
        for (auto& function : value.functions) {
            if (function.elementAddressor) return function;
        }
        ADD_FAILURE() << "no element addressor";
        return value.functions.front();
    };
    {   // A consuming copier would destroy the borrowed source.
        auto copy = std::make_shared<joyeer::ir::Module>(*module);
        const auto* definition = bufferDefinition(*copy);
        ASSERT_NE(definition, nullptr);
        ASSERT_TRUE(definition->copier.has_value());
        copy->functions[*definition->copier].parameters[0].isConsuming = true;
        EXPECT_NE(verifierMessages(*copy).find("copier"), std::string::npos) << verifierMessages(*copy);
    }
    {   // An addressor must be a runtime function of the right shape.
        auto copy = std::make_shared<joyeer::ir::Module>(*module);
        addressor(*copy).runtimeContext = false;
        EXPECT_NE(verifierMessages(*copy).find("element addressor"), std::string::npos) << verifierMessages(*copy);
    }
    {
        auto copy = std::make_shared<joyeer::ir::Module>(*module);
        addressor(*copy).parameters.pop_back();
        EXPECT_NE(verifierMessages(*copy).find("element addressor"), std::string::npos) << verifierMessages(*copy);
    }
    {
        auto copy = std::make_shared<joyeer::ir::Module>(*module);
        addressor(*copy).isExternal = true;
        EXPECT_NE(verifierMessages(*copy).find("element addressor"), std::string::npos) << verifierMessages(*copy);
    }
    {   // The place must have the element type of the array: retype it as Bool.
        auto copy = std::make_shared<joyeer::ir::Module>(*module);
        const auto boolType = std::find_if(copy->types.begin(), copy->types.end(),
                                           [](const auto& type) { return type.name == "Bool"; });
        ASSERT_NE(boolType, copy->types.end());
        for (auto& block : named(*copy, "first").blocks) {
            for (auto& instruction : block.instructions) {
                if (instruction.opcode == joyeer::ir::Opcode::integerAddress) instruction.result->type = boolType->id;
            }
        }
        EXPECT_NE(verifierMessages(*copy).find("element type"), std::string::npos) << verifierMessages(*copy);
    }
}

TEST_F(CollectionRoleTest, TheBackendDestroysARangeLastToFirst) {
    const auto library = buffer(bufferHead, bufferElement, bufferCopy, bufferEnd, R"(
    deinit() {
        joyeer_intrinsic_destroy_range<T>(address: self.address, count: self.count)
    }
}
)");
    load("func use(items: borrowing Buffer<String>): Int { return items.count }\nfunc main() {}\n", library);
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    joyeer::llvmbackend::EmitOptions options;
    options.optimizationLevel = joyeer::OptimizationLevel::O0;
    const auto emitted = joyeer::llvmbackend::Emitter().emit(*module, options);
    ASSERT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
    const auto header = emitted.text.find("range.header");
    ASSERT_NE(header, std::string::npos) << emitted.text;
    const auto section = emitted.text.substr(header, 600);
    EXPECT_NE(section.find("icmp sgt"), std::string::npos) << section;
    EXPECT_NE(section.find("sub i64"), std::string::npos) << section;
}

TEST_F(CollectionRoleTest, ALinearContainerInsideAnotherTypeMustBeEndedThere) {
    // A struct that holds a linear library array has that field as a leaf it must end.
    load("struct Holder { var items: Buffer<JoinHandle<Int>>\n var extra: JoinHandle<Int> }\n"
         "func drop(holder: consuming Holder) {\n"
         "    let answer = consume holder.extra.join()\n"
         "    print(value: answer)\n}\n");
    EXPECT_NE(analyze().find("linear-not-consumed"), std::string::npos)
            << "holder.items is never ended";
    load("struct Holder { var items: Buffer<JoinHandle<Int>>\n var extra: JoinHandle<Int> }\n"
         "func end(holder: consuming Holder) {\n"
         "    let answer = consume holder.extra.join()\n"
         "    print(value: answer)\n"
         "    consume holder.items.finish()\n}\n");
    EXPECT_EQ(analyze(), "") << "ending both fields is accepted";
    // With only the array as a linear field, ending it ends the struct.
    load("struct Holder { var items: Buffer<JoinHandle<Int>>\n var count: Int }\n"
         "func end(holder: consuming Holder) { consume holder.items.finish() }\n");
    EXPECT_EQ(analyze(), "");
    // Through Optional, and dropped on a `?` early exit.
    load("func drop(maybe: consuming Buffer<JoinHandle<Int>>?) {}\n");
    EXPECT_NE(analyze().find("linear-not-consumed"), std::string::npos);
    load("func early(items: consuming Buffer<JoinHandle<Int>>, value: Int?): Int? {\n"
         "    let checked = value?\n"
         "    consume items.finish()\n"
         "    return checked\n}\n");
    EXPECT_NE(analyze().find("linear-not-consumed"), std::string::npos)
            << "the `?` returns early with the container live";
    // A struct that declares deinit may not hold a linear container.
    load("struct Owner { var items: Buffer<JoinHandle<Int>>\n deinit() {} }\n");
    EXPECT_FALSE(checks()) << "a deinit type may not hold a linear field";
}

TEST_F(CollectionRoleTest, TheEndingHintNamesTheContainerEndMethod) {
    load("func drop(items: consuming Buffer<JoinHandle<Int>>) {}\n");
    const auto reported = analyze();
    ASSERT_FALSE(analysis.diagnostics.empty()) << reported;
    ASSERT_TRUE(analysis.diagnostics.front().help.has_value());
    EXPECT_NE(analysis.diagnostics.front().help->find("'finish'"), std::string::npos)
            << *analysis.diagnostics.front().help;
}

TEST_F(CollectionRoleTest, TheArrayRoleRequiresADeinitAndEveryMethodRole) {
    const auto withoutDeinit = std::string(bufferHead) + bufferElement + bufferCopy + bufferEnd + "}\n";
    EXPECT_TRUE(rejectedAsInvalidPrelude(withoutDeinit, "deinit"));
    // Each method role must be present, not only unique.
    EXPECT_TRUE(rejectedAsInvalidPrelude(buffer(bufferHead, "", bufferCopy, bufferEnd), "array_element"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(buffer(bufferHead, bufferElement, "", bufferEnd), "array_copy"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(buffer(bufferHead, bufferElement, bufferCopy, ""), "container_end"));
    // A non-public struct.
    {
        auto library = buffer();
        const auto at = library.find("public struct Buffer");
        ASSERT_NE(at, std::string::npos);
        library.replace(at, 6, "internal");
        EXPECT_TRUE(rejectedAsInvalidPrelude(library, "public"));
    }
}

TEST_F(CollectionRoleTest, CountCannotBeConsumedOrMutatedThroughAnyPath) {
    load("func bad(items: consuming Buffer<Int>) { let taken = consume items.count }\n");
    const auto reported = checks() ? analyze() : diagnostics();
    EXPECT_NE(reported.find("partial-consume-of-deinit-type"), std::string::npos)
            << "consuming count would zero-fill it: " << reported;
    // Writing it says why and what to use instead.
    load("func bad(items: inout Buffer<Int>) { &items.count = 3 }\n");
    ASSERT_FALSE(checking.diagnostics.empty());
    ASSERT_TRUE(checking.diagnostics.front().help.has_value());
    EXPECT_NE(checking.diagnostics.front().help->find("count"), std::string::npos);
    EXPECT_TRUE(rejectedMentioning(
            "struct Holder { var items: Buffer<Int> }\n"
            "func bad(holder: inout Holder) { &holder.items.count = 1 }\n", "count")) << diagnostics();
    // A read nested in another struct is fine.
    load("struct Holder { var items: Buffer<Int> }\n"
         "func size(holder: Holder): Int { return holder.items.count }\n");
    EXPECT_TRUE(checks()) << diagnostics();
}

TEST_F(CollectionRoleTest, ARoleAttributeInUserCodeIsNotAccepted) {
    // The prelude-only annotation is a parse error in an ordinary unit, so no user type gains a role.
    Diagnostics lexerDiagnostics;
    LexParser lexer(&lexerDiagnostics);
    const auto source = std::make_shared<SourceFile>(
            "@compiler_role(array_type)\npublic struct Mine<T> {\n private var address: Int\n"
            " private var count: Int\n private var capacity: Int\n}\nfunc main() {}\n");
    lexer.parse(source);
    ASSERT_TRUE(lexerDiagnostics.errors.empty());
    EXPECT_FALSE(joyeer::parser::Parser(source->tokens, true).parse().succeeded());
}

TEST_F(CollectionRoleTest, CloneAtOfANoncopyableTypeIsRejectedWhereItIsWritten) {
    const auto library = buffer(bufferHead, bufferElement, bufferCopy, bufferEnd, R"(
    deinit() {
        joyeer_intrinsic_destroy_range<T>(address: self.address, count: self.count)
    }
    public borrowing func bad(): Int {
        joyeer_intrinsic_clone_at<JoinHandle<Int>>(destination: 0, source: 0)
        return 0
    }
}
)");
    load("func main() {}\n", library);
    ASSERT_TRUE(resolution.succeeded()) << diagnostics();
    EXPECT_TRUE(hasTypeDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::invalidTypeArgument)) << diagnostics();
}

// ---- slot_address and the inlined addressor (decision 0026, measurement) ----

TEST_F(CollectionRoleTest, SlotAddressIsAnAuthorizedUncheckedElementAddress) {
    // Rejected in user code like every layout intrinsic, with and without written type arguments.
    load("func main() {\n    let address = joyeer_intrinsic_slot_address<Int>(base: 0, index: 1)\n}\n");
    ASSERT_TRUE(resolution.succeeded()) << diagnostics();
    EXPECT_TRUE(hasTypeDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::unauthorizedIntrinsic))
            << diagnostics();
    load("func main() {\n    print(value: joyeer_intrinsic_slot_address(base: 0, index: 1))\n}\n");
    ASSERT_TRUE(resolution.succeeded()) << diagnostics();
    EXPECT_TRUE(hasTypeDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::unauthorizedIntrinsic))
            << diagnostics();
    // Used by a library addressor it lowers to one opcode per instance and verifies.
    const auto library = buffer(bufferHead, R"(
    @compiler_role(array_element)
    public borrowing func element(index: Int): Int {
        return joyeer_intrinsic_slot_address<T>(base: self.address, index: index)
    }
)", bufferCopy, bufferEnd, bufferTail);
    load("func first(items: Buffer<Int>): Int { return items[1] }\n"
         "func text(items: Buffer<String>): Int { return items[0].count }\n"
         "func unit(items: Buffer<Void>) { let value = items[2] }\n", library);
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    EXPECT_EQ(verifierMessages(*module).find("ir."), std::string::npos) << verifierMessages(*module);
    std::set<int64_t> elementTypes;
    for (const auto& function : module->functions) {
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (instruction.opcode == joyeer::ir::Opcode::slotAddress) elementTypes.insert(instruction.integerValue);
            }
        }
    }
    EXPECT_EQ(elementTypes.size(), 3u) << "one slot_address per element type";
    joyeer::llvmbackend::EmitOptions options;
    options.optimizationLevel = joyeer::OptimizationLevel::O0;
    const auto emitted = joyeer::llvmbackend::Emitter().emit(*module, options);
    ASSERT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
    const auto& text = emitted.text;
    // The addressor is `alwaysinline`, and the element address is a plain getelementptr over the element type,
    // with no overflow check (a field access is an `inbounds` getelementptr over the struct type instead).
    EXPECT_NE(text.find("alwaysinline"), std::string::npos) << text;
    EXPECT_TRUE(std::regex_search(text, std::regex(R"(= getelementptr i64, ptr %[^,]+, i64 %)"))) << text;
    EXPECT_TRUE(std::regex_search(text, std::regex(R"(= getelementptr %joyeer\.string, ptr %[^,]+, i64 %)"))) << text;
    EXPECT_EQ(text.find("smul.with.overflow"), std::string::npos) << text;
}

TEST_F(CollectionRoleTest, SlotAddressMalformedFormsAreRejectedByTheVerifier) {
    using joyeer::ir::Opcode;
    // Each form is placed in the addressor, a runtime function, so only the targeted rule can fire.
    const auto library = buffer(bufferHead, R"(
    @compiler_role(array_element)
    public borrowing func element(index: Int): Int {
        return joyeer_intrinsic_slot_address<T>(base: self.address + 0, index: index)
    }
)", bufferCopy, bufferEnd, bufferTail);
    load("func first(items: Buffer<Int>): Int { return items[1] }\n", library);
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(verifierMessages(*module).find("ir."), std::string::npos) << verifierMessages(*module);
    const auto addressor = [](joyeer::ir::Module& value) -> joyeer::ir::Function& {
        for (auto& function : value.functions) {
            if (function.elementAddressor) return function;
        }
        ADD_FAILURE() << "no element addressor";
        return value.functions.front();
    };
    const auto typeNamed = [&](const std::string& name) {
        for (const auto& type : module->types) {
            if (type.name == name) return static_cast<int64_t>(type.id);
        }
        ADD_FAILURE() << name;
        return int64_t { 0 };
    };
    struct Case {
        const char* what;
        unsigned operands;
        bool yieldsInt;
        int64_t type;
        const char* expected;
    };
    const std::vector<Case> cases {
        { "one operand", 1, true, typeNamed("Int"), "expects 2 operand(s)" },
        { "three operands", 3, true, typeNamed("Int"), "expects 2 operand(s)" },
        { "no result", 2, false, typeNamed("Int"), "slot_address yields an Int" },
        { "Never element", 2, true, typeNamed("Never"), "needs a concrete type with values" },
        { "unknown element", 2, true, 999999, "needs a concrete type with values" },
    };
    for (const auto& form : cases) {
        SCOPED_TRACE(form.what);
        auto copy = std::make_shared<joyeer::ir::Module>(*module);
        joyeer::ir::Instruction added { Opcode::slotAddress };
        added.integerValue = form.type;
        ASSERT_TRUE(insertAfterIntConstant(addressor(*copy), added, form.operands, form.yieldsInt));
        EXPECT_NE(verifierMessages(*copy).find(form.expected), std::string::npos) << verifierMessages(*copy);
    }
    {   // A Bool result and a Bool operand.
        auto copy = std::make_shared<joyeer::ir::Module>(*module);
        joyeer::ir::Instruction added { Opcode::slotAddress };
        added.integerValue = typeNamed("Int");
        ASSERT_TRUE(insertAfterIntConstant(addressor(*copy), added, 2, true));
        for (auto& block : addressor(*copy).blocks) {
            for (auto& instruction : block.instructions) {
                if (instruction.opcode == Opcode::slotAddress && instruction.result.has_value() &&
                    instruction.result->id == 9000) {
                    instruction.result->type = static_cast<joyeer::ir::TypeId>(typeNamed("Bool"));
                }
            }
        }
        EXPECT_NE(verifierMessages(*copy).find("slot_address yields an Int"), std::string::npos)
                << verifierMessages(*copy);
    }
    {   // A target list.
        auto copy = std::make_shared<joyeer::ir::Module>(*module);
        joyeer::ir::Instruction added { Opcode::slotAddress };
        added.integerValue = typeNamed("Int");
        added.targets = { 0 };
        ASSERT_TRUE(insertAfterIntConstant(addressor(*copy), added, 2, true));
        EXPECT_NE(verifierMessages(*copy).find("0 target(s)"), std::string::npos) << verifierMessages(*copy);
    }
}

TEST_F(CollectionRoleTest, TheWireRoundTripKeepsSlotAddressInstructions) {
    const auto library = buffer(bufferHead, R"(
    @compiler_role(array_element)
    public borrowing func element(index: Int): Int {
        return joyeer_intrinsic_slot_address<T>(base: self.address, index: index)
    }
)", bufferCopy, bufferEnd, bufferTail);
    load("func first(items: Buffer<Int>): Int { return items[1] }\n", library);
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    const auto collect = [](const joyeer::ir::Module& value) {
        std::vector<std::string> found;
        for (const auto& function : value.functions) {
            for (const auto& block : function.blocks) {
                for (const auto& instruction : block.instructions) {
                    if (instruction.opcode != joyeer::ir::Opcode::slotAddress) continue;
                    std::string line = std::to_string(instruction.integerValue);
                    for (const auto operand : instruction.operands) line += "," + std::to_string(operand);
                    line += "->" + std::to_string(instruction.result.has_value() ? instruction.result->id : 0);
                    found.push_back(line);
                }
            }
        }
        return found;
    };
    joyeer::llvmbackend::wire::Writer writer;
    writer(*module);
    joyeer::ir::Module decoded;
    joyeer::llvmbackend::wire::Reader reader(writer.bytes);
    reader(decoded);
    ASSERT_TRUE(reader.succeeded());
    EXPECT_EQ(collect(decoded), collect(*module));
    EXPECT_EQ(collect(*module).size(), 1u);
    EXPECT_EQ(verifierMessages(decoded).find("ir."), std::string::npos) << verifierMessages(decoded);
}

} // namespace
