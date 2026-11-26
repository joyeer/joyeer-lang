// Decision 0025: the compiler capabilities a library-written array needs. Every test here is written
// before the implementation and fails until the capability exists. The library under test is a small
// `Buffer<T>` bound by the role `array_type` and supplied through the collections-prelude test seam.
#include "compiler/backend/ir_wire.h"
#include "compiler/codegen/llvm.h"
#include "compiler/frontend/lexparser.h"
#include "compiler/frontend/nameresolution.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/semanticanalysis.h"
#include "compiler/frontend/typechecking.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/irlowering.h"
#include "dict_role_library.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <regex>
#include <set>
#include <sstream>
#include <string>

namespace {

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
// The literal method: called on an inert zero value, it builds the array from `count` moved-in elements.
constexpr auto bufferLiteral = R"(
    @compiler_role(array_literal)
    private consuming func literal(address: Int, count: Int): Buffer<T> {
        return joyeer_intrinsic_move_out<Buffer<T>>(address: address)
    }
)";
constexpr auto bufferTail = R"(
    deinit() {
        joyeer_intrinsic_destroy_range<T>(address: self.address, count: self.count)
    }
}
)";

std::string buffer(const std::string& head = bufferHead, const std::string& element = bufferElement,
                   const std::string& copy = bufferCopy, const std::string& end = bufferEnd,
                   const std::string& tail = bufferTail, const std::string& literal = bufferLiteral) {
    return head + element + copy + end + literal + tail;
}

class CollectionRoleTest : public testing::Test {
protected:
    // Resolves and type checks `user` with `library` replacing the collections prelude.
    void load(const std::string& user, const std::string& library = buffer()) {
        Diagnostics lexerDiagnostics;
        LexParser lexer(&lexerDiagnostics);
        userSource = std::make_shared<SourceFile>(user);
        lexer.parse(userSource);
        ASSERT_TRUE(lexerDiagnostics.errors.empty());
        const auto userParse = joyeer::parser::Parser(userSource->tokens, true).parse();
        ASSERT_TRUE(userParse.succeeded()) << joyeer::parser::dump(userParse.diagnostics);

        roleSource = std::make_shared<SourceFile>(library);
        lexer.parse(roleSource);
        ASSERT_TRUE(lexerDiagnostics.errors.empty());
        for (auto& token : roleSource->tokens) token->span.sourceId = 1;
        const auto roleParse = joyeer::parser::Parser(roleSource->tokens, true, true).parse();
        ASSERT_TRUE(roleParse.succeeded()) << joyeer::parser::dump(roleParse.diagnostics);

        resolution = joyeer::semantic::NameResolver().resolve({
                { "app", { userParse.root } },
                { "$collections-prelude", { roleParse.root }, true, false, true,
                  joyeer::semantic::ReplacedPrelude::collections } });
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

// The String roles of the collections prelude (decision 0028 stage 3): five private, non-generic functions with
// closed signatures, labels and borrowed parameters, each with a body, and an optional `string_utf8` method of the
// array role. The test prelude here has none unless a test adds them.
constexpr auto stringRoles = R"(
@c_extern(symbol: "joyeer_intrinsic_string_count")
private func stringCount(text: String): Int
@compiler_role(string_count)
private func byteCount(text: String): Int { return stringCount(text: text) }
@compiler_role(string_byte)
private func byteAt(text: String, index: Int): UInt8 { return 0 }
@compiler_role(string_concat)
private func concatenate(left: String, right: String): String { return left }
@compiler_role(string_equal)
private func equals(left: String, right: String): Bool { return false }
@compiler_role(string_order)
private func order(left: String, right: String): Int { return 0 }
)";
constexpr auto bufferUtf8 = R"(
    @compiler_role(string_utf8)
    private consuming func utf8(text: String): Buffer<T> {
        return joyeer_intrinsic_move_out<Buffer<T>>(address: 0)
    }
)";

TEST_F(CollectionRoleTest, WellShapedStringRolesAreAccepted) {
    load("func main() {}\n", buffer() + stringRoles);
    EXPECT_TRUE(checks()) << diagnostics();
    load("func main() {}\n", buffer(bufferHead, bufferElement, bufferCopy, bufferEnd,
                                    std::string(bufferUtf8) + bufferTail));
    EXPECT_TRUE(checks()) << diagnostics();
}

TEST_F(CollectionRoleTest, MalformedStringRolesAreRejected) {
    const std::string valid = buffer();
    const auto role = [](const std::string& declaration) {
        return std::string("@compiler_role(string_count)\n") + declaration + "\n";
    };
    for (const auto& declaration : std::vector<std::string> {
            "public func byteCount(text: String): Int { return 0 }",
            "private func byteCount<T>(text: String): Int { return 0 }",
            "private func byteCount(text: consuming String): Int { return 0 }",
            "private func byteCount(text: inout String): Int { return 0 }",
            "private func byteCount(t: String): Int { return 0 }",
            "private func byteCount(text: String): Int32 { return 0 }",
            "private func byteCount(text: String, extra: Int): Int { return 0 }",
            "@c_extern(symbol: \"joyeer_intrinsic_string_count\")\nprivate func byteCount(text: String): Int" }) {
        SCOPED_TRACE(declaration);
        EXPECT_TRUE(rejectedAsInvalidPrelude(valid + role(declaration), "the string_count role requires"))
                << diagnostics();
    }
    EXPECT_TRUE(rejectedAsInvalidPrelude(valid + "@compiler_role(string_order)\n"
                                         "private func order(left: String, right: String): Bool { return false }\n",
                                         "the string_order role requires"))
            << diagnostics();
    EXPECT_TRUE(rejectedAsInvalidPrelude(valid + "@compiler_role(string_byte)\n"
                                         "private func byteAt(text: String, index: Int32): UInt8 { return 0 }\n",
                                         "the string_byte role requires"))
            << diagnostics();
    EXPECT_TRUE(rejectedAsInvalidPrelude(valid + stringRoles + role("private func again(text: String): Int { return 0 }"),
                                         "declares a second function for the string_count role"))
            << diagnostics();
    // The utf8 method: consuming, private, `text: String`, returning the declaring type, at most once.
    for (const auto& method : std::vector<std::string> {
            "    @compiler_role(string_utf8)\n    private borrowing func utf8(text: String): Buffer<T> {\n"
            "        return joyeer_intrinsic_move_out<Buffer<T>>(address: 0)\n    }\n",
            "    @compiler_role(string_utf8)\n    public consuming func utf8(text: String): Buffer<T> {\n"
            "        return joyeer_intrinsic_move_out<Buffer<T>>(address: 0)\n    }\n",
            "    @compiler_role(string_utf8)\n    private consuming func utf8(bytes: String): Buffer<T> {\n"
            "        return joyeer_intrinsic_move_out<Buffer<T>>(address: 0)\n    }\n",
            "    @compiler_role(string_utf8)\n    private consuming func utf8(text: String): Buffer<Int> {\n"
            "        return joyeer_intrinsic_move_out<Buffer<Int>>(address: 0)\n    }\n" }) {
        SCOPED_TRACE(method);
        EXPECT_TRUE(rejectedAsInvalidPrelude(
                buffer(bufferHead, bufferElement, bufferCopy, bufferEnd, method + bufferTail),
                "the string_utf8 role requires"))
                << diagnostics();
    }
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            buffer(bufferHead, bufferElement, bufferCopy, bufferEnd,
                   std::string(bufferUtf8) + R"(
    @compiler_role(string_utf8)
    private consuming func bytes(text: String): Buffer<T> {
        return joyeer_intrinsic_move_out<Buffer<T>>(address: 0)
    }
)" + bufferTail),
            "allows at most one method with the role string_utf8"))
            << diagnostics();
}

// A prelude without the String roles still checks; only lowering a String operation reports the missing function.
TEST_F(CollectionRoleTest, AStringOperationWithoutItsRoleIsALoweringDiagnostic) {
    for (const auto* body : { "return left + right", "return left == right", "return left.count",
                              "return left[0]", "return left.utf8()" }) {
        SCOPED_TRACE(body);
        const std::string type = std::string(body).find("+") != std::string::npos ? "String"
                : std::string(body).find("==") != std::string::npos ? "Bool"
                : std::string(body).find("count") != std::string::npos ? "Int"
                : std::string(body).find("utf8") != std::string::npos ? "[UInt8]" : "UInt8";
        load("func use(left: String, right: String): " + type + " { " + body + " }\n");
        ASSERT_TRUE(checks()) << diagnostics();
        analysis = joyeer::analysis::Analyzer().analyze(checking.model);
        ASSERT_TRUE(analysis.succeeded()) << joyeer::analysis::dump(analysis.diagnostics);
        const auto result = joyeer::lowering::Lowerer().lower(checking.model, "fixture.joyeer");
        EXPECT_FALSE(result.succeeded());
        const auto text = joyeer::lowering::dump(result.diagnostics);
        EXPECT_NE(text.find("ir-lowering.missing-symbol"), std::string::npos) << text;
    }
}

