// Decision 0027 stage 5: the dictionary roles (`dict_type`, its five method roles and `dict_entry`) and their
// closed shapes. `[K: V]` and dictionary literals stay builtin until the switch, so the roles are exercised through
// a role-bound `Table<K, V>` supplied with the library array through the collections-prelude test seam.
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
#include <functional>
#include <string>

namespace {

// A library array, so the replaced collections prelude still declares the array role.
constexpr auto arrayLibrary = R"(
@compiler_role(array_type)
public struct Array<T> {
    private var address: Int
    private var count: Int
    private var capacity: Int

    @compiler_role(array_element)
    private borrowing func element(index: Int): Int {
        return joyeer_intrinsic_slot_address<T>(base: self.address, index: index)
    }

    @compiler_role(array_copy)
    private borrowing func duplicate(): Array<T> {
        return joyeer_intrinsic_move_out<Array<T>>(address: self.address)
    }

    @compiler_role(container_end)
    public consuming func consumeEmpty(): Void {}

    @compiler_role(array_literal)
    private consuming func literal(address: Int, count: Int): Array<T> {
        return joyeer_intrinsic_move_out<Array<T>>(address: address)
    }

    deinit() {
        joyeer_intrinsic_destroy_range<T>(address: self.address, count: self.count)
    }
}
)";

using namespace joyeer::fixtures;

// The pieces of the table library, each replaceable by a malformed test.
struct TableParts {
    std::string entry = tableEntry;
    std::string head = tableHead;
    std::string element = tableElement;
    std::string set = tableSet;
    std::string get = tableGet;
    std::string copy = tableCopy;
    std::string literal = tableLiteral;
    std::string tail = tableTail;
    std::string extra;

    [[nodiscard]] std::string text() const {
        return std::string(arrayLibrary) + tableExterns + entry + head + element + set + get + copy + literal + tail +
               extra;
    }
};

std::string table() { return TableParts().text(); }

std::string replaced(std::string text, const std::string& from, const std::string& to) {
    const auto at = text.find(from);
    EXPECT_NE(at, std::string::npos) << from;
    if (at != std::string::npos) text.replace(at, from.size(), to);
    return text;
}

class DictRoleTest : public testing::Test {
protected:
    // Resolves and type checks `user` with `library` replacing the collections prelude.
    void load(const std::string& user, const std::string& library = table()) {
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

    // The library is rejected by the type checker's role validation with a message mentioning `needle`.
    bool rejectedAsInvalidPrelude(const std::string& library, const std::string& needle) {
        load("func main() {}\n", library);
        if (!resolution.succeeded()) return false;
        return std::any_of(checking.diagnostics.begin(), checking.diagnostics.end(), [&](const auto& diagnostic) {
            return diagnostic.id == joyeer::typing::TypeCheckingDiagnosticId::invalidPrelude &&
                   diagnostic.message.find(needle) != std::string::npos;
        });
    }

    // The program is rejected with a diagnostic that mentions `needle`, while the library itself loads.
    bool rejectedMentioning(const std::string& user, const std::string& needle) {
        load(user);
        if (diagnostics().find("invalid-prelude") != std::string::npos) return false;
        return diagnostics().find(needle) != std::string::npos;
    }

    bool hasTypeDiagnostic(joyeer::typing::TypeCheckingDiagnosticId id) const {
        return std::any_of(checking.diagnostics.begin(), checking.diagnostics.end(),
                           [id](const auto& diagnostic) { return diagnostic.id == id; });
    }

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
        auto result = joyeer::lowering::Lowerer().lower(checking.model, "fixture.joyeer", info(userSource), sources);
        EXPECT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
        return result.module;
    }

    SourceFile::Ptr userSource;
    SourceFile::Ptr roleSource;
    joyeer::semantic::NameResolutionResult resolution;
    joyeer::typing::TypeCheckingResult checking;
    joyeer::analysis::Result analysis;
};

// ---- A well-shaped declaration ----

TEST_F(DictRoleTest, AWellShapedDictRoleIsAccepted) {
    load("func main() {}\n");
    ASSERT_TRUE(resolution.succeeded()) << diagnostics();
    EXPECT_TRUE(checking.succeeded()) << diagnostics();
    // The role type is an ordinary nominal type to user code, with `count` and `get` its only visible members.
    load("func size(table: Table<Int, String>): Int { return table.count }\n"
         "func lookup(table: Table<String, Int>): Int? { return table.get(key: \"a\") }\n"
         "func nested(table: Table<Int, Table<String, [Int]>>): Int { return table.count }\n");
    EXPECT_TRUE(checks()) << diagnostics();
}

TEST_F(DictRoleTest, TheLiteralMethodMayLeaveItsZeroSelfUnconsumed) {
    load("func main() {}\n");
    EXPECT_EQ(analyze(), "") << "the dict_literal method's self is an inert zero value";
    // The role owns no linear key or value, so the table is affine and any consuming method may drop its self
    // (unlike an array of a linear element, decision 0026).
    TableParts parts;
    parts.tail = replaced(parts.tail, "deinit()", "public consuming func drop() {}\n    deinit()");
    load("func main() {}\n", parts.text());
    EXPECT_EQ(analyze(), "");
}

// ---- dict_type ----

