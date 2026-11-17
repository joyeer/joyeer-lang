// Characterization of the builtin dictionary's diagnostics and type rules (plan 0011 phase 4, decision 0027,
// stage 1). These pin today's behavior so the switch to a library `Dict<K, V>` is checked against it. A test marked
// `CHANGES WITH 0027` pins something decision 0027 says changes; the switch updates it deliberately.
#include "compiler/frontend/lexparser.h"
#include "compiler/frontend/nameresolution.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/semanticanalysis.h"
#include "compiler/frontend/typechecking.h"

#include <gtest/gtest.h>

#include <string>

namespace {

// All diagnostics of a one-file program, from every stage that ran, as text.
std::string diagnose(const std::string& text) {
    Diagnostics lexerDiagnostics;
    LexParser lexer(&lexerDiagnostics);
    const auto source = std::make_shared<SourceFile>(text);
    lexer.parse(source);
    if (!lexerDiagnostics.errors.empty()) return "lexer: " + lexerDiagnostics.errors.front().message;
    const auto parsing = joyeer::parser::Parser(source->tokens).parse();
    EXPECT_TRUE(parsing.succeeded()) << joyeer::parser::dump(parsing.diagnostics);
    const auto resolution = joyeer::semantic::NameResolver().resolve(parsing.root);
    auto result = joyeer::semantic::dump(resolution.diagnostics);
    if (!resolution.succeeded()) return result;
    const auto checking = joyeer::typing::TypeChecker().check(resolution.model);
    for (const auto& diagnostic : checking.diagnostics) {
        result += joyeer::typing::dump({diagnostic});
        if (diagnostic.help.has_value()) result += "help: " + *diagnostic.help + "\n";
    }
    if (!checking.succeeded()) return result;
    const auto analysis = joyeer::analysis::Analyzer().analyze(checking.model);
    for (const auto& diagnostic : analysis.diagnostics) {
        result += joyeer::analysis::dump({diagnostic});
        if (diagnostic.help.has_value()) result += "help: " + *diagnostic.help + "\n";
    }
    return result;
}

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

TEST(DictCharacterization, TypesPrintAsKeyValuePairs) {
    const auto text = diagnose("func main() { let x: [String: Int] = \"a\" }\n");
    EXPECT_TRUE(contains(text, "cannot use value of type 'String' where '[String: Int]' is required")) << text;
    const auto key = diagnose("func main() { let d: [String: Int] = [\"a\": 1, 2: 3] }\n");
    EXPECT_TRUE(contains(key, "cannot use value of type 'Int' where 'String' is required")) << key;
    const auto read = diagnose("func main() { let d: [String: Int] = [\"a\": 1]\n let n: String = d[\"a\"] }\n");
    EXPECT_TRUE(contains(read, "cannot use value of type 'Int' where 'String' is required")) << read;
}

TEST(DictCharacterization, AnEmptyLiteralNeedsAContext) {
    const auto text = diagnose("func main() { let d = [:] }\n");
    EXPECT_TRUE(contains(text, "type-checking.missing-contextual-type")) << text;
    EXPECT_TRUE(contains(text, "empty dictionary literal requires contextual key and value types")) << text;
}

size_t occurrences(const std::string& text, const std::string& needle) {
    size_t count = 0;
    for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++count;
    return count;
}

// CHANGED WITH 0027 (Decision 4): a key type outside Int, Bool, UInt8 and String passed the checker and failed only in
// the backend when a dictionary was built, read or assigned; it is now rejected at the type, once per mistake: at an
// annotation's key, a nominal Dict's first argument, or a literal's first key (the whole literal when empty) when no
// context gave the key type.
TEST(DictCharacterization, OtherKeyTypesAreRejectedAtTheType) {
    const auto point = diagnose("struct Point { var x: Int }\nfunc f(d: [Point: Int]): Int { return 1 }\nfunc main() {}\n");
    EXPECT_TRUE(contains(point, "type-checking.invalid-type-argument")) << point;
    EXPECT_TRUE(contains(point, "'Point' cannot be a dictionary key")) << point;
    EXPECT_TRUE(contains(point, "keys are restricted to Int, Bool, UInt8 and String")) << point;
    EXPECT_EQ(occurrences(point, "' cannot be a dictionary key"), 1u) << point;
    for (const auto* program : {
            "func f(d: [Double: Int]) {}\nfunc main() {}\n",
            "func f(d: [Int32: Int]) {}\nfunc main() {}\n",
            "func f(d: [Int?: Int]) {}\nfunc main() {}\n",
            "func f(d: [[Int]: Int]) {}\nfunc main() {}\n",
            "struct Point { var x: Int }\nfunc f(d: Dict<Point, Int>) {}\nfunc main() {}\n",
            "func main() { let d = [1.5: 1]\n print(value: d.count) }\n",
            "func main() { let d: [Double: Int] = [1.5: 1]\n print(value: d.count) }\n",
            "func main() { let d: [Double: Int] = [:]\n print(value: d.count) }\n" }) {
        SCOPED_TRACE(program);
        const auto text = diagnose(program);
        EXPECT_EQ(occurrences(text, "' cannot be a dictionary key"), 1u) << text;
    }
    // The four admitted keys, by type kind.
    EXPECT_EQ(diagnose("func f(a: [Int: Int], b: [Bool: Int], c: [UInt8: Int], d: [String: Int]) {}\n"
                       "func main() {}\n"), "");
    // A function-type, type-parameter or Never key keeps its own diagnostic only.
    for (const auto* program : {
            "func f(d: [(Int): Int: Int]) {}\nfunc main() {}\n",
            "func f<K>(d: [K: Int]) {}\nfunc main() {}\n",
            "func f(d: [Never: Int]) {}\nfunc main() {}\n" }) {
        SCOPED_TRACE(program);
        const auto text = diagnose(program);
        EXPECT_FALSE(contains(text, "' cannot be a dictionary key")) << text;
        EXPECT_NE(text, "") << text;
    }
}

TEST(DictCharacterization, TypeParameterKeysAreRejected) {
    const auto parameter = diagnose("func f<K>(d: [K: Int]) {}\nfunc main() {}\n");
    EXPECT_TRUE(contains(parameter, "type-checking.invalid-type-argument")) << parameter;
    EXPECT_TRUE(contains(parameter, "a dictionary key cannot be the type parameter type 'K'")) << parameter;
    EXPECT_TRUE(contains(parameter, "keys are restricted to Int, Bool, UInt8 and String")) << parameter;
}

TEST(DictCharacterization, LinearAndNeverKeysAndValuesAreRejected) {
    const auto linear = diagnose("func f(d: [String: JoinHandle<Int>]) {}\nfunc main() {}\n");
    EXPECT_TRUE(contains(linear, "type-checking.linear-collection-element")) << linear;
    EXPECT_TRUE(contains(linear, "a dictionary cannot hold a linear key or value ('[String: JoinHandle<Int>]')"))
            << linear;
    EXPECT_TRUE(contains(linear, "use an array and drain it with removeLast")) << linear;
    const auto never = diagnose("func f(d: [String: Never]) {}\nfunc main() {}\n");
    EXPECT_TRUE(contains(never, "the value type of a dictionary cannot be 'Never'")) << never;
}

TEST(DictCharacterization, GetOfANoncopyableValueIsRejected) {
    const auto text = diagnose("struct Res { var id: Int\n deinit() {} }\n"
                               "func main() { let d: [String: Res] = [\"a\": Res(id: 1)]\n let g = d.get(key: \"a\") }\n");
    EXPECT_TRUE(contains(text, "type-checking.copy-of-noncopyable")) << text;
    EXPECT_TRUE(contains(text, "'get' returns a copy of the stored value, and 'Res' is noncopyable")) << text;
    EXPECT_TRUE(contains(text, "a noncopyable element cannot be read without removing it")) << text;
}

TEST(DictCharacterization, ANoncopyableValueIsNotCopiedOutAndAConsumedOneEndsTheDictionary) {
    const auto copy = diagnose("struct Res { var id: Int\n deinit() {} }\n"
                               "func main() { let d: [String: Res] = [\"a\": Res(id: 1)]\n let x = d[\"a\"]\n"
                               " print(value: x.id) }\n");
    EXPECT_TRUE(contains(copy, "type-checking.copy-of-noncopyable")) << copy;
    EXPECT_TRUE(contains(copy, "cannot copy a value of type 'Res': 'Res' declares 'deinit'")) << copy;
    const auto consumed = diagnose("struct Res { var id: Int\n deinit() {} }\n"
                                   "func take(r: consuming Res): Int { return r.id }\n"
                                   "func main() { var d: [String: Res] = [\"a\": Res(id: 1)]\n"
                                   " print(value: take(r: consume d[\"a\"]))\n print(value: d.count) }\n");
    EXPECT_TRUE(contains(consumed, "semantic-analysis.use-after-consume")) << consumed;
}

TEST(DictCharacterization, CompoundAssignmentToAnElementIsRejected) {
    const auto text = diagnose("func main() { var d: [String: Int] = [\"a\": 1]\n &d[\"a\"] += 1 }\n");
    EXPECT_TRUE(contains(text, "type-checking.invalid-operator-operands")) << text;
    EXPECT_TRUE(contains(text, "compound assignment to a dictionary element is not supported; read it with 'get' and "
                               "assign the result")) << text;
}

TEST(DictCharacterization, ADictionaryIsNotIterable) {
    const auto text = diagnose("func main() { let d: [String: Int] = [\"a\": 1]\n for x in d { } }\n");
    EXPECT_TRUE(contains(text, "type-checking.not-iterable")) << text;
    EXPECT_TRUE(contains(text, "cannot iterate over a value of type '[String: Int]'")) << text;
}

// CHANGES WITH 0027: the library's stored fields are inaccessible, not unknown members.
TEST(DictCharacterization, UnknownMembers) {
    const auto text = diagnose("func main() { var d: [String: Int] = [\"a\": 1]\n print(value: d.address) }\n");
    EXPECT_TRUE(contains(text, "name-resolution.unknown-member")) << text;
    EXPECT_TRUE(contains(text, "type 'Dict' has no member named 'address'")) << text;
}

// CHANGES WITH 0027: a user type named Dict captures the member lookup of `[K: V]` today; afterwards `[K: V]` binds to
// the role and the user type only shadows the spelling.
TEST(DictCharacterization, AUserTypeNamedDictCapturesTheSugar) {
    const auto text = diagnose("struct Dict { var x: Int }\n"
                               "func main() { let m: [String: Int] = [\"a\": 1]\n print(value: m.count) }\n");
    EXPECT_TRUE(contains(text, "type 'Dict' has no member named 'count'")) << text;
}

TEST(DictCharacterization, ExtensionsOfDictAreRejected) {
    const auto text = diagnose("extension Dict { }\nfunc main() { }\n");
    EXPECT_TRUE(contains(text, "name-resolution.invalid-extension-target")) << text;
}

// CHANGES WITH 0027: equality of dictionaries type-checks today and fails in LLVM; afterwards the ordinary struct rule
// rejects it in the checker, as for arrays. Only the checker is exercised here: compiling it natively asserts in a
// Debug LLVM build.
TEST(DictCharacterization, EqualityTypeChecks) {
    const auto text = diagnose("func main() { let a: [Int: Int] = [1: 1]\n let e = a == a\n print(value: e) }\n");
    EXPECT_FALSE(contains(text, "error")) << text;
}

TEST(DictCharacterization, NativeBindingsTakeNoDictionary) {
    const auto text = diagnose("@c_extern(symbol: \"ext\")\nfunc ext(d: [String: Int]): Void\nfunc main() {}\n");
    EXPECT_TRUE(contains(text, "type-checking.unsupported-native-signature")) << text;
}

TEST(DictCharacterization, AnyIsNotSyntax) {
    EXPECT_TRUE(contains(diagnose("func f(d: [String: Any]) {}\nfunc main() {}\n"), "Syntax 'Any' is not supported"));
    EXPECT_TRUE(contains(diagnose("func f(d: [Any: Int]) {}\nfunc main() {}\n"), "Syntax 'Any' is not supported"));
}

} // namespace