TEST_F(CollectionRoleTest, AWellShapedArrayRoleIsAccepted) {
    load("func main() {}\n");
    ASSERT_TRUE(resolution.succeeded()) << diagnostics();
    EXPECT_TRUE(checking.succeeded()) << diagnostics();
}

TEST_F(CollectionRoleTest, ArraySugarAndLiteralsBindToTheRoleEvenWhenArrayIsShadowed) {
    load(R"(
struct Array { var value: Int }
func main() {
    let values: [Int] = [1, 2]
    let same: Buffer<Int> = values
    let unrelated = Array(value: 3)
    print(value: same.count + unrelated.value)
}
)");
    ASSERT_TRUE(checks()) << diagnostics();
    const auto id = checking.model->types().arrayType(checking.model->types().intType());
    const auto* type = checking.model->types().type(id);
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(type->kind, joyeer::typing::TypeKind::structure);
    EXPECT_EQ(type->symbol, resolution.model->roleSymbol(joyeer::semantic::LibraryRole::arrayType));
    EXPECT_TRUE(checking.model->isArrayRoleType(id));
    EXPECT_EQ(checking.model->types().displayName(id), "[Int]");
}

// A literal used directly as the base of a member access, or the sequence of a `for`, still binds to the role
// type when a user type shadows `Array`: the member must resolve on the role's `count`, not the user's.
TEST_F(CollectionRoleTest, ALiteralAsAMemberBaseBindsToTheRoleWhenArrayIsShadowed) {
    load(R"(
struct Array { var count: Int }
func main() {
    print(value: [1, 2].count)
    var total = 0
    for value in [3, 4] { total = total + value }
    print(value: total + Array(count: 5).count)
}
)");
    ASSERT_TRUE(checks()) << diagnostics();
    EXPECT_NE(lower(), nullptr) << "the literal's member access must lower to a verified module";
}

TEST_F(CollectionRoleTest, ALiteralMovesIntoOneStackBlockAndCallsTheRoleMethod) {
    load("func main() { let values: [Int] = [1, 2, 3]\n print(value: values[0]) }\n");
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    size_t blocks = 0;
    size_t literals = 0;
    for (const auto& function : module->functions) {
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                blocks += instruction.opcode == joyeer::ir::Opcode::stackBlock;
                if (instruction.opcode == joyeer::ir::Opcode::call && instruction.callee.has_value()) {
                    literals += module->functions[*instruction.callee].name.find(".literal") != std::string::npos;
                }
            }
        }
    }
    EXPECT_EQ(blocks, 1u);
    EXPECT_EQ(literals, 1u);
    EXPECT_TRUE(joyeer::ir::Verifier().verify(*module).succeeded());
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
    // The syntax sugar binds to this same role declaration.
    load("func bad(items: consuming [JoinHandle<Int>]) {\n"
         "    let handle = consume items[0]\n"
         "    consume items.finish()\n"
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
        if (definition.role == joyeer::ir::StructureRole::array) return &definition;
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
    EXPECT_EQ(joyeer::llvmbackend::wire::version, 29u);
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
            if (it->opcode != joyeer::ir::Opcode::literal || !it->result.has_value()) continue;
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
        auto* constant = find(function, Opcode::literal);
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
        ASSERT_EQ(definition->role, joyeer::ir::StructureRole::array);
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
    struct Capability {
        Opcode opcode;
        uint8_t modifier;
        unsigned operands;
        const char* message;
    };
    const auto clone = static_cast<uint8_t>(joyeer::ir::TypeHelperKind::clone);
    const auto destroy = static_cast<uint8_t>(joyeer::ir::TypeHelperKind::destroy);
    for (const auto& capability : {
                 Capability { Opcode::destroyRange, 0, 2, "a collection capability requires runtime authorization" },
                 Capability { Opcode::cloneAt, 0, 2, "a collection capability requires runtime authorization" },
                 Capability { Opcode::trapIf, 0, 1, "a trap requires runtime authorization" },
                 Capability { Opcode::typeHelper, clone, 0, "a collection capability requires runtime authorization" },
                 Capability { Opcode::typeHelper, destroy, 0,
                              "a collection capability requires runtime authorization" },
                 Capability { Opcode::slotAddress, 0, 2, "a collection capability requires runtime authorization" },
         }) {
        auto copy = std::make_shared<joyeer::ir::Module>(*module);
        auto& function = named(*copy, "first");
        joyeer::ir::Instruction added { capability.opcode };
        added.modifier = capability.modifier;
        added.text = "message";
        const bool yieldsInt = capability.opcode == Opcode::typeHelper || capability.opcode == Opcode::slotAddress;
        ASSERT_TRUE(insertAfterIntConstant(function, added, capability.operands, yieldsInt));
        EXPECT_NE(verifierMessages(*copy).find(capability.message), std::string::npos)
                << static_cast<int>(capability.opcode) << "\n" << verifierMessages(*copy);
    }
}

TEST_F(CollectionRoleTest, TheWireRoundTripKeepsTheInertZeroFlagTheCopierAndTheAddressorMark) {
    load("func first(items: Buffer<Int>): Int { return items[0] }\n");
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    const auto* before = bufferDefinition(*module);
    ASSERT_NE(before, nullptr);
    ASSERT_EQ(before->role, joyeer::ir::StructureRole::array);
    ASSERT_TRUE(before->copier.has_value()) << "Buffer<Int> is copyable, so it has a copier";
    joyeer::llvmbackend::wire::Writer writer;
    writer(*module);
    joyeer::ir::Module decoded;
    joyeer::llvmbackend::wire::Reader reader(writer.bytes);
    reader(decoded);
    ASSERT_TRUE(reader.succeeded());
    const auto* after = bufferDefinition(decoded);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->role, before->role);
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
        if (definition.name == "[Resource]") {
            sawResource = true;
            EXPECT_EQ(definition.role, joyeer::ir::StructureRole::array);
            EXPECT_FALSE(definition.copier.has_value());
        }
        if (definition.name == "[[Int]]") {
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
    EXPECT_NE(dumped.find("type_helper clone"), std::string::npos) << dumped;
    EXPECT_NE(dumped.find("type_helper destroy"), std::string::npos) << dumped;
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
    EXPECT_NE(dumped.find("trap_if"), std::string::npos) << dumped;
    EXPECT_NE(dumped.find("\"linear container dropped while not empty\""), std::string::npos) << dumped;
    EXPECT_EQ(dumped.find("destroy_range"), std::string::npos) << dumped;
    EXPECT_EQ(dumped.find("type_helper"), std::string::npos) << dumped;
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
    // The replacement prelude has no String roles (decision 0028 stage 3), so the String element is only read.
    load("func text(items: Buffer<String>): Int { let value = items[1]\n return items.callbacks() }\n"
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
        ASSERT_EQ(last.opcode, Opcode::ret);
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
        joyeer::ir::Instruction done { Opcode::ret };
        done.operands = { 9001 };
        stray.instructions = { load, done };
        function.blocks.push_back(stray);
        EXPECT_NE(verifierMessages(*copy).find("does not dominate"), std::string::npos) << verifierMessages(*copy);
    }
}

