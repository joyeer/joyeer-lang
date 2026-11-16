// Characterization of the builtin array's diagnostics and type rules (plan 0011 phase 3, decision 0026).
// These pin today's behavior so the switch to a library `Array<T>` is checked against it. A test marked
// `CHANGES WITH 0026` pins something decision 0026 says changes; the switch updates it deliberately.
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

TEST(ArrayCharacterization, TypesPrintInSquareBrackets) {
    const auto text = diagnose("func main() { let x: [Int] = \"a\" }\n");
    EXPECT_TRUE(contains(text, "cannot use value of type 'String' where '[Int]' is required")) << text;
    EXPECT_TRUE(contains(text, "expected '[Int]' but found 'String'")) << text;
    const auto nested = diagnose("func main() { let a: [Int] = [1]\n let b: [String] = a }\n");
    EXPECT_TRUE(contains(nested, "cannot use value of type '[Int]' where '[String]' is required")) << nested;
    const auto literal = diagnose("func main() { let a: [Int] = [1, \"x\"] }\n");
    EXPECT_TRUE(contains(literal, "cannot use value of type 'String' where 'Int' is required")) << literal;
}

// CHANGES WITH 0026: the library's stored fields are inaccessible, not unknown members.
TEST(ArrayCharacterization, UnknownMembersAndMisuseOfCount) {
    const auto text = diagnose("func main() { let a: [Int] = [1]\n print(value: a.address) }\n");
    EXPECT_TRUE(contains(text, "inaccessible-declaration")) << text;
    EXPECT_TRUE(contains(text, "declaration 'address' is not accessible from this file")) << text;
    const auto capacity = diagnose("func main() { let a: [Int] = [1]\n print(value: a.capacity) }\n");
    EXPECT_TRUE(contains(capacity, "inaccessible-declaration")) << capacity;
    EXPECT_TRUE(contains(capacity, "declaration 'capacity' is not accessible from this file")) << capacity;
    const auto call = diagnose("func main() { let a: [Int] = [1]\n print(value: a.count()) }\n");
    EXPECT_TRUE(contains(call, "name-resolution.not-callable")) << call;
    EXPECT_TRUE(contains(call, "'count' is not callable")) << call;
}

// Stays the same after 0026: a user cannot extend an array, today because it is not declared in the module,
// afterwards because the library struct belongs to another module.
TEST(ArrayCharacterization, AnExtensionOfAnArrayIsRejected) {
    for (const auto* source : { "extension Array<Int> { }\nfunc main() {}\n", "extension [Int] { }\nfunc main() {}\n" }) {
        const auto text = diagnose(source);
        EXPECT_TRUE(contains(text, "name-resolution.invalid-extension-target")) << source << "\n" << text;
        EXPECT_TRUE(contains(text, "an extension must extend a struct or enum declared in the same module")) << text;
    }
}

TEST(ArrayCharacterization, ForInNeedsAnIterable) {
    const auto text = diagnose("func main() { for x in 5 { print(value: x) } }\n");
    EXPECT_TRUE(contains(text, "type-checking.not-iterable")) << text;
    EXPECT_TRUE(contains(text, "cannot iterate over a value of type 'Int'; 'for' needs an array, a string, or an "
                               "integer range ('a..<b' or 'a...b')")) << text;
}

// CHANGES WITH 0026: a user type named Array prints as `Array` today; afterwards it prints as `<main>.Array`
// because the prelude declares an `Array` too.
TEST(ArrayCharacterization, AUserTypeNamedArrayShadowsTheBuiltin) {
    const auto text = diagnose("struct Array { var x: Int }\n"
                               "func main() { let a = Array(x: 1)\n let b: Int = a }\n");
    EXPECT_TRUE(contains(text, "cannot use value of type '<main>.Array' where 'Int' is required")) << text;
}

