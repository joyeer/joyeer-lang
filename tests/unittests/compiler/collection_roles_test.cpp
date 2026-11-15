// Decision 0025: the compiler capabilities a library-written array needs. Every test here is written
// before the implementation and fails until the capability exists. The library under test is a small
// `Buffer<T>` bound by the role `array_type` and supplied through the role-prelude test seam of
// decision 0024 (the bundled prelude has no such declaration until Phase 3 of plan 0011).
#include "compiler/backend/ir_wire.h"
#include "compiler/frontend/lexparser.h"
#include "compiler/frontend/nameresolution.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/semanticanalysis.h"
#include "compiler/frontend/typechecking.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/irlowering.h"

#include <gtest/gtest.h>

#include <algorithm>
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
        return Buffer(address: 0, count: 0, capacity: 0)
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
        const auto userParse = joyeer::parser::Parser(userSource->tokens).parse();
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
        auto result = joyeer::lowering::Lowerer().lower(
                checking.model, "fixture.joyeer",
                joyeer::ir::SourceInfo { "fixture.joyeer", ".", userSource->content.size(), userSource->lineStarts });
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
            buffer(head, replaceAll(element, "borrowing func element(index: Int)", "func element(index: Int)")),
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
            "func touch(value: inout Int) {}\n", "count")) << diagnostics();
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
         "func ok(items: consuming Buffer<Resource>) {\n"
         "    let first = consume items[0]\n"
         "    consume items.finish()\n"
         "}\n");
    EXPECT_EQ(analyze().find("consume-of-linear-element"), std::string::npos);
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
    EXPECT_EQ(joyeer::llvmbackend::wire::version, 20u);
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

} // namespace