TEST_F(DictRoleTest, EachMalformedDictTypeShapeIsRejected) {
    const auto withHead = [](const std::string& head) {
        TableParts parts;
        parts.head = head;
        return parts.text();
    };
    const auto withFields = [&](const std::string& fields) {
        return withHead("\n@compiler_role(dict_type)\npublic struct Table<K, V> {\n" + fields);
    };
    // The fields are the private Int words address, count and capacity, in that order.
    EXPECT_TRUE(rejectedAsInvalidPrelude(withFields(
            "private var count: Int\nprivate var address: Int\nprivate var capacity: Int\n"), "dict_type"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(withFields("private var address: Int\nprivate var count: Int\n"),
                                         "dict_type"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(withFields(
            "private var address: Int\nprivate var count: Int\nprivate var capacity: Int\nprivate var extra: Int\n"),
            "dict_type"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(withFields(
            "private var address: Int\nprivate var count: String\nprivate var capacity: Int\n"), "dict_type"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(withFields(
            "public var address: Int\nprivate var count: Int\nprivate var capacity: Int\n"), "dict_type"));
    // Exactly two unconstrained type parameters.
    // (The struct's own rule: with a third parameter every method role is also rejected, so match its text.)
    EXPECT_TRUE(rejectedAsInvalidPrelude(withHead(replaced(tableHead, "Table<K, V>", "Table<K, V: Copyable>")),
                                         "exactly two unconstrained type parameters"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(withHead(replaced(tableHead, "Table<K, V>", "Table<K, V, W>")),
                                         "exactly two unconstrained type parameters"));
    // A public struct with a deinit.
    EXPECT_TRUE(rejectedAsInvalidPrelude(withHead(replaced(tableHead, "public struct", "internal struct")),
                                         "public"));
    {
        TableParts parts;
        parts.tail = "}\n";
        EXPECT_TRUE(rejectedAsInvalidPrelude(parts.text(), "deinit"));
    }
    // The role belongs on a struct, and only one.
    {
        TableParts parts;
        parts.extra = "\n@compiler_role(dict_type)\npublic func wrong(): Int { return 0 }\n";
        EXPECT_TRUE(rejectedAsInvalidPrelude(parts.text(), "dict_type"));
    }
    {
        TableParts parts;
        parts.extra = "\n@compiler_role(dict_type)\npublic struct Other<K, V> {\n"
                      "private var address: Int\nprivate var count: Int\nprivate var capacity: Int\n}\n";
        EXPECT_TRUE(rejectedAsInvalidPrelude(parts.text(), "second"));
    }
}

TEST_F(DictRoleTest, EveryDictMethodRoleIsRequiredExactlyOnce) {
    const auto without = [](std::string TableParts::*part) {
        TableParts parts;
        parts.*part = "";
        return parts.text();
    };
    EXPECT_TRUE(rejectedAsInvalidPrelude(without(&TableParts::element), "dict_element"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(without(&TableParts::set), "dict_set"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(without(&TableParts::get), "dict_get"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(without(&TableParts::copy), "dict_copy"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(without(&TableParts::literal), "dict_literal"));
    const auto twice = [](std::string TableParts::*part, const std::string& name, const std::string& rename) {
        TableParts parts;
        parts.*part += replaced(parts.*part, name, rename);
        return parts.text();
    };
    EXPECT_TRUE(rejectedAsInvalidPrelude(twice(&TableParts::element, "func element", "func again"), "dict_element"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(twice(&TableParts::set, "func insert", "func again"), "dict_set"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(twice(&TableParts::copy, "func duplicate", "func again"), "dict_copy"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(twice(&TableParts::literal, "func literal", "func again"), "dict_literal"));
}

TEST_F(DictRoleTest, EachMalformedDictMethodRoleIsRejected) {
    const auto with = [](std::string TableParts::*part, const std::string& from, const std::string& to) {
        TableParts parts;
        parts.*part = replaced(parts.*part, from, to);
        return parts.text();
    };
    // dict_element: `private borrowing func name(key: K): Int`.
    const auto element = &TableParts::element;
    EXPECT_TRUE(rejectedAsInvalidPrelude(with(element, "borrowing func element", "consuming func element"),
                                         "dict_element"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(with(element, "private borrowing", "public borrowing"), "dict_element"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(with(element, "element(key: K)", "element(at key: K)"), "dict_element"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(with(element, "element(key: K)", "element(index key: K)"), "dict_element"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(with(element, "element(key: K)", "element(key: V)"), "dict_element"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(with(element, "element(key: K)", "element(key: consuming K)"),
                                         "dict_element"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(with(element, "element(key: K): Int", "element(key: K, extra: Int): Int"),
                                         "dict_element"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            with(element, "element(key: K): Int {", "element(key: K): Int32 { return 0\n"), "dict_element"));
    // dict_set: `private mutating func name(key: consuming K, value: consuming V): Void`.
    const auto set = &TableParts::set;
    EXPECT_TRUE(rejectedAsInvalidPrelude(with(set, "private mutating", "public mutating"), "dict_set"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(with(set, "private mutating func", "private borrowing func"), "dict_set"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            with(set, "insert(key: consuming K, value: consuming V)", "insert(key: consuming K, item value: consuming V)"),
            "dict_set"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            with(set, "insert(key: consuming K, value: consuming V)", "insert(value: consuming K, key: consuming V)"),
            "dict_set"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            with(set, "insert(key: consuming K, value: consuming V)", "insert(key: consuming V, value: consuming K)"),
            "dict_set"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            with(set, "insert(key: consuming K, value: consuming V)", "insert(key: K, value: consuming V)"),
            "dict_set"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(with(set, "value: consuming V): Void {", "value: consuming V): Int {\n"
                                                                                  "return 0\n"),
                                         "dict_set"));
    // dict_get: the one public method role, `public borrowing func get(key: K): Optional<V>`.
    const auto get = &TableParts::get;
    EXPECT_TRUE(rejectedAsInvalidPrelude(with(get, "public borrowing", "private borrowing"), "dict_get"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(with(get, "public borrowing", "public consuming"), "dict_get"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(with(get, "get(key: K)", "get(at key: K)"), "dict_get"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(with(get, "get(key: K)", "get(key: consuming K)"), "dict_get"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(with(get, "get(key: K): Optional<V> {", "get(key: K): Optional<K> {\n"
                                                                                 "return .None\n"),
                                         "dict_get"));
    // dict_copy: `private borrowing func name(): Self`.
    const auto copy = &TableParts::copy;
    EXPECT_TRUE(rejectedAsInvalidPrelude(with(copy, "private borrowing", "public borrowing"), "dict_copy"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(with(copy, "private borrowing", "private consuming"), "dict_copy"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            with(copy, "duplicate(): Table<K, V> {", "duplicate(): Table<V, K> {\n"
                                                     "return joyeer_intrinsic_move_out<Table<V, K>>(address: 0)\n"),
            "dict_copy"));
    // dict_literal: `private consuming func name(address: Int, count: Int): Self`.
    const auto literal = &TableParts::literal;
    EXPECT_TRUE(rejectedAsInvalidPrelude(with(literal, "private consuming", "public consuming"), "dict_literal"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(with(literal, "private consuming", "private borrowing"), "dict_literal"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            with(literal, "literal(address: Int, count: Int)", "literal(count: Int, address: Int)"), "dict_literal"));
    // A method role on a free function, and a role of the other collection.
    {
        TableParts parts;
        parts.extra = "\n@compiler_role(dict_element)\npublic func loose(key: Int): Int { return key }\n";
        EXPECT_TRUE(rejectedAsInvalidPrelude(parts.text(), "dict_element"));
    }
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            with(element, "@compiler_role(dict_element)", "@compiler_role(array_element)"), "array_element"));
    {
        auto library = table();
        library = replaced(library, "@compiler_role(array_literal)", "@compiler_role(dict_literal)");
        EXPECT_TRUE(rejectedAsInvalidPrelude(library, "dict_literal"));
    }
}

// ---- dict_entry ----

TEST_F(DictRoleTest, TheEntryRoleHasAClosedShape) {
    const auto withEntry = [](const std::string& entry) {
        TableParts parts;
        parts.entry = entry;
        return parts.text();
    };
    const auto entry = std::string(tableEntry);
    // Required (the struct stays, so the literal still names it), and unique.
    EXPECT_TRUE(rejectedAsInvalidPrelude(withEntry(replaced(entry, "@compiler_role(dict_entry)\n", "")),
                                         "dict_entry"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(withEntry(entry + replaced(entry, "TableEntry", "OtherEntry")),
                                         "dict_entry"));
    // Private, exactly the fields `key: K` then `value: V`, and no deinit.
    EXPECT_TRUE(rejectedAsInvalidPrelude(withEntry(replaced(entry, "private struct", "public struct")), "private"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(withEntry(replaced(entry, "var value: V\n", "var value: V\n    deinit() {}\n")),
                                         "dict_entry"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(withEntry(replaced(entry, "var value: V\n", "var value: V\n    var extra: Int\n")),
                                         "dict_entry"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(withEntry(replaced(entry, "var key: K\n    var value: V",
                                                            "var value: V\n    var key: K")),
                                         "dict_entry"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(withEntry(replaced(entry, "var key: K\n    var value: V",
                                                            "var key: V\n    var value: K")),
                                         "dict_entry"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(withEntry(replaced(entry, "var key: K", "var first: K")), "dict_entry"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(withEntry(replaced(entry, "TableEntry<K, V>", "TableEntry<K, V, W>")),
                                         "dict_entry"));
    // The role belongs on a struct.
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            withEntry(entry + "\n@compiler_role(dict_entry)\nprivate func loose(): Int { return 0 }\n"), "dict_entry"));
}

TEST_F(DictRoleTest, TheRoleTypesMayNotBeExtended) {
    TableParts parts;
    parts.extra = "extension Table<A, B> { }\n";
    EXPECT_TRUE(rejectedAsInvalidPrelude(parts.text(), "may not extend"));
    parts.extra = "extension TableEntry<A, B> { }\n";
    EXPECT_TRUE(rejectedAsInvalidPrelude(parts.text(), "may not extend"));
}

// ---- Use from user code ----

TEST_F(DictRoleTest, CountIsReadableAndTheOtherMembersAreInaccessible) {
    EXPECT_TRUE(rejectedMentioning("func bad(table: inout Table<Int, Int>) { &table.count = 3 }\n", "count"))
            << diagnostics();
    EXPECT_TRUE(rejectedMentioning("func bad(table: Table<Int, Int>): Int { return table.address }\n", "address"))
            << diagnostics();
    EXPECT_TRUE(rejectedMentioning("func bad(table: Table<Int, Int>): Int { return table.element(key: 1) }\n",
                                   "element"))
            << diagnostics();
    EXPECT_TRUE(rejectedMentioning(
            "func bad(table: inout Table<Int, Int>) { &table.insert(key: 1, value: 2) }\n", "insert"))
            << diagnostics();
    EXPECT_TRUE(rejectedMentioning("func bad(): Int { return joyeer_intrinsic_size_of<TableEntry<Int, Int>>() }\n",
                                   "TableEntry"))
            << diagnostics();
}

TEST_F(DictRoleTest, CopyabilityFollowsBothOwnedTypes) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    load("func clone(table: Table<Int, String>): Table<Int, String> { let again = table\n return again }\n"
         "func nested(table: Table<String, Table<Int, [String]>>): Table<String, Table<Int, [String]>> {\n"
         "    let again = table\n return again\n}\n");
    EXPECT_TRUE(checks()) << diagnostics();
    // A noncopyable key or value makes the table noncopyable, although the role type declares `deinit`.
    load("struct Resource { var value: Int\n deinit() {} }\n"
         "func bad(table: Table<Int, Resource>): Table<Int, Resource> { let again = table\n return again }\n");
    EXPECT_TRUE(hasTypeDiagnostic(Id::copyOfNoncopyable)) << diagnostics();
    load("struct Resource { var value: Int\n deinit() {} }\n"
         "func bad(table: Table<Resource, Int>): Table<Resource, Int> { let again = table\n return again }\n");
    EXPECT_TRUE(hasTypeDiagnostic(Id::copyOfNoncopyable)) << diagnostics();
    load("struct Resource { var value: Int\n deinit() {} }\n"
         "func use(table: borrowing Table<Int, Resource>): Int { return table.count }\n");
    EXPECT_TRUE(checks()) << diagnostics();
}

TEST_F(DictRoleTest, TheDictRoleIsAffineOnly) {
    // Unlike the array role, the dictionary role owns no linear key or value (0027 Decision 3).
    // Since the switch the role type is the dictionary, so it reports the dictionary's own diagnostic, once.
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    load("func bad(table: borrowing Table<Int, JoinHandle<Int>>): Int { return table.count }\n");
    EXPECT_TRUE(hasTypeDiagnostic(Id::linearCollectionElement)) << diagnostics();
    EXPECT_FALSE(hasTypeDiagnostic(Id::linearTypeArgument)) << diagnostics();
    load("func bad(table: borrowing Table<JoinHandle<Int>, Int>): Int { return table.count }\n");
    EXPECT_TRUE(hasTypeDiagnostic(Id::linearCollectionElement)) << diagnostics();
}

TEST_F(DictRoleTest, DictionarySyntaxBindsToTheRole) {
    // The switch (0027 stage 6): `[K: V]` and literals bind to the `dict_type` declaration, never to a spelling.
    load("func same(table: Table<String, Int>): [String: Int] { return table }\n"
         "func back(table: [String: Int]): Table<String, Int> { return table }\n"
         "func literal(): Table<String, Int> { return [\"a\": 1] }\n");
    EXPECT_TRUE(checks()) << diagnostics();
}

TEST_F(DictRoleTest, EveryDictionaryOperationLowersThroughTheRoles) {
    load("func make(): [String: Int] { return [\"a\": 1, \"b\": 2, \"a\": 3] }\n"
         "func empty(): [String: Int] { return [:] }\n"
         "func read(table: [String: Int]): Int { return table[\"a\"] }\n"
         "func write(table: inout [String: Int]) { &table[\"c\"] = 3 }\n"
         "func mutate(table: inout [String: [Int]]) { &table[\"c\"][0] = 3 }\n"
         "func look(table: [String: Int]): Int? { return table.get(key: \"a\") }\n"
         "func size(table: [String: Int]): Int { return table.count }\n"
         "func copy(table: [String: Int]): [String: Int] { let again = table\n return again }\n");
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    const auto verification = joyeer::ir::Verifier().verify(*module);
    EXPECT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
    // No removed builtin dictionary slot (the extern names in the dump would match a text search).
    for (const auto& function : module->functions) {
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                const auto slot = static_cast<unsigned>(instruction.opcode);
                EXPECT_TRUE((slot < 39 || slot > 41) && slot != 50) << function.name;
            }
        }
    }
    // A literal packs into one block per literal and calls the literal method.
    size_t blocks = 0;
    for (const auto& function : module->functions) {
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                blocks += instruction.opcode == joyeer::ir::Opcode::stackBlock && !function.runtimeContext;
            }
        }
    }
    EXPECT_EQ(blocks, 2u);
    for (const auto* method : { ".literal", ".element", ".insert", ".get", ".duplicate" }) {
        EXPECT_TRUE(std::any_of(module->functions.begin(), module->functions.end(), [&](const auto& function) {
            return function.name.find(method) != std::string::npos;
        })) << method;
    }
}

TEST_F(DictRoleTest, TheRoleIsRequiredOnlyWhenAProgramMentionsADictionary) {
    // The seam (0027 Decision 7): a collections prelude without `dict_type` is valid until a dictionary is used.
    load("func main() { let values: [Int] = [1]\n print(value: values[0]) }\n", arrayLibrary);
    EXPECT_TRUE(checks()) << diagnostics();
    load("func f(table: [String: Int]): Int { return 1 }\n", arrayLibrary);
    EXPECT_NE(diagnostics().find("invalid-prelude"), std::string::npos) << diagnostics();
    EXPECT_NE(diagnostics().find("dict_type"), std::string::npos) << diagnostics();
    load("func f(): Int { let table: [String: Int] = [\"a\": 1]\n return table.count }\n", arrayLibrary);
    EXPECT_NE(diagnostics().find("dict_type"), std::string::npos) << diagnostics();
    // An unannotated literal is a mention too.
    load("func f(): Int { let table = [\"a\": 1]\n return table.count }\n", arrayLibrary);
    EXPECT_NE(diagnostics().find("invalid-prelude"), std::string::npos) << diagnostics();
    EXPECT_NE(diagnostics().find("dict_type"), std::string::npos) << diagnostics();
}

// Only the dictionary's own declaration may key a dictionary by its type parameter (0027 Decision 4); any other
// generic in a bundled prelude is held to the key rules.
TEST_F(DictRoleTest, OnlyTheDictionaryItselfMayKeyByATypeParameter) {
    load("func main() {}\n", table() + "\npublic func misuse<T>(table: Table<T, Int>): Int { return 1 }\n");
    EXPECT_NE(diagnostics().find("a dictionary key cannot be the type parameter type 'T'"), std::string::npos)
            << diagnostics();
    load("func main() {}\n", table() + "\npublic func misuse<T>(table: [T: Int]): Int { return 1 }\n");
    EXPECT_NE(diagnostics().find("a dictionary key cannot be the type parameter type 'T'"), std::string::npos)
            << diagnostics();
}

// ---- IR, wire and backend ----

// The structure roles by their wire values (decision 0027, wire 24): 2 the dictionary, 3 its entry.
constexpr auto dictRole = static_cast<joyeer::ir::StructureRole>(2);
constexpr auto entryRole = static_cast<joyeer::ir::StructureRole>(3);

joyeer::ir::StructureDefinition* structureWithRole(joyeer::ir::Module& module, joyeer::ir::StructureRole role,
                                                    const std::string& name = "") {
    for (auto& definition : module.structures) {
        if (definition.role == role && (name.empty() || definition.name == name)) return &definition;
    }
    return nullptr;
}

joyeer::ir::Function* functionNamed(joyeer::ir::Module& module, const std::string& part) {
    for (auto& function : module.functions) {
        if (function.name.find(part) != std::string::npos) return &function;
    }
    return nullptr;
}

const joyeer::ir::TypeName* typeOf(const joyeer::ir::Module& module, joyeer::ir::TypeId id) {
    for (const auto& type : module.types) {
        if (type.id == id) return &type;
    }
    return nullptr;
}

TEST_F(DictRoleTest, TheRoleStructsLowerWithTheirRolesAndNoLiveFlag) {
    load("func size(table: Table<Int, String>): Int { return table.count }\n");
    auto module = lower();
    ASSERT_NE(module, nullptr);
    const auto* table = structureWithRole(*module, dictRole);
    ASSERT_NE(table, nullptr) << joyeer::ir::dump(*module);
    ASSERT_EQ(table->fields.size(), 3u) << "an inert-zero handle has no hidden live flag";
    EXPECT_EQ(table->fields[0].name, "address");
    EXPECT_EQ(table->fields[1].name, "count");
    EXPECT_EQ(table->fields[2].name, "capacity");
    EXPECT_TRUE(table->deinitializer.has_value());
    EXPECT_TRUE(table->copier.has_value()) << "Table<Int, String> is copyable";
    // The interned entry over the same key and value: exactly `key: Int`, `value: String`, nothing to run.
    const auto* entry = structureWithRole(*module, entryRole);
    ASSERT_NE(entry, nullptr) << joyeer::ir::dump(*module);
    ASSERT_EQ(entry->fields.size(), 2u);
    EXPECT_EQ(entry->fields[0].name, "key");
    EXPECT_EQ(entry->fields[1].name, "value");
    EXPECT_EQ(entry->fields[0].type, checking.model->types().intType());
    EXPECT_EQ(entry->fields[1].type, checking.model->types().stringType());
    EXPECT_FALSE(entry->deinitializer.has_value());
    EXPECT_FALSE(entry->copier.has_value());
    const auto verification = joyeer::ir::Verifier().verify(*module);
    EXPECT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
}

TEST_F(DictRoleTest, InstanceDiscoveryDemandsTheCompilerCalledMethodsAndInlinesTheWrappers) {
    load("func size(table: Table<Int, String>): Int { return table.count }\n");
    auto module = lower();
    ASSERT_NE(module, nullptr);
    const auto* element = functionNamed(*module, ".element");
    const auto* insert = functionNamed(*module, ".insert");
    const auto* copy = functionNamed(*module, ".duplicate");
    const auto* literal = functionNamed(*module, ".literal");
    ASSERT_NE(element, nullptr) << joyeer::ir::dump(*module);
    ASSERT_NE(insert, nullptr);
    ASSERT_NE(copy, nullptr);
    ASSERT_NE(literal, nullptr);
    EXPECT_TRUE(element->elementAddressor);
    EXPECT_TRUE(literal->literalFactory);
    EXPECT_FALSE(insert->literalFactory);
    // The closed wrappers are always inlined (0026 A4, extended by role in 0027).
    EXPECT_TRUE(insert->alwaysInline);
    EXPECT_TRUE(copy->alwaysInline);
    EXPECT_TRUE(literal->alwaysInline);
    EXPECT_EQ(functionNamed(*module, ".get"), nullptr) << "get is demanded only by user calls";
    load("func lookup(table: Table<Int, String>): String? { return table.get(key: 1) }\n");
    module = lower();
    ASSERT_NE(module, nullptr);
    const auto* get = functionNamed(*module, ".get");
    ASSERT_NE(get, nullptr) << joyeer::ir::dump(*module);
    EXPECT_TRUE(get->alwaysInline);
    EXPECT_TRUE(joyeer::ir::Verifier().verify(*module).succeeded());
}

TEST_F(DictRoleTest, ANoncopyableValueHasNoCopier) {
    // (A noncopyable key is not an admitted key: its `key_kind` is a lowering error, so the checker test covers it.)
    load("struct Resource { var value: Int\n deinit() {} }\n"
         "func value(table: borrowing Table<Int, Resource>): Int { return table.count }\n");
    auto module = lower();
    ASSERT_NE(module, nullptr);
    size_t tables = 0;
    for (const auto& definition : module->structures) {
        if (definition.role != dictRole) continue;
        ++tables;
        EXPECT_FALSE(definition.copier.has_value()) << definition.name;
    }
    EXPECT_EQ(tables, 1u);
    EXPECT_EQ(functionNamed(*module, ".duplicate"), nullptr);
    const auto verification = joyeer::ir::Verifier().verify(*module);
    EXPECT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
}

// Wire 24 added the dictionary roles; wire 25 removed the builtin dictionary's slots.
TEST_F(DictRoleTest, TheWireCarriesTheDictionaryRoles) {
    EXPECT_EQ(joyeer::llvmbackend::wire::version, 25u);
    load("func size(table: Table<String, [Int]>): Int { return table.count }\n");
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    joyeer::llvmbackend::wire::Writer writer;
    writer(*module);
    joyeer::ir::Module decoded;
    joyeer::llvmbackend::wire::Reader reader(writer.bytes);
    reader(decoded);
    ASSERT_TRUE(reader.succeeded());
    EXPECT_EQ(joyeer::ir::dump(decoded), joyeer::ir::dump(*module));
    ASSERT_NE(structureWithRole(decoded, dictRole), nullptr);
    ASSERT_NE(structureWithRole(decoded, entryRole), nullptr);
    EXPECT_TRUE(functionNamed(decoded, ".literal")->literalFactory);
    EXPECT_TRUE(functionNamed(decoded, ".element")->elementAddressor);
}

TEST_F(DictRoleTest, TheVerifierRejectsForgedDictionaryRoles) {
    load("func size(table: Table<Int, String>): Int { return table.count }\n");
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    ASSERT_TRUE(joyeer::ir::Verifier().verify(*module).succeeded());
    const auto integer = checking.model->types().intType();
    const auto text = checking.model->types().stringType();
    for (unsigned mutation = 0; mutation < 14; ++mutation) {
        SCOPED_TRACE(mutation);
        auto copy = *module;
        auto* table = structureWithRole(copy, dictRole);
        auto* entry = structureWithRole(copy, entryRole);
        ASSERT_NE(table, nullptr);
        ASSERT_NE(entry, nullptr);
        const auto arguments = [&](joyeer::ir::StructureDefinition& definition) -> std::vector<joyeer::ir::TypeId>& {
            return std::find_if(copy.types.begin(), copy.types.end(), [&](const auto& type) {
                return type.id == definition.type;
            })->arguments;
        };
        switch (mutation) {
            case 0: table->role = static_cast<joyeer::ir::StructureRole>(4); break;
            // The dictionary: two type arguments, the three Int words, no live flag, a deinit.
            case 1: arguments(*table).pop_back(); break;
            case 2: arguments(*table).push_back(integer); break;
            case 3: table->fields.pop_back(); break;
            case 4:
                table->fields.push_back({ joyeer::semantic::invalidSymbolId, joyeer::ir::liveFieldName,
                                          checking.model->types().boolType(), false });
                break;
            case 5: table->deinitializer.reset(); break;
            case 6: table->fields[1].type = text; break;
            // The entry: exactly `key: K`, `value: V` of its two type arguments, nothing to run or copy.
            case 7: std::swap(entry->fields[0].type, entry->fields[1].type); break;
            case 8: entry->fields.push_back(entry->fields[0]); entry->fields.back().symbol += 1000; break;
            case 9: entry->deinitializer = table->deinitializer; break;
            case 10: arguments(*entry).push_back(integer); break;
            case 11: entry->copier = table->copier; break;
            // The addressor borrows the dictionary and a key of type K.
            case 12: functionNamed(copy, ".element")->parameters[1].value.type = text; break;
            // A literal factory consumes a dictionary and returns the same instance.
            case 13: functionNamed(copy, ".literal")->resultType = integer; break;
        }
        EXPECT_FALSE(joyeer::ir::Verifier().verify(copy).succeeded());
    }
}

// ---- The closed literal packing protocol for a dictionary (0027 Decision 3, rules 1-6) ----

joyeer::ir::Function& functionExactly(joyeer::ir::Module& module, const std::string& name) {
    const auto found = std::find_if(module.functions.begin(), module.functions.end(),
                                    [&](const auto& function) { return function.name == name; });
    EXPECT_NE(found, module.functions.end()) << name;
    return *found;
}

// The parts of a packed dictionary literal that the mutations change.
struct PackedLiteral {
    joyeer::ir::TypeId table = 0;
    joyeer::ir::TypeId entry = 0;
    joyeer::semantic::SymbolId keyField = 0;
    joyeer::semantic::SymbolId valueField = 0;
    joyeer::ir::FunctionId factory = 0;
};

// Replaces the body of `name`, a function returning a table, with a literal of `entries` entries packed the way the
// switch's lowering will pack one: keys and values evaluated first, an inert zero receiver, one stack block of
// entries, each entry's key and (unless the value is Void) value stored through a field address, then one transfer
// to the literal method.
PackedLiteral packLiteral(joyeer::ir::Module& module, const std::string& name, int64_t entries) {
    using joyeer::ir::Opcode;
    using joyeer::ir::Value;
    using joyeer::ir::ValueCategory;
    using joyeer::ir::ValueId;
    auto& function = functionExactly(module, name);
    PackedLiteral literal;
    literal.table = function.resultType;
    const auto* tableType = typeOf(module, literal.table);
    const auto entry = std::find_if(module.structures.begin(), module.structures.end(), [&](const auto& definition) {
        return definition.role == entryRole && typeOf(module, definition.type)->arguments == tableType->arguments;
    });
    EXPECT_NE(entry, module.structures.end());
    literal.entry = entry->type;
    literal.keyField = entry->fields[0].symbol;
    literal.valueField = entry->fields[1].symbol;
    const auto factory = std::find_if(module.functions.begin(), module.functions.end(), [&](const auto& candidate) {
        return candidate.literalFactory && candidate.resultType == literal.table;
    });
    EXPECT_NE(factory, module.functions.end());
    literal.factory = factory->id;
    const auto keyType = tableType->arguments[0];
    const auto valueType = tableType->arguments[1];
    const bool unitValue = typeOf(module, valueType)->kind == joyeer::typing::TypeKind::voidType;
    const auto integer = keyType;
    const auto prototype = function.blocks[0].instructions.front();
    std::vector<joyeer::ir::Instruction> body;
    ValueId next = 5000;
    const auto emit = [&](Opcode opcode, std::optional<std::pair<joyeer::ir::TypeId, ValueCategory>> result,
                          std::vector<ValueId> operands, int64_t number = 0,
                          std::optional<joyeer::semantic::SymbolId> symbol = std::nullopt) -> ValueId {
        joyeer::ir::Instruction instruction { opcode };
        if (result.has_value()) instruction.result = Value { next++, result->first, result->second };
        instruction.operands = std::move(operands);
        instruction.integerValue = number;
        instruction.symbol = symbol;
        instruction.span = prototype.span;
        instruction.debugLocation = prototype.debugLocation;
        if (opcode == Opcode::call) instruction.callee = literal.factory;
        body.push_back(instruction);
        return instruction.result.has_value() ? instruction.result->id : 0;
    };
    const auto value = [](joyeer::ir::TypeId type) { return std::make_pair(type, ValueCategory::value); };
    const auto address = [](joyeer::ir::TypeId type) { return std::make_pair(type, ValueCategory::address); };
    std::vector<ValueId> keys;
    std::vector<ValueId> values;
    for (int64_t index = 0; index < entries; ++index) {
        keys.push_back(emit(Opcode::integerConstant, value(integer), {}, index + 1));
        if (!unitValue) values.push_back(emit(Opcode::booleanConstant, value(valueType), {}, index % 2));
    }
    const auto count = emit(Opcode::integerConstant, value(integer), {}, entries);
    const auto place = emit(Opcode::stackAllocate, address(literal.table), {});
    emit(Opcode::zeroInitialize, std::nullopt, { place });
    const auto receiver = emit(Opcode::load, value(literal.table), { place });
    const auto block = emit(Opcode::stackBlock, value(integer), { count }, static_cast<int64_t>(literal.entry));
    for (int64_t index = 0; index < entries; ++index) {
        const auto offset = emit(Opcode::integerConstant, value(integer), {}, index);
        const auto slot = emit(Opcode::slotAddress, value(integer), { block, offset },
                               static_cast<int64_t>(literal.entry));
        const auto at = emit(Opcode::integerAddress, address(literal.entry), { slot });
        const auto key = emit(Opcode::fieldAddress, address(keyType), { at }, 0, literal.keyField);
        emit(Opcode::store, std::nullopt, { keys[static_cast<size_t>(index)], key });
        if (unitValue) continue;
        const auto stored = emit(Opcode::fieldAddress, address(valueType), { at }, 0, literal.valueField);
        emit(Opcode::store, std::nullopt, { values[static_cast<size_t>(index)], stored });
    }
    const auto result = emit(Opcode::call, value(literal.table), { receiver, block, count });
    emit(Opcode::returnValue, std::nullopt, { result });
    function.blocks.resize(1);
    function.blocks[0].instructions = std::move(body);
    return literal;
}

std::vector<joyeer::ir::Instruction>& bodyOf(joyeer::ir::Module& module, const std::string& name) {
    return functionExactly(module, name).blocks[0].instructions;
}

// The index of the `nth` instruction of `opcode` (counting from 0), optionally with field `symbol`.
size_t nth(const std::vector<joyeer::ir::Instruction>& body, joyeer::ir::Opcode opcode, size_t which = 0,
           std::optional<joyeer::semantic::SymbolId> symbol = std::nullopt) {
    for (size_t index = 0; index < body.size(); ++index) {
        if (body[index].opcode != opcode || (symbol.has_value() && body[index].symbol != symbol)) continue;
        if (which-- == 0) return index;
    }
    ADD_FAILURE() << "no such instruction";
    return 0;
}

constexpr auto literalProgram =
        "func make(): Table<Int, Bool> { return make() }\n"
        "func makeUnit(): Table<Int, Void> { return makeUnit() }\n"
        "func makeEmpty(): Table<Int, Bool> { return makeEmpty() }\n";

TEST_F(DictRoleTest, TheVerifierAcceptsAPackedDictionaryLiteral) {
    load(literalProgram);
    auto module = lower();
    ASSERT_NE(module, nullptr);
    packLiteral(*module, "make", 2);
    packLiteral(*module, "makeUnit", 2);
    packLiteral(*module, "makeEmpty", 0);
    const auto verification = joyeer::ir::Verifier().verify(*module);
    EXPECT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification) << joyeer::ir::dump(*module);
}

TEST_F(DictRoleTest, TheVerifierRejectsEachMalformedPackingAndNamesTheRule) {
    using joyeer::ir::Opcode;
    load(literalProgram);
    const auto lowered = lower();
    ASSERT_NE(lowered, nullptr);
    auto base = *lowered;
    const auto literal = packLiteral(base, "make", 2);
    packLiteral(base, "makeUnit", 2);
    ASSERT_TRUE(joyeer::ir::Verifier().verify(base).succeeded());
    const auto key = literal.keyField;
    const auto value = literal.valueField;
    struct Case {
        const char* name;
        const char* rule;
        std::function<void(joyeer::ir::Module&, std::vector<joyeer::ir::Instruction>&)> mutate;
    };
    const auto eraseFieldStore = [](std::vector<joyeer::ir::Instruction>& body, size_t field) {
        // A field address is followed by its store.
        body.erase(body.begin() + static_cast<std::ptrdiff_t>(field), body.begin() + static_cast<std::ptrdiff_t>(field + 2));
    };
    const std::vector<Case> cases {
        { "missing key store", "rule 5", [&](auto&, auto& body) { eraseFieldStore(body, nth(body, Opcode::fieldAddress, 0, key)); } },
        { "missing value store", "rule 5", [&](auto&, auto& body) { eraseFieldStore(body, nth(body, Opcode::fieldAddress, 1, value)); } },
        { "wrong count", "rule 5", [&](auto&, auto& body) {
              body[nth(body, Opcode::stackBlock) - 4].integerValue = 3;
          } },
        { "key stored twice for one entry", "rule 3", [&](auto&, auto& body) {
              // Entry 1's key field address projects entry 0's place instead.
              body[nth(body, Opcode::fieldAddress, 1, key)].operands[0] =
                      body[nth(body, Opcode::fieldAddress, 0, key)].operands[0];
          } },
        { "a value stored as the key", "rule 4", [&](auto&, auto& body) {
              body[nth(body, Opcode::fieldAddress, 0, key) + 1].operands[0] =
                      body[nth(body, Opcode::booleanConstant)].result->id;
          } },
        { "swapped fields", "rule 4", [&](auto&, auto& body) {
              auto& first = body[nth(body, Opcode::fieldAddress, 0, key)];
              auto& second = body[nth(body, Opcode::fieldAddress, 0, value)];
              std::swap(first.symbol, second.symbol);
              std::swap(first.result->type, second.result->type);
          } },
        { "an escaping field address", "rule 4", [&](auto&, auto& body) {
              const auto at = nth(body, Opcode::fieldAddress, 0, key);
              joyeer::ir::Instruction escape { Opcode::pointerToInteger };
              escape.operands = { body[at].result->id };
              escape.result = joyeer::ir::Value { 9000, body[nth(body, Opcode::stackBlock)].result->type,
                                                  joyeer::ir::ValueCategory::value };
              body.insert(body.begin() + static_cast<std::ptrdiff_t>(at + 2), escape);
          } },
        { "a non-entry element", "rule 2", [&](auto& module, auto& body) {
              const auto integer = checking.model->types().intType();
              (void) module;
              for (auto& instruction : body) {
                  if (instruction.opcode == Opcode::stackBlock || instruction.opcode == Opcode::slotAddress) {
                      instruction.integerValue = static_cast<int64_t>(integer);
                  }
                  if (instruction.opcode == Opcode::integerAddress) instruction.result->type = integer;
              }
          } },
        { "a load in the window", "rule 6", [&](auto&, auto& body) {
              const auto block = nth(body, Opcode::stackBlock);
              auto load = body[nth(body, Opcode::load)];
              load.result->id = 9000;
              body.insert(body.begin() + static_cast<std::ptrdiff_t>(block + 1), load);
          } },
        { "a fallible operation in the window", "rule 6", [&](auto&, auto& body) {
              const auto block = nth(body, Opcode::stackBlock);
              joyeer::ir::Instruction add { Opcode::add };
              add.operands = { body[block].operands[0], body[block].operands[0] };
              add.result = joyeer::ir::Value { 9000, body[block].result->type, joyeer::ir::ValueCategory::value };
              body.insert(body.begin() + static_cast<std::ptrdiff_t>(block + 1), add);
          } },
        { "a non-inert receiver", "rule 6", [&](auto&, auto& body) {
              body.erase(body.begin() + static_cast<std::ptrdiff_t>(nth(body, Opcode::zeroInitialize)));
          } },
        { "two transfers", "rule 6", [&](auto&, auto& body) {
              const auto call = nth(body, Opcode::call);
              auto again = body[call];
              again.result->id = 9000;
              body.insert(body.begin() + static_cast<std::ptrdiff_t>(call), again);
          } },
        { "a cross-block store and transfer", "rule 6", [&](auto& module, auto& body) {
              // The last value store, the transfer and the return move to a second block.
              auto& function = functionExactly(module, "make");
              const auto from = nth(body, Opcode::fieldAddress, 1, value) + 1;
              joyeer::ir::BasicBlock next { 1, "next", { body.begin() + static_cast<std::ptrdiff_t>(from), body.end() } };
              body.erase(body.begin() + static_cast<std::ptrdiff_t>(from), body.end());
              joyeer::ir::Instruction branch { Opcode::branch };
              branch.targets = { 1 };
              body.push_back(branch);
              function.blocks.push_back(std::move(next));
          } },
    };
    for (const auto& mutation : cases) {
        SCOPED_TRACE(mutation.name);
        auto copy = base;
        mutation.mutate(copy, bodyOf(copy, "make"));
        const auto verification = joyeer::ir::Verifier().verify(copy);
        EXPECT_FALSE(verification.succeeded());
        const auto messages = joyeer::ir::dump(verification);
        EXPECT_NE(messages.find(std::string("dictionary literal ") + mutation.rule), std::string::npos) << messages;
    }
    {
        // Rule 5 for a Void value: no value field is projected or stored.
        SCOPED_TRACE("a Void value stored");
        auto copy = base;
        auto& body = bodyOf(copy, "makeUnit");
        const auto key = nth(body, Opcode::fieldAddress, 0, literal.keyField);
        auto unitValue = body[key];
        unitValue.symbol = std::find_if(copy.structures.begin(), copy.structures.end(), [&](const auto& definition) {
            return definition.role == entryRole && definition.fields[1].type != definition.fields[0].type &&
                   typeOf(copy, definition.fields[1].type)->kind == joyeer::typing::TypeKind::voidType;
        })->fields[1].symbol;
        unitValue.result->id = 9000;
        unitValue.result->type = typeOf(copy, functionExactly(copy, "makeUnit").resultType)->arguments[1];
        joyeer::ir::Instruction unit { Opcode::unitConstant };
        unit.result = joyeer::ir::Value { 9001, unitValue.result->type, joyeer::ir::ValueCategory::value };
        joyeer::ir::Instruction store { Opcode::store };
        store.operands = { 9001, 9000 };
        body.insert(body.begin(), unit);
        body.insert(body.begin() + static_cast<std::ptrdiff_t>(key + 3), { unitValue, store });
        const auto messages = joyeer::ir::dump(joyeer::ir::Verifier().verify(copy));
        EXPECT_NE(messages.find("dictionary literal rule"), std::string::npos) << messages;
    }
    {
        // An entry field address outside verified literal storage, in an otherwise valid function.
        SCOPED_TRACE("a hand-written field address");
        auto copy = base;
        auto& body = bodyOf(copy, "make");
        joyeer::ir::Instruction storage { Opcode::stackAllocate };
        storage.result = joyeer::ir::Value { 9000, literal.entry, joyeer::ir::ValueCategory::address };
        joyeer::ir::Instruction field { Opcode::fieldAddress };
        field.operands = { 9000 };
        field.symbol = literal.keyField;
        field.result = joyeer::ir::Value { 9001, checking.model->types().intType(), joyeer::ir::ValueCategory::address };
        body.insert(body.begin(), { storage, field });
        const auto messages = joyeer::ir::dump(joyeer::ir::Verifier().verify(copy));
        EXPECT_NE(messages.find("dictionary entry"), std::string::npos) << messages;
    }
    {
        // The transfer goes to a method that is not the literal factory.
        SCOPED_TRACE("no literal factory");
        auto copy = base;
        copy.functions[literal.factory].literalFactory = false;
        EXPECT_FALSE(joyeer::ir::Verifier().verify(copy).succeeded());
    }
}

TEST_F(DictRoleTest, MorePackingFormsAreRejectedByTheirRule) {
    using joyeer::ir::Opcode;
    load(literalProgram);
    const auto lowered = lower();
    ASSERT_NE(lowered, nullptr);
    auto base = *lowered;
    const auto literal = packLiteral(base, "make", 2);
    packLiteral(base, "makeEmpty", 0);
    ASSERT_TRUE(joyeer::ir::Verifier().verify(base).succeeded());
    const auto key = literal.keyField;
    const auto value = literal.valueField;
    struct Case {
        const char* name;
        const char* function;
        const char* rule;
        std::function<void(std::vector<joyeer::ir::Instruction>&)> mutate;
    };
    const std::vector<Case> cases {
        { "two stores through one field address", "make", "rule 4", [&](auto& body) {
              const auto store = nth(body, Opcode::fieldAddress, 0, key) + 1;
              body.insert(body.begin() + static_cast<std::ptrdiff_t>(store), body[store]);
          } },
        { "two slots at one offset", "make", "rule 5", [&](auto& body) {
              body[nth(body, Opcode::slotAddress, 1)].operands[1] = body[nth(body, Opcode::slotAddress, 0)].operands[1];
          } },
        { "a value projected twice", "make", "rule 3", [&](auto& body) {
              const auto at = nth(body, Opcode::fieldAddress, 0, value);
              auto again = body[at];
              again.result->id = 9000;
              body.insert(body.begin() + static_cast<std::ptrdiff_t>(at + 2), again);
          } },
        { "a projection after the transfer", "make", "rule 3", [&](auto& body) {
              auto late = body[nth(body, Opcode::fieldAddress, 0, key)];
              late.result->id = 9000;
              body.insert(body.begin() + static_cast<std::ptrdiff_t>(nth(body, Opcode::call) + 1), late);
          } },
        { "a key evaluated inside the window", "make", "rule 4", [&](auto& body) {
              const auto constant = body.front();
              body.erase(body.begin());
              body.insert(body.begin() + static_cast<std::ptrdiff_t>(nth(body, Opcode::fieldAddress, 0, key) + 1),
                          constant);
          } },
        { "one value stored as two keys", "make", "rule 4", [&](auto& body) {
              body[nth(body, Opcode::fieldAddress, 1, key) + 1].operands[0] =
                      body[nth(body, Opcode::fieldAddress, 0, key) + 1].operands[0];
          } },
        // A stored value's only other uses are on paths that never reach the transfer, such as its cleanup on an
        // early exit; a use the packing block reaches, or one before the storage, would drop or move it twice.
        { "a stored key used again after the transfer", "make", "rule 4", [&](auto& body) {
              const auto stored = body[nth(body, Opcode::fieldAddress, 0, key) + 1].operands[0];
              joyeer::ir::Instruction place { Opcode::stackAllocate };
              place.result = joyeer::ir::Value { 9000, checking.model->types().intType(),
                                                 joyeer::ir::ValueCategory::address };
              joyeer::ir::Instruction store { Opcode::store };
              store.operands = { stored, 9000 };
              body.insert(body.begin() + static_cast<std::ptrdiff_t>(nth(body, Opcode::call) + 1), { place, store });
          } },
        { "a stored value used before the storage", "make", "rule 4", [&](auto& body) {
              const auto stored = body[nth(body, Opcode::fieldAddress, 0, value) + 1].operands[0];
              joyeer::ir::Instruction place { Opcode::stackAllocate };
              place.result = joyeer::ir::Value { 9000, checking.model->types().boolType(),
                                                 joyeer::ir::ValueCategory::address };
              joyeer::ir::Instruction store { Opcode::store };
              store.operands = { stored, 9000 };
              body.insert(body.begin() + static_cast<std::ptrdiff_t>(nth(body, Opcode::stackBlock)), { place, store });
          } },
        { "an empty literal with a stray slot", "makeEmpty", "rule 3", [&](auto& body) {
              const auto block = nth(body, Opcode::stackBlock);
              joyeer::ir::Instruction offset { Opcode::integerConstant };
              offset.result = joyeer::ir::Value { 9000, body[block].result->type, joyeer::ir::ValueCategory::value };
              joyeer::ir::Instruction slot { Opcode::slotAddress };
              slot.operands = { body[block].result->id, 9000 };
              slot.integerValue = body[block].integerValue;
              slot.result = joyeer::ir::Value { 9001, body[block].result->type, joyeer::ir::ValueCategory::value };
              body.insert(body.begin() + static_cast<std::ptrdiff_t>(block + 1), { offset, slot });
          } },
    };
    for (const auto& mutation : cases) {
        SCOPED_TRACE(mutation.name);
        auto copy = base;
        mutation.mutate(bodyOf(copy, mutation.function));
        const auto messages = joyeer::ir::dump(joyeer::ir::Verifier().verify(copy));
        EXPECT_NE(messages.find(std::string("dictionary literal ") + mutation.rule), std::string::npos) << messages;
    }
}

// Rule 4 across blocks: a stored value's other uses must be on paths that never reach the packing and that the
// packing never reaches. Each shape splits the packed `make` into blocks and adds one use of the first key.
TEST_F(DictRoleTest, AStoredValueUsedOnAPathThroughThePackingIsRejected) {
    using joyeer::ir::Opcode;
    load(literalProgram);
    const auto lowered = lower();
    ASSERT_NE(lowered, nullptr);
    auto base = *lowered;
    const auto literal = packLiteral(base, "make", 2);
    ASSERT_TRUE(joyeer::ir::Verifier().verify(base).succeeded());
    const auto& packed = bodyOf(base, "make");
    // The packing starts at the zero receiver, which rule 6 keeps in the packing block.
    const auto storage = nth(packed, Opcode::stackAllocate);
    const auto call = nth(packed, Opcode::call);
    const auto key0 = packed[nth(packed, Opcode::fieldAddress, 0, literal.keyField) + 1].operands[0];
    using Body = std::vector<joyeer::ir::Instruction>;
    const Body before(packed.begin(), packed.begin() + static_cast<std::ptrdiff_t>(storage));
    const Body packing(packed.begin() + static_cast<std::ptrdiff_t>(storage),
                       packed.begin() + static_cast<std::ptrdiff_t>(call + 1));
    const Body ret(packed.begin() + static_cast<std::ptrdiff_t>(call + 1), packed.end());
    const auto integer = checking.model->types().intType();
    const auto boolean = checking.model->types().boolType();
    const auto useKey = [&]() -> Body {
        joyeer::ir::Instruction place { Opcode::stackAllocate };
        place.result = joyeer::ir::Value { 9000, integer, joyeer::ir::ValueCategory::address };
        joyeer::ir::Instruction store { Opcode::store };
        store.operands = { key0, 9000 };
        return { place, store };
    };
    const auto branch = [](joyeer::ir::BlockId target) {
        joyeer::ir::Instruction instruction { Opcode::branch };
        instruction.targets = { target };
        return instruction;
    };
    const auto condition = [&]() {
        joyeer::ir::Instruction instruction { Opcode::booleanConstant };
        instruction.integerValue = 1;
        instruction.result = joyeer::ir::Value { 9100, boolean, joyeer::ir::ValueCategory::value };
        return instruction;
    };
    const auto choose = [](joyeer::ir::BlockId yes, joyeer::ir::BlockId no) {
        joyeer::ir::Instruction instruction { Opcode::conditionalBranch };
        instruction.operands = { 9100 };
        instruction.targets = { yes, no };
        return instruction;
    };
    const auto join = [](Body first, const Body& second) {
        first.insert(first.end(), second.begin(), second.end());
        return first;
    };
    const joyeer::ir::Instruction stop { Opcode::unreachable };
    // The key's definition apart from the rest of the code before the storage.
    Body withoutKey0;
    Body key0Definition;
    for (const auto& instruction : before) {
        const bool definesKey0 = instruction.result.has_value() && instruction.result->id == key0;
        (definesKey0 ? key0Definition : withoutKey0).push_back(instruction);
    }
    ASSERT_EQ(key0Definition.size(), 1u);
    struct Case {
        const char* name;
        std::vector<Body> blocks;
    };
    const std::vector<Case> cases {
        // A use in a block the packing block dominates (after the transfer).
        { "a successor of the packing", { join(join(before, packing), { branch(1) }), join(useKey(), ret) } },
        // A use in a dominator of the packing block (before the storage).
        { "a dominator of the packing", { join(join(before, useKey()), { branch(1) }), join(packing, ret) } },
        // A join reached both from the packing and from a path that skips it.
        { "a join after the packing", {
              join(before, { condition(), choose(1, 2) }), join(packing, { branch(3) }), { branch(3) },
              join(useKey(), { stop }) } },
        // A use on one arm of a diamond between the definition and the packing: neither dominates the other, but
        // the packing is reached after it (only the backward walk sees this one).
        { "a branch before the packing", {
              join(before, { condition(), choose(1, 2) }), join(useKey(), { branch(3) }), { branch(3) },
              join(packing, ret) } },
        // The packing in a loop whose value was evaluated before the loop: the second iteration transfers it again.
        { "a packing in a loop", {
              join(before, { condition(), branch(1) }), { choose(2, 3) }, join(packing, { branch(1) }), { stop } } },
        // A key evaluated on one arm of a diamond whose join packs it: the store is its only use, so only the
        // evaluated-before (dominance) check sees that the other arm reaches the packing without it.
        { "a definition that does not dominate the packing", {
              join(withoutKey0, { condition(), choose(1, 2) }), join(key0Definition, { branch(3) }), { branch(3) },
              join(packing, ret) } },
    };
    for (const auto& mutation : cases) {
        SCOPED_TRACE(mutation.name);
        auto copy = base;
        auto& function = functionExactly(copy, "make");
        function.blocks.clear();
        for (size_t index = 0; index < mutation.blocks.size(); ++index) {
            function.blocks.push_back(joyeer::ir::BasicBlock { static_cast<joyeer::ir::BlockId>(index),
                                                               "b" + std::to_string(index), mutation.blocks[index] });
        }
        const auto messages = joyeer::ir::dump(joyeer::ir::Verifier().verify(copy));
        EXPECT_NE(messages.find("dictionary literal rule 4"), std::string::npos) << messages;
    }
    // The same key evaluated in a block that dominates the packing block is accepted.
    auto dominated = base;
    auto& function = functionExactly(dominated, "make");
    function.blocks.clear();
    const std::vector<Body> blocks { join(withoutKey0, { branch(1) }), join(key0Definition, { branch(2) }),
                                     join(packing, ret) };
    for (size_t index = 0; index < blocks.size(); ++index) {
        function.blocks.push_back(joyeer::ir::BasicBlock { static_cast<joyeer::ir::BlockId>(index),
                                                           "b" + std::to_string(index), blocks[index] });
    }
    const auto verification = joyeer::ir::Verifier().verify(dominated);
    EXPECT_TRUE(verification.succeeded()) << joyeer::ir::dump(verification);
}

TEST_F(DictRoleTest, AnEntryIsNeverAValue) {
    // Only the packing of a verified literal reaches an entry, through addresses (0027 Decision 3): nothing holds,
    // passes, clones or destroys one.
    using joyeer::ir::Opcode;
    load(std::string(literalProgram) + "struct Holder { var count: Int }\n"
                                       "func hold(holder: Holder): Int { return holder.count }\n");
    const auto lowered = lower();
    ASSERT_NE(lowered, nullptr);
    auto base = *lowered;
    const auto literal = packLiteral(base, "make", 2);
    ASSERT_TRUE(joyeer::ir::Verifier().verify(base).succeeded());
    const auto integer = checking.model->types().intType();
    const std::vector<std::pair<const char*, std::function<void(joyeer::ir::Module&)>>> cases {
        { "a stack entry loaded as a value", [&](auto& module) {
              auto& body = bodyOf(module, "make");
              joyeer::ir::Instruction storage { Opcode::stackAllocate };
              storage.result = joyeer::ir::Value { 9000, literal.entry, joyeer::ir::ValueCategory::address };
              joyeer::ir::Instruction load { Opcode::load };
              load.operands = { 9000 };
              load.result = joyeer::ir::Value { 9001, literal.entry, joyeer::ir::ValueCategory::value };
              body.insert(body.begin(), { storage, load });
          } },
        { "an entry field", [&](auto& module) {
              structureWithRole(module, joyeer::ir::StructureRole::none, "Holder")->fields[0].type = literal.entry;
          } },
        { "an entry parameter", [&](auto& module) {
              functionExactly(module, "hold").parameters[0].value.type = literal.entry;
          } },
        { "an entry's clone callback", [&](auto& module) {
              auto& body = bodyOf(module, "make");
              joyeer::ir::Instruction callback { Opcode::cloneCallback };
              callback.integerValue = static_cast<int64_t>(literal.entry);
              callback.result = joyeer::ir::Value { 9000, integer, joyeer::ir::ValueCategory::value };
              body.insert(body.begin(), callback);
          } },
        { "an entry as a type argument", [&](auto& module) {
              auto table = *typeOf(module, literal.table);
              table.id = static_cast<joyeer::ir::TypeId>(module.types.size() + 100);
              table.name = "nested";
              table.arguments = { integer, literal.entry };
              module.types.push_back(table);
          } },
    };
    for (const auto& [name, mutate] : cases) {
        SCOPED_TRACE(name);
        auto copy = base;
        mutate(copy);
        const auto messages = joyeer::ir::dump(joyeer::ir::Verifier().verify(copy));
        EXPECT_NE(messages.find("a dictionary entry is never a value"), std::string::npos) << messages;
    }
}

TEST_F(DictRoleTest, APackedLiteralReachesLLVMInBothProfiles) {
    load(literalProgram);
    auto module = lower();
    ASSERT_NE(module, nullptr);
    packLiteral(*module, "make", 2);
    packLiteral(*module, "makeUnit", 2);
    packLiteral(*module, "makeEmpty", 0);
    for (const auto level : { joyeer::OptimizationLevel::O0, joyeer::OptimizationLevel::O2 }) {
        SCOPED_TRACE(static_cast<int>(level));
        joyeer::llvmbackend::EmitOptions options;
        options.optimizationLevel = level;
        const auto emitted = joyeer::llvmbackend::Emitter().emit(*module, options);
        EXPECT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
    }
}

TEST_F(DictRoleTest, MoreDictionaryRoleShapesAreClosed) {
    const auto with = [](std::string TableParts::*part, const std::string& from, const std::string& to) {
        TableParts parts;
        parts.*part = replaced(parts.*part, from, to);
        return parts.text();
    };
    // dict_get is `get`, unique, and returns exactly Optional<V>.
    EXPECT_TRUE(rejectedAsInvalidPrelude(with(&TableParts::get, "func get(", "func lookup("), "dict_get"));
    {
        TableParts parts;
        parts.get += replaced(parts.get, "func get(", "func again(");
        EXPECT_TRUE(rejectedAsInvalidPrelude(parts.text(), "dict_get"));
    }
    const auto plainGet = [](const std::string& result, const std::string& body) {
        TableParts parts;
        parts.get = "\n    @compiler_role(dict_get)\n    public borrowing func get(key: K): " + result + " {\n        " +
                    body + "\n    }\n";
        return parts.text();
    };
    EXPECT_TRUE(rejectedAsInvalidPrelude(plainGet("V", "return joyeer_intrinsic_move_out<V>(address: 0)"), "dict_get"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(plainGet("Optional<Optional<V>>", "return .None"), "dict_get"));
    // dict_copy takes no parameter; dict_element is borrowing; dict_literal returns exactly Self.
    EXPECT_TRUE(rejectedAsInvalidPrelude(with(&TableParts::copy, "duplicate()", "duplicate(extra: Int)"), "dict_copy"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(with(&TableParts::element, "private borrowing func element",
                                              "private mutating func element"),
                                         "dict_element"));
    {
        TableParts parts;
        parts.literal = "\n    @compiler_role(dict_literal)\n    private consuming func literal(address: Int, count: Int): "
                        "Table<V, K> {\n        return joyeer_intrinsic_move_out<Table<V, K>>(address: address)\n    }\n";
        EXPECT_TRUE(rejectedAsInvalidPrelude(parts.text(), "dict_literal"));
    }
    // An array method role on the dictionary.
    {
        TableParts parts;
        parts.extra = "";
        parts.tail = replaced(parts.tail, "deinit()",
                              "@compiler_role(container_end)\n    public consuming func finish() {}\n    deinit()");
        EXPECT_TRUE(rejectedAsInvalidPrelude(parts.text(), "container_end"));
    }
    // The entry: no methods, exactly two unconstrained type parameters.
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            with(&TableParts::entry, "var value: V\n", "var value: V\n    func touch(): Int { return 0 }\n"),
            "dict_entry"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(with(&TableParts::entry, "TableEntry<K, V>", "TableEntry<K, V: Copyable>"),
                                         "dict_entry"));
    // Neither struct takes the transfer marker: the dictionary is affine-only.
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            with(&TableParts::head, "@compiler_role(dict_type)", "@compiler_transfer()\n@compiler_role(dict_type)"),
            "dict_type"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(
            with(&TableParts::entry, "@compiler_role(dict_entry)", "@compiler_transfer()\n@compiler_role(dict_entry)"),
            "dict_entry"));
}

TEST_F(DictRoleTest, GetOfANoncopyableValueIsRejectedAtTheCall) {
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    load("struct Resource { var value: Int\n deinit() {} }\n"
         "func bad(table: borrowing Table<Int, Resource>): Resource? { return table.get(key: 1) }\n");
    EXPECT_TRUE(hasTypeDiagnostic(Id::copyOfNoncopyable)) << diagnostics();
    load("func good(table: borrowing Table<Int, [String]>): [String]? { return table.get(key: 1) }\n");
    EXPECT_TRUE(checks()) << diagnostics();
}

TEST_F(DictRoleTest, UserCodeCannotConstructTheRoleType) {
    load("func bad(): Table<Int, Int> { return Table(address: 0, count: 0, capacity: 0) }\n");
    EXPECT_FALSE(checks());
}

TEST_F(DictRoleTest, TheEntryIsInternedForADictionaryReachedOnlyThroughSubstitution) {
    load("struct Pair<A, B> { var first: A\n var second: B }\n"
         "struct Box<T> { var pair: Pair<T, T> }\n"
         "func probe<T>(value: T): Int { let none: Optional<Table<Int, Box<T> > > = .None\n return 0 }\n"
         "func main() { let answer = probe(value: 1)\n print(value: answer) }\n");
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    size_t tables = 0;
    size_t entries = 0;
    for (const auto& definition : module->structures) {
        tables += definition.role == dictRole;
        entries += definition.role == entryRole;
    }
    EXPECT_EQ(tables, 1u);
    EXPECT_EQ(entries, tables);
}

TEST_F(DictRoleTest, TheBackendEmitsTheHandleAndTheEntryLayoutForASpreadOfTypes) {
    // Every admitted key against values of each layout class: Void, UInt8, Int, String, a padded struct, an
    // optional and an enum. The backend checks each entry's field offsets against the runtime's formula.
    load("struct Padded { var small: UInt8\n var large: Int }\n"
         "enum Choice {\n    One,\n    Two(String),\n}\n"
         "func a(table: Table<Int, String>): Int { return table.count }\n"
         "func b(table: Table<String, Void>): Int { return table.count }\n"
         "func c(table: Table<UInt8, Int>): Int { return table.count }\n"
         "func d(table: Table<Bool, Padded>): Int { return table.count }\n"
         "func e(table: Table<Int, Int?>): Int { return table.count }\n"
         "func f(table: Table<UInt8, UInt8>): Int { return table.count }\n"
         "func g(table: Table<String, Choice>): Int { return table.count }\n"
         "func h(table: Table<Bool, Void>): Int { return table.count }\n"
         "func main() {}\n");
    const auto module = lower();
    ASSERT_NE(module, nullptr);
    size_t entries = 0;
    for (const auto& definition : module->structures) entries += definition.role == entryRole;
    EXPECT_EQ(entries, 8u);
    for (const auto level : { joyeer::OptimizationLevel::O0, joyeer::OptimizationLevel::O2 }) {
        SCOPED_TRACE(static_cast<int>(level));
        joyeer::llvmbackend::EmitOptions options;
        options.optimizationLevel = level;
        const auto emitted = joyeer::llvmbackend::Emitter().emit(*module, options);
        ASSERT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
        EXPECT_NE(emitted.text.find("{ i64, i64, i64 }"), std::string::npos);
        EXPECT_EQ(emitted.text.find("{ i64, i64, i64, i1 }"), std::string::npos) << "no live flag on the handle";
    }
}

} // namespace