TEST_F(CollectionRoleTest, AnElementOfAnArrayOrDictionaryTypeCanBeMutatedInPlace) {
    const auto library = buffer(bufferHead, bufferElement, bufferCopy, bufferEnd, R"(
    public mutating func append(element: consuming T) {
        joyeer_intrinsic_move_in<T>(address: self.address, value: consume element)
    }
    public mutating func removeLast(): Optional<T> {
        return .Some(joyeer_intrinsic_move_out<T>(address: self.address))
    }
    deinit() {
        joyeer_intrinsic_destroy_range<T>(address: self.address, count: self.count)
    }
}
)");
    load("func appended(items: inout Buffer<[Int]>) { &items[0].append(element: 1) }\n"
         "func removed(items: inout Buffer<[Int]>): Int? { return &items[0].removeLast() }\n"
         "func inserted(items: inout Buffer<[String: Int]>) { &items[0][\"k\"] = 1 }\n"
         "func looked(items: Buffer<[String: Int]>): Int? { return items[0].get(key: \"k\") }\n"
         "func lookedUp(items: Buffer<[Int]>): Int { return items[0].count }\n",
         // `[K: V]` binds to the dictionary role, which a replaced collections prelude declares (decision 0027).
         library + joyeer::fixtures::dictRoleLibrary());
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
        EXPECT_TRUE(rejectedAsInvalidPrelude(library, "public")) << diagnostics();
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
         "func text(items: Buffer<String>) { let value = items[0] }\n"
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
    // Internal linkage lets the always-inliner discard the out-of-line copy of every instance.
    EXPECT_TRUE(std::regex_search(text, std::regex(R"(define internal i64 @joyeer_fn_[0-9]+\(%joyeer\.struct)")))
            << text;
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

// ---- The collections prelude (decision 0026, stage 3) ----

// Resolves `user` with the bundled preludes, optionally replacing the collections prelude by `collections`
// and/or the concurrency prelude by `concurrency`; returns the source table of the resolved model.
struct PreludeLoad {
    joyeer::semantic::NameResolutionResult resolution;
    std::vector<uint32_t> ids;
};

PreludeLoad loadPreludes(const std::string& user, const std::string& collections = {},
                         const std::string& concurrency = {}) {
    Diagnostics diagnostics;
    LexParser lexer(&diagnostics);
    const auto userSource = std::make_shared<SourceFile>(user);
    lexer.parse(userSource);
    const auto userParse = joyeer::parser::Parser(userSource->tokens, true).parse();
    std::vector<joyeer::semantic::ModuleInput> modules { { "app", { userParse.root } } };
    std::vector<SourceFile::Ptr> keep;
    uint32_t nextId = 1;
    const auto supply = [&](const std::string& text, joyeer::semantic::ReplacedPrelude replaces) {
        const auto source = std::make_shared<SourceFile>(text);
        lexer.parse(source);
        for (auto& token : source->tokens) token->span.sourceId = nextId;
        ++nextId;
        const auto parse = joyeer::parser::Parser(source->tokens, true, true).parse();
        EXPECT_TRUE(parse.succeeded()) << joyeer::parser::dump(parse.diagnostics);
        joyeer::semantic::ModuleInput module { replaces == joyeer::semantic::ReplacedPrelude::none ? "$prelude" : "$collections-prelude",
                                               { parse.root }, true, false, true, replaces };
        modules.push_back(module);
        keep.push_back(source);
    };
    if (!concurrency.empty()) supply(concurrency, joyeer::semantic::ReplacedPrelude::none);
    if (!collections.empty()) supply(collections, joyeer::semantic::ReplacedPrelude::collections);
    PreludeLoad load;
    load.resolution = joyeer::semantic::NameResolver().resolve(modules);
    if (load.resolution.model != nullptr) {
        for (const auto& source : load.resolution.model->preludeSources()) {
            load.ids.push_back(source != nullptr && !source->tokens.empty() ? source->tokens.front()->span.sourceId : 999);
        }
    }
    return load;
}

TEST(CollectionsPrelude, BundledPreludesGetDenseSourceIdentitiesInOrder) {
    const auto load = loadPreludes("func main() {}\n");
    ASSERT_TRUE(load.resolution.succeeded()) << joyeer::semantic::dump(load.resolution.diagnostics);
    // The user source is 0; concurrency, roles, collections, and contracts follow.
    EXPECT_EQ(load.ids, (std::vector<uint32_t> { 1, 2, 3, 4 }));
    const auto& sources = load.resolution.model->preludeSources();
    ASSERT_EQ(sources.size(), 4u);
    EXPECT_NE(sources[0]->getAbstractPath().string().find("concurrency.joyeer"), std::string::npos);
    EXPECT_NE(sources[1]->getAbstractPath().string().find("roles.joyeer"), std::string::npos);
    EXPECT_NE(sources[2]->getAbstractPath().string().find("collections.joyeer"), std::string::npos);
    EXPECT_NE(sources[3]->getAbstractPath().string().find("contracts.joyeer"), std::string::npos);
}

TEST(CollectionsPrelude, ReplacingTheCollectionsPreludeKeepsTheIdentitiesDense) {
    const auto load = loadPreludes("func main() {}\n", buffer() + "public func fromTest(): Int { return 7 }\n");
    ASSERT_TRUE(load.resolution.succeeded()) << joyeer::semantic::dump(load.resolution.diagnostics);
    // The replacement carries source 1; the remaining bundled preludes follow it.
    EXPECT_EQ(load.ids, (std::vector<uint32_t> { 2, 3, 4 }));
}

TEST(CollectionsPrelude, ASuppliedConcurrencyPreludeDoesNotRemoveTheCollectionsPrelude) {
    // The collections prelude is separate from the concurrency prelude (decision 0026): a test that supplies its own
    // concurrency prelude still gets the bundled role and collections preludes, and `Optional` is visible to it.
    const auto load = loadPreludes("func main() {}\n", {},
            "public func fromConcurrency(): Int? { return nil }\n");
    ASSERT_TRUE(load.resolution.succeeded()) << joyeer::semantic::dump(load.resolution.diagnostics);
    EXPECT_EQ(load.ids, (std::vector<uint32_t> { 2, 3, 4 }));
}

TEST(CollectionsPrelude, ALibraryInTheCollectionsPreludeIsVisibleToUserCodeAndMayUseTheRoles) {
    const auto load = loadPreludes("func main() { let value: Int? = answer() }\n",
            buffer() + "public func answer(): Int? { return 42 }\n");
    ASSERT_TRUE(load.resolution.succeeded()) << joyeer::semantic::dump(load.resolution.diagnostics);
    const auto checking = joyeer::typing::TypeChecker().check(load.resolution.model);
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST(CollectionsPrelude, AMissingArrayRoleIsRejectedAtNameResolution) {
    const auto load = loadPreludes("func main() {}\n", "public struct Unrelated { var value: Int }\n");
    EXPECT_FALSE(load.resolution.succeeded());
    EXPECT_NE(joyeer::semantic::dump(load.resolution.diagnostics).find("array_type"), std::string::npos);
}

TEST_F(CollectionRoleTest, StackBlockIsAuthorizedHasALiteralCountAndLowersToOneOpcode) {
    load("func main() {\n    let block = joyeer_intrinsic_stack_block<Int>(count: 4)\n}\n");
    ASSERT_TRUE(resolution.succeeded()) << diagnostics();
    EXPECT_TRUE(hasTypeDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::unauthorizedIntrinsic))
            << diagnostics();
    const auto withBlock = [&](const std::string& body) {
        return buffer(bufferHead, bufferElement, bufferCopy, bufferEnd, R"(
    deinit() {
        joyeer_intrinsic_destroy_range<T>(address: self.address, count: self.count)
    }
    public borrowing func scratch(count: Int): Int {
)" + body + R"(
    }
}
)");
    };
    // A count that is not a literal is rejected where it is written.
    load("func main() {}\n", withBlock("        return joyeer_intrinsic_stack_block<T>(count: count)"));
    ASSERT_TRUE(resolution.succeeded()) << diagnostics();
    EXPECT_TRUE(hasTypeDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::invalidCallArguments)) << diagnostics();
    // A literal count lowers per instance to one `stack_block` and verifies; the backend sizes it at compile time.
    load("func use(items: Buffer<Int>): Int { return items.scratch(count: 1) }\n"
         "func text(items: Buffer<String>): Int { return items.scratch(count: 1) }\n",
         withBlock("        return joyeer_intrinsic_stack_block<T>(count: 8)"));
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    EXPECT_NE(joyeer::ir::dump(*module).find("stack_block of type"), std::string::npos);
    EXPECT_EQ(verifierMessages(*module).find("ir."), std::string::npos) << verifierMessages(*module);
    joyeer::llvmbackend::EmitOptions options;
    options.optimizationLevel = joyeer::OptimizationLevel::O0;
    const auto emitted = joyeer::llvmbackend::Emitter().emit(*module, options);
    ASSERT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
    EXPECT_TRUE(std::regex_search(emitted.text, std::regex(R"(alloca \[8 x i64\])"))) << emitted.text;
    EXPECT_TRUE(std::regex_search(emitted.text, std::regex(R"(alloca \[8 x %joyeer\.string\])"))) << emitted.text;
    // Wire round trip keeps the opcode, its type and its constant operand.
    joyeer::llvmbackend::wire::Writer writer;
    writer(*module);
    joyeer::ir::Module decoded;
    joyeer::llvmbackend::wire::Reader reader(writer.bytes);
    reader(decoded);
    ASSERT_TRUE(reader.succeeded());
    EXPECT_EQ(joyeer::ir::dump(decoded), joyeer::ir::dump(*module));
}