TEST(ArrayCharacterization, OwnershipReasonsNameTheElement) {
    const auto copy = diagnose("struct Res { var id: Int\n deinit() {} }\n"
                               "func main() { let a: [Res] = [Res(id: 1)]\n let b = a }\n");
    EXPECT_TRUE(contains(copy, "type-checking.copy-of-noncopyable")) << copy;
    EXPECT_TRUE(contains(copy, "cannot copy a value of type '[Res]': '[Res]' holds the noncopyable type 'Res'"))
            << copy;
    EXPECT_TRUE(contains(copy, "help: move an owning local or consuming parameter with 'consume', borrow it, or "
                               "build a new value")) << copy;
    const auto linear = diagnose("func main() { let h: [JoinHandle<Int>] = [] }\n");
    EXPECT_TRUE(contains(linear, "semantic-analysis.linear-not-consumed")) << linear;
    EXPECT_TRUE(contains(linear, "'h' holds a linear value of type '[JoinHandle<Int>]' that is not consumed "
                                 "before the end of its block")) << linear;
    EXPECT_TRUE(contains(linear, "help: end each handle with its consuming 'join' method")) << linear;
}

TEST(ArrayCharacterization, LinearElementsCannotBeIterated) {
    const auto text = diagnose("func main() { var hs: [JoinHandle<Int>] = []\n for h in hs { } }\n");
    EXPECT_TRUE(contains(text, "type-checking.linear-collection-element")) << text;
    EXPECT_TRUE(contains(text, "cannot iterate over an array of linear elements")) << text;
    EXPECT_TRUE(contains(text, "help: drain it with removeLast instead")) << text;
}

TEST(ArrayCharacterization, MembersNeedTheirMarkersAndIndexTypes) {
    const auto marker = diagnose("func main() { var a: [Int] = [1]\n a.append(element: 2) }\n");
    EXPECT_TRUE(contains(marker, "type-checking.invalid-access-marker")) << marker;
    EXPECT_TRUE(contains(marker, "mutating method call requires '&' on the receiver")) << marker;
    const auto result = diagnose("func main() { var a: [Int] = []\n let n: Int = &a.append(element: 1) }\n");
    EXPECT_TRUE(contains(result, "cannot use value of type 'Void' where 'Int' is required")) << result;
    const auto index = diagnose("func main() { let a: [Int] = [1]\n print(value: a[\"x\"]) }\n");
    EXPECT_TRUE(contains(index, "cannot use value of type 'String' where 'Int' is required")) << index;
}

TEST(ArrayCharacterization, ConsumingAnElementOfANoncopyableTypeIsAccepted) {
    const auto text = diagnose("struct Res { var id: Int\n deinit() {} }\n"
                               "func use(value: consuming Res): Int { return value.id }\n"
                               "func main() { var a: [Res] = [Res(id: 1)]\n print(value: use(value: consume a[0])) }\n");
    EXPECT_EQ(text, "") << text;
}

TEST(ArrayCharacterization, NeverElementsAreAcceptedAndAnyIsNotSyntax) {
    EXPECT_EQ(diagnose("func f(a: [Never]) {}\nfunc main() {}\n"), "");
    const auto any = diagnose("func g(a: [Any]) {}\nfunc main() {}\n");
    EXPECT_TRUE(contains(any, "Syntax 'Any' is not supported")) << any;
}

TEST(ArrayCharacterization, FunctionValueElementsAreAccepted) {
    EXPECT_EQ(diagnose("func f(a: [(Int): Int]) {}\nfunc main() {}\n"), "");
}

TEST(ArrayCharacterization, EqualityOfLibraryArraysWaitsForEquatable) {
    const auto text = diagnose("func main() { let equal = [1] == [1]\n print(value: equal) }\n");
    EXPECT_TRUE(contains(text, "type-checking.invalid-operator-operands")) << text;
    EXPECT_TRUE(contains(text, "[Int]")) << text;
}

} // namespace
