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

#include <gtest/gtest.h>

#include <algorithm>
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

// The runtime dictionary ABI the table is written over (decision 0027 option A).
constexpr auto tableExterns = R"(
@c_extern(symbol: "joyeer_dictionary_create_owned_abi")
private func createTable(output: Int, entries: Int, count: Int, keySize: Int, valueSize: Int,
        entrySize: Int, valueOffset: Int, keyKind: Int32, cloneKey: Int, destroyKey: Int,
        cloneValue: Int, destroyValue: Int): Void

@c_extern(symbol: "joyeer_dictionary_clone_abi")
private func cloneTable(output: Int, data: Int, count: Int): Void

@c_extern(symbol: "joyeer_dictionary_set_owned_abi")
private func setOwned(dictionary: Int, key: Int, value: Int): Void

@c_extern(symbol: "joyeer_dictionary_find_abi")
private func findValue(data: Int, count: Int, key: Int, keySize: Int, keyKind: Int32): Int

@c_extern(symbol: "joyeer_dictionary_at_abi")
private func valueAt(data: Int, count: Int, key: Int, keySize: Int, keyKind: Int32): Int

@c_extern(symbol: "joyeer_dictionary_destroy_abi")
private func destroyTable(dictionary: Int): Void

private func roundUp(value: Int, alignment: Int): Int {
    return (value + alignment - 1) / alignment * alignment
}
)";

// The interleaved entry a literal packs: its ordinary struct layout is the runtime's entry layout.
constexpr auto tableEntry = R"(
@compiler_role(dict_entry)
private struct TableEntry<K, V> {
    var key: K
    var value: V
}
)";

constexpr auto tableHead = R"(
@compiler_role(dict_type)
public struct Table<K, V> {
    private var address: Int
    private var count: Int
    private var capacity: Int
)";

constexpr auto tableElement = R"(
    @compiler_role(dict_element)
    private borrowing func element(key: K): Int {
        return valueAt(data: self.address, count: self.count,
                key: joyeer_intrinsic_borrow_address<K>(value: key), keySize: joyeer_intrinsic_size_of<K>(),
                keyKind: joyeer_intrinsic_key_kind<K>() as Int32)
    }
)";

constexpr auto tableSet = R"(
    @compiler_role(dict_set)
    private mutating func insert(key: consuming K, value: consuming V): Void {
        let keySlot = joyeer_intrinsic_stack_slot<K>()
        let valueSlot = joyeer_intrinsic_stack_slot<V>()
        joyeer_intrinsic_move_in<K>(address: keySlot, value: consume key)
        joyeer_intrinsic_move_in<V>(address: valueSlot, value: consume value)
        setOwned(dictionary: joyeer_intrinsic_place_address<Table<K, V>>(value: &self), key: keySlot,
                value: valueSlot)
    }
)";

constexpr auto tableGet = R"(
    @compiler_role(dict_get)
    public borrowing func get(key: K): Optional<V> {
        let value = findValue(data: self.address, count: self.count,
                key: joyeer_intrinsic_borrow_address<K>(value: key), keySize: joyeer_intrinsic_size_of<K>(),
                keyKind: joyeer_intrinsic_key_kind<K>() as Int32)
        if value == 0 {
            return .None
        }
        let slot = joyeer_intrinsic_stack_slot<V>()
        joyeer_intrinsic_clone_at<V>(destination: slot, source: value)
        return .Some(joyeer_intrinsic_move_out<V>(address: slot))
    }
)";

constexpr auto tableCopy = R"(
    @compiler_role(dict_copy)
    private borrowing func duplicate(): Table<K, V> {
        let slot = joyeer_intrinsic_stack_slot<Table<K, V>>()
        cloneTable(output: slot, data: self.address, count: self.count)
        return joyeer_intrinsic_move_out<Table<K, V>>(address: slot)
    }
)";

// Called on an inert zero value with `count` entries packed at `address` (the closed packing protocol).
constexpr auto tableLiteral = R"(
    @compiler_role(dict_literal)
    private consuming func literal(address: Int, count: Int): Table<K, V> {
        let valueOffset = roundUp(value: joyeer_intrinsic_size_of<K>(), alignment: joyeer_intrinsic_align_of<V>())
        let slot = joyeer_intrinsic_stack_slot<Table<K, V>>()
        createTable(output: slot, entries: address, count: count, keySize: joyeer_intrinsic_size_of<K>(),
                valueSize: joyeer_intrinsic_size_of<V>(), entrySize: joyeer_intrinsic_size_of<TableEntry<K, V>>(),
                valueOffset: valueOffset, keyKind: joyeer_intrinsic_key_kind<K>() as Int32,
                cloneKey: joyeer_intrinsic_clone_callback<K>(), destroyKey: joyeer_intrinsic_destroy_callback<K>(),
                cloneValue: joyeer_intrinsic_clone_callback<V>(),
                destroyValue: joyeer_intrinsic_destroy_callback<V>())
        return joyeer_intrinsic_move_out<Table<K, V>>(address: slot)
    }
)";

constexpr auto tableTail = R"(
    deinit() {
        destroyTable(dictionary: joyeer_intrinsic_place_address<Table<K, V>>(value: &self))
    }
}
)";

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
    EXPECT_TRUE(rejectedAsInvalidPrelude(withHead(replaced(tableHead, "Table<K, V>", "Table<K, V: Copyable>")),
                                         "dict_type"));
    EXPECT_TRUE(rejectedAsInvalidPrelude(withHead(replaced(tableHead, "Table<K, V>", "Table<K, V, W>")),
                                         "dict_type"));
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
    using Id = joyeer::typing::TypeCheckingDiagnosticId;
    load("func bad(table: borrowing Table<Int, JoinHandle<Int>>): Int { return table.count }\n");
    EXPECT_TRUE(hasTypeDiagnostic(Id::linearTypeArgument)) << diagnostics();
    load("func bad(table: borrowing Table<JoinHandle<Int>, Int>): Int { return table.count }\n");
    EXPECT_TRUE(hasTypeDiagnostic(Id::linearTypeArgument)) << diagnostics();
}

TEST_F(DictRoleTest, DictionarySyntaxStaysBuiltinUntilTheSwitch) {
    // Stage 5 adds the roles only: `[K: V]` and literals keep the builtin dictionary even when the prelude has the
    // role.
    load("func make(): [String: Int] { return [\"a\": 1] }\n"
         "func read(table: [String: Int]): Int { return table[\"a\"] }\n");
    EXPECT_TRUE(checks()) << diagnostics();
    EXPECT_TRUE(rejectedMentioning("func bad(table: [String: Int]): Table<String, Int> { return table }\n", "Table"))
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

TEST_F(DictRoleTest, TheWireCarriesTheDictionaryRolesAtVersion24) {
    EXPECT_EQ(joyeer::llvmbackend::wire::version, 24u);
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