TEST_F(CollectionRoleTest, StackBlockMalformedFormsAreRejectedByTheVerifier) {
    using joyeer::ir::Opcode;
    const auto library = buffer(bufferHead, bufferElement, bufferCopy, bufferEnd, R"(
    deinit() {
        joyeer_intrinsic_destroy_range<T>(address: self.address, count: self.count)
    }
    public borrowing func scratch(): Int {
        return joyeer_intrinsic_stack_block<T>(count: 2)
    }
}
)");
    load("func use(items: Buffer<Int>): Int { return items.scratch() }\n", library);
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    ASSERT_EQ(verifierMessages(*module).find("ir."), std::string::npos) << verifierMessages(*module);
    // The count must be a constant: point the operand at a non-constant value.
    auto copy = std::make_shared<joyeer::ir::Module>(*module);
    bool changed = false;
    for (auto& function : copy->functions) {
        for (auto& block : function.blocks) {
            for (auto& instruction : block.instructions) {
                if (instruction.opcode == Opcode::stackBlock) {
                    // The addressor's own parameter is not an integer literal definition.
                    instruction.operands = { function.parameters.empty() ? 0 : function.parameters.back().value.id };
                    changed = true;
                }
            }
        }
    }
    ASSERT_TRUE(changed);
    EXPECT_NE(verifierMessages(*copy).find("constant count"), std::string::npos) << verifierMessages(*copy);
    // Two operands and a missing result.
    for (auto& function : copy->functions) {
        for (auto& block : function.blocks) {
            for (auto& instruction : block.instructions) {
                if (instruction.opcode == Opcode::stackBlock) instruction.operands.push_back(instruction.operands.front());
            }
        }
    }
    EXPECT_NE(verifierMessages(*copy).find("expects 1 operand(s)"), std::string::npos) << verifierMessages(*copy);
}

TEST_F(CollectionRoleTest, PlaceAddressIsAuthorizedAndOnlyADirectArgumentOfAnInoutPlace) {
    load("func main() {\n    var value = 1\n    let address = joyeer_intrinsic_place_address<Int>(value: &value)\n}\n");
    ASSERT_TRUE(resolution.succeeded()) << diagnostics();
    EXPECT_TRUE(hasTypeDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::unauthorizedIntrinsic))
            << diagnostics();
    const auto withMethods = [&](const std::string& body) {
        return buffer(bufferHead, bufferElement, bufferCopy, bufferEnd, R"(
    deinit() {
        joyeer_intrinsic_destroy_range<T>(address: self.address, count: self.count)
    }
)" + body + R"(
}
private func sink(address: Int, count: Int) {}
private func hold(address: Int): Int { return address }
)");
    };
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    // The accepted form: a direct argument of a call, over a mutating receiver and a field path of it.
    load("func main() {}\n", withMethods(R"(
    public mutating func accepted() {
        sink(address: joyeer_intrinsic_place_address<Buffer<T>>(value: &self), count: 1)
        sink(address: joyeer_intrinsic_place_address<Int>(value: &self.count), count: 1)
    }
    public func local() {
        var slot = 0
        sink(address: joyeer_intrinsic_place_address<Int>(value: &slot), count: 1)
    }
    public consuming func owned() {
        sink(address: joyeer_intrinsic_place_address<Buffer<T>>(value: &self), count: 1)
    }
)"));
    ASSERT_TRUE(resolution.succeeded()) << diagnostics();
    EXPECT_TRUE(checking.succeeded()) << diagnostics();
    // A stored result, a returned result and one in a condition outlive the access and are rejected.
    const auto stored = [&](const std::string& form) {
        load("func main() {}\n", withMethods("    public func bad() {\n        var slot = 0\n        " + form + "\n    }\n"));
        EXPECT_TRUE(resolution.succeeded()) << diagnostics();
        return hasTypeDiagnostic(Id::invalidCallArguments);
    };
    EXPECT_TRUE(stored("let address = joyeer_intrinsic_place_address<Int>(value: &slot)")) << diagnostics();
    EXPECT_TRUE(stored("let again = hold(address: 1) + joyeer_intrinsic_place_address<Int>(value: &slot)"))
            << diagnostics();
    // Receivers that are not writable places are rejected: a `let`, a borrowing parameter and a borrowing self.
    const auto notWritable = [&](const std::string& body) {
        load("func main() {}\n", withMethods(body));
        EXPECT_TRUE(resolution.succeeded()) << diagnostics();
        return !checking.succeeded();
    };
    EXPECT_TRUE(notWritable("    public func bad() {\n        let slot = 0\n"
            "        sink(address: joyeer_intrinsic_place_address<Int>(value: &slot), count: 1)\n    }\n")) << diagnostics();
    EXPECT_TRUE(notWritable("    public func bad(other: Int) {\n"
            "        sink(address: joyeer_intrinsic_place_address<Int>(value: &other), count: 1)\n    }\n")) << diagnostics();
    EXPECT_TRUE(notWritable("    public borrowing func bad() {\n"
            "        sink(address: joyeer_intrinsic_place_address<Int>(value: &self.count), count: 1)\n    }\n")) << diagnostics();
    EXPECT_TRUE(notWritable("    public func bad() {\n"
            "        sink(address: joyeer_intrinsic_place_address<Int>(value: &(1 + 2)), count: 1)\n    }\n")) << diagnostics();
    // The place and the sibling argument may not overlap: the call holds the place inout.
    load("func main() {}\n", withMethods(R"(
    public mutating func overlap() {
        sink(address: joyeer_intrinsic_place_address<Buffer<T>>(value: &self), count: self.count)
    }
)"));
    ASSERT_TRUE(resolution.succeeded()) << diagnostics();
    EXPECT_TRUE(hasTypeDiagnostic(Id::overlappingAccess)) << diagnostics();
    // A disjoint sibling is fine.
    load("func main() {}\n", withMethods(R"(
    public mutating func disjoint(other: Int) {
        sink(address: joyeer_intrinsic_place_address<Buffer<T>>(value: &self), count: other)
    }
)"));
    ASSERT_TRUE(resolution.succeeded()) << diagnostics();
    EXPECT_TRUE(checking.succeeded()) << diagnostics();
}

// `borrow_address<T>(value: place)` (decision 0027 Decision 3): the read-only address of a named readable place,
// including a borrowing parameter, valid for the call it is a direct argument of.
TEST_F(CollectionRoleTest, BorrowAddressIsAuthorizedAndOnlyADirectArgumentOfANamedPlace) {
    load("func main() {\n    let value = 1\n    let address = joyeer_intrinsic_borrow_address<Int>(value: value)\n}\n");
    ASSERT_TRUE(resolution.succeeded()) << diagnostics();
    EXPECT_TRUE(hasTypeDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::unauthorizedIntrinsic))
            << diagnostics();
    const auto withMethods = [&](const std::string& body) {
        return buffer(bufferHead, bufferElement, bufferCopy, bufferEnd, R"(
    deinit() {
        joyeer_intrinsic_destroy_range<T>(address: self.address, count: self.count)
    }
)" + body + R"(
}
private func sink(address: Int, count: Int) {}
private func both(read: Int, write: Int) {}
private func hold(address: Int): Int { return address }
)");
    };
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    // Accepted: a borrowing receiver's field, a borrowing parameter, a `let` and a `var` local.
    load("func main() {}\n", withMethods(R"(
    public borrowing func field() {
        sink(address: joyeer_intrinsic_borrow_address<Int>(value: self.count), count: 1)
    }
    public borrowing func parameter(key: Int) {
        sink(address: joyeer_intrinsic_borrow_address<Int>(value: key), count: 1)
    }
    public func locals() {
        let fixed = 1
        var moving = 2
        sink(address: joyeer_intrinsic_borrow_address<Int>(value: fixed), count: 1)
        sink(address: joyeer_intrinsic_borrow_address<Int>(value: moving), count: 1)
    }
)"));
    ASSERT_TRUE(resolution.succeeded()) << diagnostics();
    EXPECT_TRUE(checking.succeeded()) << diagnostics();
    // Rejected: a stored or computed result, and an rvalue, which has no place.
    const auto rejected = [&](const std::string& form) {
        load("func main() {}\n", withMethods("    public func bad(key: Int) {\n        " + form + "\n    }\n"));
        EXPECT_TRUE(resolution.succeeded()) << diagnostics();
        return hasTypeDiagnostic(Id::invalidCallArguments);
    };
    EXPECT_TRUE(rejected("let address = joyeer_intrinsic_borrow_address<Int>(value: key)")) << diagnostics();
    EXPECT_TRUE(rejected("let again = hold(address: 1) + joyeer_intrinsic_borrow_address<Int>(value: key)"))
            << diagnostics();
    EXPECT_TRUE(rejected("sink(address: joyeer_intrinsic_borrow_address<Int>(value: key + 1), count: 1)"))
            << diagnostics();
    // A read of a place held inout by a sibling argument overlaps.
    load("func main() {}\n", withMethods(R"(
    public mutating func overlap() {
        both(read: joyeer_intrinsic_borrow_address<Int>(value: self.count),
             write: joyeer_intrinsic_place_address<Buffer<T>>(value: &self))
    }
)"));
    ASSERT_TRUE(resolution.succeeded()) << diagnostics();
    EXPECT_TRUE(hasTypeDiagnostic(Id::overlappingAccess)) << diagnostics();
    // It lowers to the address of the place, and the module verifies.
    load("func use(items: Buffer<Int>) { items.peek(key: 3) }\n", withMethods(R"(
    public borrowing func peek(key: Int) {
        sink(address: joyeer_intrinsic_borrow_address<Int>(value: key), count: 1)
    }
)"));
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    EXPECT_NE(joyeer::ir::dump(*module).find("pointer_to_integer"), std::string::npos);
    EXPECT_EQ(verifierMessages(*module).find("ir."), std::string::npos) << verifierMessages(*module);
}

// The place of `borrow_address` and `place_address` (stage 2b review): its type is exactly `T` (no optional
// promotion), it is a local, a parameter or a field path of those (no subscript, computed member, constant or enum
// case), and the address goes to a call that does not store its arguments (no initializer or enum case).
TEST_F(CollectionRoleTest, BorrowAndPlaceAddressNeedTheExactTypeOfANamedLocalPlace) {
    const auto withMethods = [&](const std::string& body) {
        return buffer(bufferHead, bufferElement, bufferCopy, bufferEnd, R"(
    deinit() {
        joyeer_intrinsic_destroy_range<T>(address: self.address, count: self.count)
    }
)" + body + R"(
}
private func sink(address: Int, count: Int) {}
private struct Holder { var address: Int }
private let limit: Int = 5
)");
    };
    const auto rejected = [&](const std::string& form) {
        load("func main() {}\n", withMethods("    public mutating func bad(key: Int, text: String) {\n"
                                             "        var slot = 1\n        " + form + "\n    }\n"));
        EXPECT_TRUE(resolution.succeeded()) << diagnostics();
        return !checking.succeeded();
    };
    // Optional promotion would describe an `Int?` layout over an 8-byte `Int`.
    EXPECT_TRUE(rejected("sink(address: joyeer_intrinsic_borrow_address<Int?>(value: key), count: 1)")) << diagnostics();
    EXPECT_TRUE(rejected("sink(address: joyeer_intrinsic_place_address<Int?>(value: &slot), count: 1)"))
            << diagnostics();
    // A subscript, a computed member, a constant and an enum case are not named places.
    EXPECT_TRUE(rejected("sink(address: joyeer_intrinsic_borrow_address<T>(value: self[0]), count: 1)"))
            << diagnostics();
    EXPECT_TRUE(rejected("sink(address: joyeer_intrinsic_borrow_address<Int>(value: text.count), count: 1)"))
            << diagnostics();
    EXPECT_TRUE(rejected("sink(address: joyeer_intrinsic_borrow_address<Int>(value: limit), count: 1)"))
            << diagnostics();
    // An initializer or enum case stores its arguments, so the address would outlive the call.
    EXPECT_TRUE(rejected("let held = Holder(address: joyeer_intrinsic_borrow_address<Int>(value: key))"))
            << diagnostics();
    EXPECT_TRUE(rejected("let held = Holder(address: joyeer_intrinsic_place_address<Int>(value: &slot))"))
            << diagnostics();
    const auto noKeyKind = [&](const std::string& declaration) {
        load("func main() {}\n", buffer(bufferHead, bufferElement, bufferCopy, bufferEnd, R"(
    deinit() {
        joyeer_intrinsic_destroy_range<T>(address: self.address, count: self.count)
    }
}
)" + declaration));
        EXPECT_TRUE(resolution.succeeded()) << diagnostics();
        return std::count_if(checking.diagnostics.begin(), checking.diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.id == joyeer::typing::TypeCheckingDiagnosticId::invalidTypeArgument;
        });
    };
    // `Never` is reported once, and a type that can never be a key is rejected even in a generic body.
    EXPECT_EQ(noKeyKind("public func bad(): Int { return joyeer_intrinsic_key_kind<Never>() }\n"), 1) << diagnostics();
    EXPECT_EQ(noKeyKind("public func bad<K>(sample: K): Int { return joyeer_intrinsic_key_kind<[K]>() }\n"), 1)
            << diagnostics();
}

// `key_kind<K>()` (decision 0027 Decision 3): the runtime key kind of an admitted key type, a constant per instance;
// any other type is rejected, in the checker when it is concrete and in lowering when an instance substitutes it.
TEST_F(CollectionRoleTest, KeyKindIsAdmittedOnlyForTheFourKeyTypes) {
    const auto withFunction = [&](const std::string& body) {
        return buffer(bufferHead, bufferElement, bufferCopy, bufferEnd, R"(
    deinit() {
        joyeer_intrinsic_destroy_range<T>(address: self.address, count: self.count)
    }
}
)" + body);
    };
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    load("func main() {}\n", withFunction(R"(
public func kinds(): Int {
    return joyeer_intrinsic_key_kind<Int>() * 1000 + joyeer_intrinsic_key_kind<Bool>() * 100 +
            joyeer_intrinsic_key_kind<String>() * 10 + joyeer_intrinsic_key_kind<UInt8>()
}
)"));
    ASSERT_TRUE(resolution.succeeded()) << diagnostics();
    EXPECT_TRUE(checking.succeeded()) << diagnostics();
    load("func main() {}\n", withFunction("public func bad(): Int { return joyeer_intrinsic_key_kind<Double>() }\n"));
    ASSERT_TRUE(resolution.succeeded()) << diagnostics();
    EXPECT_TRUE(hasTypeDiagnostic(Id::invalidTypeArgument)) << diagnostics();
    // A generic body is checked once; the instance for a non-admitted type fails in lowering, never yields 0.
    load("func main() { print(value: kindOf(sample: 1.5)) }\n",
         withFunction("public func kindOf<K>(sample: K): Int { return joyeer_intrinsic_key_kind<K>() }\n"));
    ASSERT_TRUE(checks()) << diagnostics();
    analysis = joyeer::analysis::Analyzer().analyze(checking.model);
    const auto info = [](const SourceFile::Ptr& file) {
        return joyeer::ir::SourceInfo { "fixture.joyeer", ".", file->content.size(), file->lineStarts };
    };
    std::vector<joyeer::ir::SourceInfo> sources { info(userSource), info(roleSource) };
    for (const auto& prelude : checking.model->semanticModel()->preludeSources()) sources.push_back(info(prelude));
    const auto lowered = joyeer::lowering::Lowerer().lower(checking.model, "fixture.joyeer", info(userSource), sources);
    EXPECT_FALSE(lowered.succeeded());
}

TEST_F(CollectionRoleTest, PlaceAddressLowersToTheAddressOfThePlace) {
    const auto library = buffer(bufferHead, bufferElement, bufferCopy, bufferEnd, R"(
    deinit() {
        joyeer_intrinsic_destroy_range<T>(address: self.address, count: self.count)
    }
    public mutating func bump() {
        sink(address: joyeer_intrinsic_place_address<Int>(value: &self.count), count: 1)
    }
}
private func sink(address: Int, count: Int) {}
)");
    load("func use(items: inout Buffer<Int>) { &items.bump() }\n", library);
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    const auto dumped = joyeer::ir::dump(*module);
    EXPECT_NE(dumped.find("pointer_to_integer"), std::string::npos) << dumped;
    EXPECT_EQ(verifierMessages(*module).find("ir."), std::string::npos) << verifierMessages(*module);
}

TEST_F(CollectionRoleTest, TheArrayLiteralRoleHasAClosedShapeAndIsRequired) {
    const auto head = std::string(bufferHead);
    const auto element = std::string(bufferElement);
    const auto copy = std::string(bufferCopy);
    const auto end = std::string(bufferEnd);
    const auto tail = std::string(bufferTail);
    const auto literal = std::string(bufferLiteral);
    const auto replaceOnce = [](std::string text, const std::string& from, const std::string& to) {
        const auto at = text.find(from);
        EXPECT_NE(at, std::string::npos) << from;
        if (at != std::string::npos) text.replace(at, from.size(), to);
        return text;
    };
    // Required, and unique.
    EXPECT_TRUE(rejectedAsInvalidPrelude(buffer(head, element, copy, end, tail, ""), "array_literal"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(buffer(head, element, copy, end, tail, literal + R"(
    @compiler_role(array_literal)
    public consuming func again(address: Int, count: Int): Buffer<T> {
        return Buffer(address: address, count: count, capacity: count)
    }
)"), "array_literal"));
    // Shape: consuming, the labels address and count of Int, a result of the declaring type.
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            buffer(head, element, copy, end, tail, replaceOnce(literal, "consuming func", "borrowing func")),
            "array_literal"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            buffer(head, element, copy, end, tail, replaceOnce(literal, "private consuming", "public consuming")),
            "array_literal"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            buffer(head, element, copy, end, tail, replaceOnce(literal, "address: Int, count: Int", "count: Int, address: Int")),
            "array_literal"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            buffer(head, element, copy, end, tail, replaceOnce(literal, "(address: Int, count: Int)", "(address: Int)")),
            "array_literal"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            buffer(head, element, copy, end, tail, replaceOnce(literal, "count: Int): Buffer<T>", "count: String): Buffer<T>")),
            "array_literal"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            buffer(head, element, copy, end, tail, R"(
    @compiler_role(array_literal)
    public consuming func literal(address: Int, count: Int): Int { return 0 }
)"), "array_literal"));
    // A free function with the role is a misplaced role.
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            buffer() + "\n@compiler_role(array_literal)\npublic func loose(address: Int, count: Int): Int { return 0 }\n",
            "array_literal"));
}

TEST_F(CollectionRoleTest, TheLiteralMethodMayLeaveItsZeroSelfUnconsumedButNoOtherConsumingMethodMay) {
    load("func main() {}\n");
    EXPECT_EQ(analyze(), "") << "the literal method's self is an inert zero value";
    // Another consuming method of the declaration still has to consume self.
    const auto extra = std::string(R"(
    public consuming func leak() {}
)");
    load("func main() {}\n", buffer(bufferHead, bufferElement, bufferCopy, bufferEnd, bufferTail,
                                    std::string(bufferLiteral) + extra));
    EXPECT_NE(analyze().find("linear-not-consumed"), std::string::npos);
}

TEST_F(CollectionRoleTest, ALibraryDeinitCannotOverwriteItsWholeSelf) {
    load("func main() {}\n", buffer(bufferHead, bufferElement, bufferCopy, bufferEnd, R"(
    deinit() {
        &self = Buffer(address: 0, count: 0, capacity: 0)
    }
}
)"));
    EXPECT_TRUE(hasTypeDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::assignmentToSelfInDeinit)) << diagnostics();
}

TEST_F(CollectionRoleTest, OwnedElementArgumentsCopyUnmarkedPlacesAndMoveExplicitly) {
    const auto library = buffer(bufferHead, bufferElement, bufferCopy, bufferEnd, R"(
    public mutating func put(value: consuming T) {
        joyeer_intrinsic_move_in<T>(address: self.address, value: consume value)
    }
    deinit() {
        joyeer_intrinsic_destroy_range<T>(address: self.address, count: self.count)
    }
}
)");
    load(R"(
func use(items: inout Buffer<String>, text: consuming String) {
    &items.put(value: "first")
    &items.put(value: text)
    print(value: text)
    &items.put(value: consume text)
}
)", library);
    ASSERT_TRUE(checks()) << diagnostics();
    EXPECT_EQ(analyze(), "");
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    const auto wrapper = std::find_if(module->functions.begin(), module->functions.end(), [](const auto& function) {
        return function.name.ends_with(".put");
    });
    ASSERT_NE(wrapper, module->functions.end());
    EXPECT_TRUE(wrapper->alwaysInline);
    for (const bool debug : { false, true }) {
        joyeer::llvmbackend::EmitOptions options;
        options.emitLineTables = debug;
        options.emitVariables = debug;
        const auto emitted = joyeer::llvmbackend::Emitter().emit(*module, options);
        EXPECT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
    }

    load(R"(
struct Resource { var id: Int
    deinit() {}
}
func use(items: inout Buffer<Resource>, value: consuming Resource) {
    &items.put(value: value)
}
)", library);
    EXPECT_TRUE(hasTypeDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::copyOfNoncopyable)) << diagnostics();

    load("func use(items: inout Buffer<Int>, value: inout Int) { &items.put(value: &value) }\n", library);
    EXPECT_TRUE(hasTypeDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::invalidConsumeArgument)) << diagnostics();
}

TEST_F(CollectionRoleTest, OwnedElementArgumentClassificationPrecedesSubstitution) {
    const auto library = buffer(bufferHead, bufferElement, bufferCopy, bufferEnd, R"(
    public mutating func put(value: consuming T, number: consuming Int) {
        joyeer_intrinsic_move_in<T>(address: self.address, value: consume value)
        print(value: number)
    }
    deinit() {
        joyeer_intrinsic_destroy_range<T>(address: self.address, count: self.count)
    }
}
)");
    load("func use(items: inout Buffer<Int>) { &items.put(value: 1, number: 2) }\n", library);
    ASSERT_TRUE(resolution.succeeded()) << diagnostics();
    EXPECT_EQ(std::count_if(checking.diagnostics.begin(), checking.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.id == joyeer::typing::TypeCheckingDiagnosticId::invalidConsumeArgument;
    }), 1);
    load("func use(items: inout Buffer<Int>) { &items.put(value: 1, number: consume 2) }\n", library);
    EXPECT_TRUE(checks()) << diagnostics();
}

TEST_F(CollectionRoleTest, LiteralStorageIsAClosedBoundedInitializeAndTransferProtocol) {
    load("func make(): [Int] { return [1, 2] }\n");
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    ASSERT_TRUE(joyeer::ir::Verifier().verify(*module).succeeded());
    using joyeer::ir::Opcode;
    for (unsigned mutation = 0; mutation < 9; ++mutation) {
        SCOPED_TRACE(mutation);
        auto copy = *module;
        auto& function = named(copy, "make");
        auto& instructions = function.blocks[0].instructions;
        const auto first = [&](Opcode opcode) -> joyeer::ir::Instruction& {
            return *std::find_if(instructions.begin(), instructions.end(), [opcode](const auto& instruction) {
                return instruction.opcode == opcode;
            });
        };
        auto& storage = first(Opcode::stackBlock);
        auto& slot = first(Opcode::slotAddress);
        const auto definition = [&](joyeer::ir::ValueId id) -> joyeer::ir::Instruction& {
            return *std::find_if(instructions.begin(), instructions.end(), [id](const auto& instruction) {
                return instruction.result.has_value() && instruction.result->id == id;
            });
        };
        switch (mutation) {
            case 0:
                definition(storage.operands[0]).integerValue = 0;
                break;
            case 1:
                definition(slot.operands[1]).integerValue = 2;
                break;
            case 2: {
                const auto second = std::find_if(instructions.begin(), instructions.end(), [&](const auto& instruction) {
                    return instruction.opcode == Opcode::slotAddress && instruction.result != slot.result;
                });
                second->operands[1] = slot.operands[1];
                break;
            }
            case 3:
                slot.integerValue = checking.model->types().stringType();
                break;
            case 4:
                instructions.erase(std::find_if(instructions.begin(), instructions.end(), [](const auto& instruction) {
                    return instruction.opcode == Opcode::store;
                }));
                break;
            case 5:
                first(Opcode::call).operands[1] = storage.operands[0];
                break;
            case 6:
                first(Opcode::integerAddress).opcode = Opcode::pointerToInteger;
                break;
            case 7: {
                auto duplicate = first(Opcode::call);
                duplicate.result->id = 9000;
                instructions.insert(std::find_if(instructions.begin(), instructions.end(), [](const auto& instruction) {
                    return instruction.opcode == Opcode::call;
                }), duplicate);
                break;
            }
            case 8:
                copy.functions[*first(Opcode::call).callee].literalFactory = false;
                break;
        }
        EXPECT_FALSE(joyeer::ir::Verifier().verify(copy).succeeded());
    }
}

// An element is evaluated before the storage: in its block or in a block that dominates the storage's block. One
// evaluated on one arm of a diamond whose join holds the storage is rejected; one in a dominating block is accepted.
TEST_F(CollectionRoleTest, ALiteralElementMustBeEvaluatedInABlockThatDominatesTheStorage) {
    load("func make(): [Int] { return [1, 2] }\n");
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    ASSERT_TRUE(joyeer::ir::Verifier().verify(*module).succeeded());
    using joyeer::ir::Opcode;
    using Body = std::vector<joyeer::ir::Instruction>;
    const auto& original = named(*module, "make");
    ASSERT_EQ(original.blocks.size(), 1u);
    const auto& instructions = original.blocks[0].instructions;
    const auto storage = std::find_if(instructions.begin(), instructions.end(), [](const auto& instruction) {
        return instruction.opcode == Opcode::stackBlock;
    });
    const auto store = std::find_if(instructions.begin(), instructions.end(), [](const auto& instruction) {
        return instruction.opcode == Opcode::store;
    });
    ASSERT_NE(storage, instructions.end());
    ASSERT_NE(store, instructions.end());
    const auto element = store->operands[0];
    Body before;
    Body definition;
    for (auto instruction = instructions.begin(); instruction != storage; ++instruction) {
        const bool definesElement = instruction->result.has_value() && instruction->result->id == element;
        (definesElement ? definition : before).push_back(*instruction);
    }
    ASSERT_EQ(definition.size(), 1u);
    const Body rest(storage, instructions.end());
    const auto branch = [](joyeer::ir::BlockId target) {
        joyeer::ir::Instruction instruction { Opcode::branch };
        instruction.targets = { target };
        return instruction;
    };
    joyeer::ir::Instruction condition { Opcode::literal };
    condition.integerValue = 1;
    condition.result = joyeer::ir::Value { 9100, checking.model->types().boolType(), joyeer::ir::ValueCategory::value };
    joyeer::ir::Instruction choose { Opcode::conditionalBranch };
    choose.operands = { 9100 };
    choose.targets = { 1, 2 };
    const auto join = [](Body first, const Body& second) {
        first.insert(first.end(), second.begin(), second.end());
        return first;
    };
    const auto verify = [&](const std::vector<Body>& blocks) {
        auto copy = *module;
        auto& function = named(copy, "make");
        function.blocks.clear();
        for (size_t index = 0; index < blocks.size(); ++index) {
            function.blocks.push_back(joyeer::ir::BasicBlock { static_cast<joyeer::ir::BlockId>(index),
                                                               "b" + std::to_string(index), blocks[index] });
        }
        return joyeer::ir::Verifier().verify(copy);
    };
    // The rest of the code before the storage (the zeroed receiver among it) stays in the storage's block.
    const auto storageBlock = join(before, rest);
    const auto diamond = verify({ { condition, choose }, join(definition, { branch(3) }), { branch(3) },
                                  storageBlock });
    EXPECT_FALSE(diamond.succeeded());
    EXPECT_NE(joyeer::ir::dump(diamond).find("array literal storage"), std::string::npos) << joyeer::ir::dump(diamond);
    const auto dominated = verify({ join(definition, { branch(1) }), storageBlock });
    EXPECT_TRUE(dominated.succeeded()) << joyeer::ir::dump(dominated);
}

TEST_F(CollectionRoleTest, AFactoryCallWithoutLiteralStorageIsRejected) {
    load("func make(): [Int] { return [1, 2] }\n");
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    auto copy = *module;
    auto& function = named(copy, "make");
    auto& instructions = function.blocks[0].instructions;
    const auto factoryCall = std::find_if(instructions.begin(), instructions.end(), [](const auto& instruction) {
        return instruction.opcode == joyeer::ir::Opcode::call;
    });
    const auto count = factoryCall->operands[2];
    auto call = *factoryCall;
    call.operands[1] = count;
    call.result->id = 9000;
    instructions.insert(instructions.end() - 1, call);
    EXPECT_FALSE(joyeer::ir::Verifier().verify(copy).succeeded());
}

TEST_F(CollectionRoleTest, AFactoryFunctionValueWithoutLiteralStorageIsRejected) {
    load("func make(): [Int] { return [1, 2] }\n");
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    auto copy = *module;
    auto& function = named(copy, "make");
    auto& instructions = function.blocks[0].instructions;
    auto call = *std::find_if(instructions.begin(), instructions.end(), [](const auto& instruction) {
        return instruction.opcode == joyeer::ir::Opcode::call;
    });
    const auto factory = *call.callee;
    const auto functionType = static_cast<joyeer::ir::TypeId>(copy.types.size());
    const auto integer = checking.model->types().intType();
    copy.types.push_back({ functionType, "literal factory function", joyeer::typing::TypeKind::function,
                           joyeer::semantic::invalidSymbolId,
                           { call.result->type, integer, integer, call.result->type },
                           { joyeer::syntax::AccessEffect::consuming, joyeer::syntax::AccessEffect::borrowing,
                             joyeer::syntax::AccessEffect::borrowing } });
    const auto receiver = call.operands[0];
    const auto count = call.operands[2];
    instructions.erase(std::find_if(instructions.begin(), instructions.end(), [](const auto& instruction) {
        return instruction.opcode == joyeer::ir::Opcode::stackBlock;
    }), instructions.end());
    joyeer::ir::Instruction address { joyeer::ir::Opcode::functionAddress };
    address.callee = factory;
    address.result = joyeer::ir::Value { 9000, functionType, joyeer::ir::ValueCategory::value };
    instructions.push_back(address);
    call.opcode = joyeer::ir::Opcode::callIndirect;
    call.callee.reset();
    call.operands = { 9000, receiver, count, count };
    instructions.push_back(call);
    instructions.push_back({ joyeer::ir::Opcode::ret, std::nullopt, { call.result->id } });
    const auto verification = joyeer::ir::Verifier().verify(copy);
    EXPECT_FALSE(verification.succeeded());
    EXPECT_NE(joyeer::ir::dump(verification).find("literal factories cannot be formed as function values"),
              std::string::npos);
}

TEST_F(CollectionRoleTest, WireRejectsFullWidthValuesBeforeNarrowingRoles) {
    for (const uint64_t role : { uint64_t { 256 }, uint64_t { 257 }, UINT64_MAX }) {
        joyeer::ir::StructureDefinition structure;
        joyeer::llvmbackend::wire::Writer writer;
        writer(structure.symbol, structure.type, structure.name, structure.fields, structure.deinitializer,
               role, structure.copier);
        joyeer::ir::StructureDefinition decoded;
        joyeer::llvmbackend::wire::Reader reader(writer.bytes);
        reader(decoded);
        EXPECT_FALSE(reader.succeeded()) << role;
    }
    joyeer::ir::StructureDefinition structure;
    structure.role = joyeer::ir::StructureRole::array;
    joyeer::llvmbackend::wire::Writer writer;
    writer(structure);
    joyeer::ir::StructureDefinition decoded;
    joyeer::llvmbackend::wire::Reader reader(writer.bytes);
    reader(decoded);
    EXPECT_TRUE(reader.succeeded());
    EXPECT_EQ(decoded.role, structure.role);
}

TEST_F(CollectionRoleTest, TheVerifierRejectsForgedArrayRolesAndLiteralFactories) {
    load("func make(): [Int] { return [1] }\n");
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    using joyeer::ir::StructureRole;
    for (unsigned mutation = 0; mutation < 9; ++mutation) {
        SCOPED_TRACE(mutation);
        auto copy = *module;
        auto structure = std::find_if(copy.structures.begin(), copy.structures.end(), [](const auto& value) {
            return value.role == StructureRole::array;
        });
        ASSERT_NE(structure, copy.structures.end());
        auto factory = std::find_if(copy.functions.begin(), copy.functions.end(), [](const auto& value) {
            return value.literalFactory;
        });
        ASSERT_NE(factory, copy.functions.end());
        switch (mutation) {
            case 0: structure->role = static_cast<StructureRole>(255); break;
            case 1: structure->fields.pop_back(); break;
            case 2: structure->symbol = joyeer::semantic::invalidSymbolId; break;
            case 3: factory->runtimeContext = false; break;
            case 4: factory->parameters[0].isConsuming = false; break;
            case 5: factory->parameters[1].value.category = joyeer::ir::ValueCategory::address; break;
            case 6: factory->resultType = checking.model->types().intType(); break;
            // The array role needs exactly one type argument (its element), checked before any rule that reads it.
            case 7:
            case 8: {
                auto type = std::find_if(copy.types.begin(), copy.types.end(), [&](const auto& value) {
                    return value.id == structure->type;
                });
                ASSERT_NE(type, copy.types.end());
                if (mutation == 7) type->arguments.push_back(checking.model->types().intType());
                else type->arguments.clear();
                break;
            }
        }
        EXPECT_FALSE(joyeer::ir::Verifier().verify(copy).succeeded());
    }
    joyeer::llvmbackend::wire::Writer writer;
    writer(*module);
    joyeer::ir::Module decoded;
    joyeer::llvmbackend::wire::Reader reader(writer.bytes);
    reader(decoded);
    ASSERT_TRUE(reader.succeeded());
    EXPECT_EQ(joyeer::ir::dump(decoded), joyeer::ir::dump(*module));
}

TEST_F(CollectionRoleTest, AThousandElementLiteralLowersAndEmitsInBothProfiles) {
    std::string program = "func make(): [Int] { return [";
    for (unsigned index = 0; index < 1000; ++index) {
        if (index != 0) program += ", ";
        program += std::to_string(index);
    }
    program += "] }\n";
    load(program);
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    ASSERT_TRUE(joyeer::ir::Verifier().verify(*module).succeeded());
    for (const auto level : { joyeer::OptimizationLevel::O0, joyeer::OptimizationLevel::O2 }) {
        joyeer::llvmbackend::EmitOptions options;
        options.optimizationLevel = level;
        const auto emitted = joyeer::llvmbackend::Emitter().emit(*module, options);
        EXPECT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
    }
}

TEST_F(CollectionRoleTest, NeverIsNotAnElementOfTheRoleType) {
    load("func bottom(items: Buffer<Never>) {}\n");
    EXPECT_FALSE(checks());
    EXPECT_NE(diagnostics().find("invalid-type-argument"), std::string::npos) << diagnostics();
}

TEST_F(CollectionRoleTest, UnitElementSignaturesKeepTheirLayout) {
    load("func unit(items: Buffer<Void>) {}\n");
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    EXPECT_TRUE(joyeer::ir::Verifier().verify(*module).succeeded()) << verifierMessages(*module);
    const auto emitted = joyeer::llvmbackend::Emitter().emit(*module);
    EXPECT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
}

TEST_F(CollectionRoleTest, ACallWithADivergingElementDoesNotNeedAnInstance) {
    const auto library = buffer(bufferHead, bufferElement, bufferCopy, bufferEnd, R"(
    public mutating func append(element: consuming T) {
        joyeer_intrinsic_move_in<T>(address: self.address, value: consume element)
    }
    deinit() {
        joyeer_intrinsic_destroy_range<T>(address: self.address, count: self.count)
    }
}
)");
    load("func main() { var values: [Int] = []\n &values.append(element: return) }\n", library);
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    EXPECT_TRUE(joyeer::ir::Verifier().verify(*module).succeeded()) << verifierMessages(*module);
    EXPECT_TRUE(std::none_of(module->functions.begin(), module->functions.end(), [](const auto& function) {
        return function.name.ends_with(".append");
    }));
}

TEST_F(CollectionRoleTest, DivergingGenericCallsStillCheckTheirTypeArguments) {
    load(R"(
struct Resource { var id: Int
    deinit() {}
}
func accept<U: Copyable>(value: U, sample: U) {}
func stop() { accept(value: return, sample: Resource(id: 1)) }
)");
    EXPECT_NE(diagnostics().find("unsatisfied-constraint"), std::string::npos) << diagnostics();
    load(R"(
func accept<U>(value: U, sample: U) {}
func stop(handle: consuming JoinHandle<Int>) { accept(value: return, sample: handle) }
)");
    EXPECT_NE(diagnostics().find("linear-type-argument"), std::string::npos) << diagnostics();
}

TEST_F(CollectionRoleTest, DivergingCallsKeepExistingInstancesAndCallerSubstitutions) {
    load(R"(
struct Resource { var id: Int
    deinit() {}
}
func accept<U: Copyable>(owned: consuming Resource, value: U, sample: U) {}
func caller<V: Copyable>(sample: V) {
    accept(owned: consume Resource(id: 12), value: return, sample: sample)
}
func main() {
    accept(owned: consume Resource(id: 10), value: 1, sample: 2)
    caller(sample: "sample")
    accept(owned: consume Resource(id: 11), value: return, sample: 3)
}
)");
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    EXPECT_TRUE(joyeer::ir::Verifier().verify(*module).succeeded()) << verifierMessages(*module);
    EXPECT_EQ(std::count_if(module->functions.begin(), module->functions.end(), [](const auto& function) {
        return function.name.ends_with("accept<Int>");
    }), 1);
    EXPECT_EQ(std::count_if(module->functions.begin(), module->functions.end(), [](const auto& function) {
        return function.name.ends_with("caller<String>");
    }), 1);
    EXPECT_TRUE(std::none_of(module->functions.begin(), module->functions.end(), [](const auto& function) {
        return function.name.ends_with("accept<String>");
    }));
}

TEST_F(CollectionRoleTest, OwnershipHelpersHaveArtificialDebugScopesForInlining) {
    load("func count(items: Buffer<Int>): Int { return items.count }\n");
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    joyeer::llvmbackend::EmitOptions options;
    options.emitLineTables = true;
    options.emitVariables = true;
    options.optimizationLevel = joyeer::OptimizationLevel::O2;
    const auto emitted = joyeer::llvmbackend::Emitter().emit(*module, options);
    ASSERT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
    std::istringstream lines(emitted.text);
    std::string line;
    size_t helpers = 0;
    while (std::getline(lines, line)) {
        if (!line.starts_with("define internal void @joyeer_clone_type_") &&
            !line.starts_with("define internal void @joyeer_destroy_type_")) continue;
        ++helpers;
        EXPECT_NE(line.find("!dbg"), std::string::npos) << line;
    }
    EXPECT_GT(helpers, 0u);
    EXPECT_NE(emitted.text.find("DIFlagArtificial"), std::string::npos);
    EXPECT_EQ(emitted.text.find("noinline"), std::string::npos);
}

TEST_F(CollectionRoleTest, ExpandingRecursionThroughARoleArrayIsRejected) {
    load("struct Recursive<T> { var next: Buffer<Recursive<[T]>> }\nfunc main() {}\n");
    ASSERT_TRUE(resolution.succeeded()) << diagnostics();
    EXPECT_TRUE(hasTypeDiagnostic(joyeer::typing::TypeCheckingDiagnosticId::expandingGenericRecursion))
            << diagnostics();
}

} // namespace

namespace {

// The `array_equal` role (decision 0031) is `private func name<T: Equatable>(left: [T], right: [T]): Bool` with a
// body, declared once (plan 0012 phase 2).
std::string arrayEqualDeclaration(const std::string& head, const std::string& body = "return true") {
    return "\n@compiler_role(array_equal)\n" + head + " {\n    " + body + "\n}\n";
}

TEST_F(CollectionRoleTest, TheArrayEqualRoleShapeIsChecked) {
    const std::string good = "private func same<T: Equatable>(left: [T], right: [T]): Bool";
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            buffer() + arrayEqualDeclaration("private func same<T>(left: [T], right: [T]): Bool"), "array_equal"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            buffer() + arrayEqualDeclaration("private func same<T: Equatable>(a: [T], b: [T]): Bool"), "array_equal"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            buffer() + arrayEqualDeclaration("private func same<T: Equatable>(left: [T], right: T): Bool"),
            "array_equal"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            buffer() + arrayEqualDeclaration("private func same<T: Equatable>(left: [T], right: [T]): Int", "return 0"),
            "array_equal"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            buffer() + arrayEqualDeclaration("public func same<T: Equatable>(left: [T], right: [T]): Bool"),
            "array_equal"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            buffer() + arrayEqualDeclaration(good) +
                    arrayEqualDeclaration("private func other<T: Equatable>(left: [T], right: [T]): Bool"),
            "second"));
    EXPECT_FALSE(rejectedAsInvalidPrelude(buffer() + arrayEqualDeclaration(good), "array_equal"));
}

}  // namespace
