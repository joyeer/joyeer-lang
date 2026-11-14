#include "compiler/codegen/llvm.h"
#include "compiler/frontend/lexparser.h"
#include "compiler/frontend/nameresolution.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/semanticanalysis.h"
#include "compiler/frontend/typechecking.h"
#include "compiler/ir/irlowering.h"
#include "compiler/backend/ir_wire.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr auto identitySource = R"(
@c_extern(symbol: "native_identity")
func nativeIdentity(value: borrowing Int): Int

@c_export(symbol: "runtime_identity")
func runtimeIdentity(input value: Int): Int {
    return nativeIdentity(value: value)
}
)";

class CInteropTest : public testing::Test {
protected:
    joyeer::syntax::SourceFileSyntax::Ptr parse(
            const std::string& text, uint32_t sourceId = 0, bool intrinsicTypeArguments = true,
            bool preludeAnnotations = false) {
        source = std::make_shared<SourceFile>(text);
        Diagnostics diagnostics;
        LexParser(&diagnostics).parse(source);
        EXPECT_FALSE(diagnostics.hasFailure());
        for (auto& token : source->tokens) token->span.sourceId = sourceId;
        auto result = joyeer::parser::Parser(source->tokens, intrinsicTypeArguments, preludeAnnotations).parse();
        EXPECT_TRUE(result.succeeded()) << joyeer::parser::dump(result.diagnostics);
        return result.root;
    }

    void check(const std::string& text, bool runtimeContext = true) {
        checkModules({{"runtime.fixture", {parse(text)}, runtimeContext}});
    }

    void checkModules(const std::vector<joyeer::semantic::ModuleInput>& modules) {
        const auto resolution = joyeer::semantic::NameResolver().resolve(modules);
        ASSERT_TRUE(resolution.succeeded()) << joyeer::semantic::dump(resolution.diagnostics);
        checking = joyeer::typing::TypeChecker().check(resolution.model);
    }

    void checkPrelude(const std::string& text) {
        const auto root = parse("func main() {}\n", 0);
        const auto library = parse(text, 1, true, true);
        checkModules({{"app", {root}}, {"runtime.fixture", {library}, true, false, true}});
    }

    bool has(joyeer::typing::TypeCheckingDiagnosticId id) const {
        return std::any_of(checking.diagnostics.begin(), checking.diagnostics.end(),
                          [id](const auto& diagnostic) { return diagnostic.id == id; });
    }

    void lower(const std::string& text) {
        check(text);
        ASSERT_NE(checking.model, nullptr);
        ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
        const auto analysis = joyeer::analysis::Analyzer().analyze(checking.model);
        ASSERT_TRUE(analysis.succeeded()) << joyeer::analysis::dump(analysis.diagnostics);
        auto result = joyeer::lowering::Lowerer().lower(
                checking.model, "fixture.joyeer",
                joyeer::ir::SourceInfo {
                    "fixture.joyeer", ".", source->content.size(), source->lineStarts,
                });
        ASSERT_TRUE(result.succeeded()) << joyeer::lowering::dump(result.diagnostics);
        module = result.module;
        module->isRuntimeObject = true;
    }

    joyeer::ir::Function& function(const std::string& name) {
        const auto found = std::find_if(module->functions.begin(), module->functions.end(),
                [&](const auto& value) { return value.name == name; });
        EXPECT_NE(found, module->functions.end()) << name;
        return module->functions.at(std::distance(module->functions.begin(), found));
    }

    SourceFile::Ptr source;
    joyeer::typing::TypeCheckingResult checking;
    std::shared_ptr<joyeer::ir::Module> module;
};

TEST_F(CInteropTest, PreservesSourceLabelsAndImportBodyAbsence) {
    check(identitySource);
    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    const auto& root = checking.model->semanticModel()->root();
    ASSERT_EQ(root->items.size(), 2u);
    const auto imported = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(root->items[0]);
    const auto exported = std::static_pointer_cast<joyeer::syntax::FunctionDeclSyntax>(root->items[1]);
    ASSERT_EQ(imported->attributes.size(), 1u);
    EXPECT_EQ(imported->attributes[0].name->rawValue, "c_extern");
    EXPECT_EQ(imported->body, nullptr);
    EXPECT_NE(exported->body, nullptr);
    EXPECT_EQ(exported->parameters[0]->label->rawValue, "input");
    EXPECT_EQ(exported->parameters[0]->name->rawValue, "value");
}

TEST_F(CInteropTest, RejectsOrdinarySourceEvenWithRuntimeLookingNames) {
    check(identitySource, false);
    EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::unauthorizedNativeBinding));
}

TEST_F(CInteropTest, RejectsInvalidAnnotationArgumentsAndUnknownAnnotations) {
    for (const auto& annotation : std::vector<std::string> {
            "@c_extern()", "@c_extern(\"native\")",
            "@c_extern(symbol: \"native\", abi: \"C\")",
            "@c_extern(symbol: \"native\", symbol: \"other\")",
            "@c_extern(abi: \"C\")", "@c_extern(symbol: 1)",
            "@c_extern(symbol: \"\")", "@c_extern(symbol: \"bad-name\")",
            "@c_extern(symbol: \"with\\0nul\")",
            "@c_extern(symbol: \"native\" + \"_suffix\")",
            "@c_extern(symbol: name)",
            "@spec(symbol: \"native\")", "@unknown(symbol: \"native\")",
            "@c_extern(symbol: \"native\")\n@c_export(symbol: \"native\")",
            "@c_extern(symbol: \"native\")\n@c_extern(symbol: \"native\")",
        }) {
        SCOPED_TRACE(annotation);
        check(annotation + "\nfunc native(): Int\n");
        EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::invalidAnnotation))
                << joyeer::typing::dump(checking.diagnostics);
    }
}

TEST_F(CInteropTest, RejectsWrongBodiesAndImplicitUnitResults) {
    for (const auto& text : std::vector<std::string> {
            "@c_extern(symbol: \"native\")\nfunc native(): Int { return 1 }\n",
            "@c_export(symbol: \"native\")\nfunc native(): Int\n",
            "@c_extern(symbol: \"native\")\nfunc native()\n",
            "@c_export(symbol: \"native\")\nfunc native() {}\n",
        }) {
        SCOPED_TRACE(text);
        check(text);
        EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::invalidNativeDeclaration));
    }
}

TEST_F(CInteropTest, RejectsUnsupportedNativeTypesAndAccessEffects) {
    for (const auto& parameter : std::vector<std::string> {
            "Void", "()", "Int?", "[Int]", "[[[UInt8]]]", "[Bool]",
            "Result<Int, IOError>", "inout Int", "consuming Int", "initializing Int",
            "inout [UInt8]", "consuming String", "initializing [[UInt8]]",
        }) {
        SCOPED_TRACE(parameter);
        check("@c_extern(symbol: \"native\")\nfunc native(value: " + parameter + "): Int\n");
        EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::unsupportedNativeSignature));
    }
    // Never is accepted as a result (it marks a C function that does not return).
    for (const auto& result : {"String", "Int?", "[Int]"}) {
        SCOPED_TRACE(result);
        check("@c_extern(symbol: \"native\")\nfunc native(): " + std::string(result) + "\n");
        EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::unsupportedNativeSignature));
    }
}

TEST_F(CInteropTest, ImportsBorrowedBuffersAndInitializesOwnedOutputs) {
    ASSERT_NO_FATAL_FAILURE(lower(R"(
@c_extern(symbol: "native_buffers")
func nativeBuffers(text: String, bytes: [UInt8], names: [String], contents: [[UInt8]],
    output: initializing [UInt8], name: initializing String, listing: initializing [String]): Int
func use(text: String, bytes: [UInt8], names: [String], contents: [[UInt8]]): Int {
    var output: [UInt8]
    var name: String
    var listing: [String]
    let status = nativeBuffers(text: text, bytes: bytes, names: names, contents: contents,
        output: &output, name: &name, listing: &listing)
    return status + output.count + name.count + listing.count
}
)"));
    joyeer::llvmbackend::wire::Writer writer;
    writer(*module);
    joyeer::ir::Module decoded;
    joyeer::llvmbackend::wire::Reader reader(writer.bytes);
    reader(decoded);
    ASSERT_TRUE(reader.succeeded());
    ASSERT_TRUE(joyeer::ir::Verifier().verify(decoded).succeeded());
    const auto emitted = joyeer::llvmbackend::Emitter().emit(decoded);
    ASSERT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
    EXPECT_NE(emitted.text.find("@native_buffers(ptr, i64, ptr, i64, ptr, i64, ptr, i64, ptr, ptr, ptr)"),
            std::string::npos);
    EXPECT_NE(emitted.text.find("call void @joyeer_native_bytes_init"), std::string::npos);
    EXPECT_NE(emitted.text.find("call void @joyeer_native_strings_init"), std::string::npos);
}

TEST_F(CInteropTest, RejectsBufferExportsAndConflictingBufferShapesOrAccess) {
    check("@c_export(symbol: \"exported\")\nfunc exported(value: String): Void {}\n");
    EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::unsupportedNativeSignature));
    for (const auto& other : {"[UInt8]", "[String]", "initializing String"}) {
        check("@c_extern(symbol: \"native\")\nfunc first(value: String): Void\n"
              "@c_extern(symbol: \"native\")\nfunc second(value: " + std::string(other) + "): Void\n");
        EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::conflictingNativeSymbol));
    }
}

TEST_F(CInteropTest, VerifierRejectsInvalidBufferAccessAndConflictingAdapters) {
    ASSERT_NO_FATAL_FAILURE(lower(R"(
@c_extern(symbol: "native_borrow")
func borrowed(bytes: [UInt8]): Void
@c_extern(symbol: "native_output")
func output(bytes: initializing [UInt8]): Void
)"));
    ASSERT_TRUE(joyeer::ir::Verifier().verify(*module).succeeded());
    auto& borrowed = function("borrowed");
    auto& output = function("output");
    const auto original = output.parameters[0];
    output.parameters[0].isMutable = false;
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*module).succeeded());
    output.parameters[0] = original;
    output.parameters[0].isConsuming = true;
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*module).succeeded());
    output.parameters[0] = original;
    output.parameters[0].value.category = joyeer::ir::ValueCategory::value;
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*module).succeeded());
    output.parameters[0] = original;
    borrowed.parameters[0].isMutable = true;
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*module).succeeded());
    borrowed.parameters[0].isMutable = false;
    output.nativeBinding->symbol = borrowed.nativeBinding->symbol;
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*module).succeeded());
}

TEST_F(CInteropTest, ImportsBareBytePointersWithoutACount) {
    ASSERT_NO_FATAL_FAILURE(lower(R"(
@c_extern(symbol: "native_pointers", pointer: "text", pointer: "output")
func nativePointers(text: [UInt8], limit: Int, output: inout [UInt8]): Int
func use(text: [UInt8]): Int {
    var output: [UInt8] = []
    return nativePointers(text: text, limit: 4, output: &output)
}
)"));
    const auto& imported = function("nativePointers");
    ASSERT_TRUE(imported.nativeBinding.has_value());
    EXPECT_EQ(imported.nativeBinding->pointerParameters, (std::vector<uint32_t> {0, 2}));
    EXPECT_NE(joyeer::ir::dump(*module).find("pointer#0 pointer#2"), std::string::npos);
    joyeer::llvmbackend::wire::Writer writer;
    writer(*module);
    joyeer::ir::Module decoded;
    joyeer::llvmbackend::wire::Reader reader(writer.bytes);
    reader(decoded);
    ASSERT_TRUE(reader.succeeded());
    EXPECT_EQ(joyeer::ir::dump(decoded), joyeer::ir::dump(*module));
    ASSERT_TRUE(joyeer::ir::Verifier().verify(decoded).succeeded());
    const auto emitted = joyeer::llvmbackend::Emitter().emit(decoded);
    ASSERT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
    // A bare pointer contributes one C parameter, not a pointer and a count.
    EXPECT_NE(emitted.text.find("@native_pointers(ptr, i64, ptr)"), std::string::npos);
}

TEST_F(CInteropTest, RejectsMisusedPointerAnnotations) {
    for (const auto& declaration : std::vector<std::string> {
            "@c_extern(symbol: \"n\", pointer: \"missing\")\nfunc f(bytes: [UInt8]): Void\n",
            "@c_extern(symbol: \"n\", pointer: \"bytes\", pointer: \"bytes\")\nfunc f(bytes: [UInt8]): Void\n",
            "@c_extern(symbol: \"n\", pointer: \"text\")\nfunc f(text: String): Void\n",
            "@c_extern(symbol: \"n\", pointer: \"value\")\nfunc f(value: Int): Void\n",
            "@c_extern(symbol: \"n\", pointer: \"names\")\nfunc f(names: [String]): Void\n",
            "@c_extern(symbol: \"n\", pointer: \"out\")\nfunc f(out: initializing [UInt8]): Void\n",
            "@c_extern(symbol: \"n\", pointer: \"bytes\")\nfunc f(bytes: consuming [UInt8]): Void\n",
            "@c_extern(symbol: \"n\", pointer: 3)\nfunc f(bytes: [UInt8]): Void\n",
            "@c_extern(symbol: \"n\", extra: \"bytes\")\nfunc f(bytes: [UInt8]): Void\n",
            "@c_extern(symbol: \"n\", \"bytes\")\nfunc f(bytes: [UInt8]): Void\n",
            "@c_extern(symbol: \"n\")\nfunc f(bytes: inout [UInt8]): Void\n" }) {
        check(declaration);
        EXPECT_FALSE(checking.succeeded()) << declaration;
    }
    check("@c_export(symbol: \"n\", pointer: \"bytes\")\nfunc f(bytes: [UInt8]): Void {}\n");
    EXPECT_FALSE(checking.succeeded());
    // Imports that agree on which buffers are bare pointers may share a symbol.
    check("@c_extern(symbol: \"n\", pointer: \"bytes\")\nfunc first(bytes: [UInt8]): Void\n"
          "@c_extern(symbol: \"n\", pointer: \"data\")\nfunc second(data: [UInt8]): Void\n");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    check("@c_extern(symbol: \"n\", pointer: \"bytes\")\nfunc first(bytes: [UInt8]): Void\n"
          "@c_extern(symbol: \"n\")\nfunc second(data: [UInt8]): Void\n");
    EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::conflictingNativeSymbol));
}

TEST_F(CInteropTest, VerifierRejectsMalformedPointerMetadata) {
    ASSERT_NO_FATAL_FAILURE(lower(R"(
@c_extern(symbol: "native_pointer_borrow", pointer: "bytes")
func borrowed(bytes: [UInt8]): Void
@c_extern(symbol: "native_pointer_mutable", pointer: "bytes")
func mutable(bytes: inout [UInt8]): Void
@c_extern(symbol: "native_pointer_scalar")
func scalar(value: Int): Void
)"));
    ASSERT_TRUE(joyeer::ir::Verifier().verify(*module).succeeded());
    auto& borrowed = function("borrowed");
    auto& mutable_ = function("mutable");
    auto& scalar = function("scalar");
    scalar.nativeBinding->pointerParameters = {0};
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*module).succeeded());
    scalar.nativeBinding->pointerParameters.clear();
    borrowed.nativeBinding->pointerParameters = {1};
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*module).succeeded());
    borrowed.nativeBinding->pointerParameters = {0};
    ASSERT_TRUE(joyeer::ir::Verifier().verify(*module).succeeded());
    borrowed.parameters[0].isMutable = true;
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*module).succeeded());
    borrowed.parameters[0].isMutable = false;
    mutable_.parameters[0].isMutable = false;
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*module).succeeded());
    mutable_.parameters[0].isMutable = true;
    mutable_.parameters[0].isConsuming = true;
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*module).succeeded());
    mutable_.parameters[0].isConsuming = false;
    // A symbol shared by imports must agree on bare pointers.
    mutable_.nativeBinding->symbol = borrowed.nativeBinding->symbol;
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*module).succeeded());
}

TEST_F(CInteropTest, LowersCompilerIntrinsicsInlineWithoutDeclaringCFunctions) {
    ASSERT_NO_FATAL_FAILURE(lower(R"(
@c_extern(symbol: "joyeer_intrinsic_address_of", pointer: "bytes")
func addressOf(bytes: inout [UInt8]): Int
@c_extern(symbol: "joyeer_intrinsic_load_u16")
func load16(address: Int): UInt16
@c_extern(symbol: "joyeer_intrinsic_store_u32")
func store32(address: Int, value: UInt32): Void
@c_extern(symbol: "joyeer_intrinsic_atomic_add")
func atomicAdd(address: Int, delta: Int): Int
func use(): Int {
    var bytes: [UInt8] = [0, 0, 0, 0, 0, 0, 0, 0]
    let location = addressOf(bytes: &bytes)
    store32(address: location, value: 7)
    return atomicAdd(address: location, delta: 1) + (load16(address: location) as Int)
}
)"));
    joyeer::llvmbackend::wire::Writer writer;
    writer(*module);
    joyeer::ir::Module decoded;
    joyeer::llvmbackend::wire::Reader reader(writer.bytes);
    reader(decoded);
    ASSERT_TRUE(reader.succeeded());
    ASSERT_TRUE(joyeer::ir::Verifier().verify(decoded).succeeded());
    const auto emitted = joyeer::llvmbackend::Emitter().emit(decoded);
    ASSERT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
    EXPECT_NE(emitted.text.find("ptrtoint ptr"), std::string::npos);
    EXPECT_NE(emitted.text.find("load i16, ptr"), std::string::npos);
    EXPECT_NE(emitted.text.find("store i32"), std::string::npos);
    EXPECT_NE(emitted.text.find("atomicrmw add ptr"), std::string::npos);
    EXPECT_EQ(emitted.text.find("@joyeer_intrinsic"), std::string::npos);
}

// Core units (spec 19.4.6) may use only scalars, because everything else lowers
// to a call into the runtime that they are part of.
TEST_F(CInteropTest, AcceptsScalarOnlyCodeInACoreUnit) {
    checkModules({{"runtime.core", {parse(R"(
@c_extern(symbol: "joyeer_intrinsic_load_u8")
func loadByte(address: Int): UInt8
let limit: Int = 4
func sum(address: Int, count: Int): Int {
    var total = 0
    var index = 0
    while index < count && index < limit {
        total = total + (loadByte(address: address + index) as Int)
        index = index + 1
    }
    return total
}
func ready(flag: Bool, ratio: Double): Bool { return flag && ratio > 0.5 }
)")}, true, true}});
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(CInteropTest, RejectsEveryManagedTypeInACoreUnit) {
    for (const auto& source : std::vector<std::string> {
            "func f(text: String): Int { return 0 }\n",
            "func f(): Int { let text = \"x\"\n return 0 }\n",
            "func f(): Int { let values = [1, 2]\n return 0 }\n",
            "func f(): Int { let values: [Int: Int] = [:]\n return 0 }\n",
            "func f(): Int? { return nil }\n",
            "func f(): Result<Int, Int> { return .Ok(1) }\n",
            "struct Pair { var left: Int }\nfunc f(): Int { let pair = Pair(left: 1)\n return pair.left }\n",
            "enum Mode { Fast, Slow }\nfunc f(): Int { let mode = Mode.Fast\n return 0 }\n",
            "func f() { print(value: 1) }\n",
            "let table: [UInt8] = [1, 2]\n",
            "func f(): String { return \"x\" }\n" }) {
        checkModules({{"runtime.core", {parse(source)}, true, true}});
        EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::managedTypeInCoreUnit)) << source;
    }
}

TEST_F(CInteropTest, AllowsAStringLiteralOnlyAsTheArgumentOfTheConstantAddressIntrinsic) {
    // A core unit names a message with the intrinsic and builds no String.
    checkModules({{"runtime.core", {parse(R"(
@c_extern(symbol: "joyeer_intrinsic_literal_address")
func messageAddress(text: String): Int
func message(): Int { return messageAddress(text: "boom") }
)")}, true, true}});
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    // Any other String in a core unit is still rejected.
    checkModules({{"runtime.core", {parse(R"(
@c_extern(symbol: "joyeer_intrinsic_literal_address")
func messageAddress(text: String): Int
func message(): Int {
    let text = "boom"
    return messageAddress(text: "boom") + text.count
}
)")}, true, true}});
    EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::managedTypeInCoreUnit));
}

TEST_F(CInteropTest, RequiresALiteralForTheConstantAddressIntrinsic) {
    check(R"(
@c_extern(symbol: "joyeer_intrinsic_literal_address")
func messageAddress(text: String): Int
func message(text: String): Int { return messageAddress(text: text) }
)");
    EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::invalidCallArguments));
    // Wrong shapes are not the intrinsic.
    for (const auto& declaration : std::vector<std::string> {
            "@c_extern(symbol: \"joyeer_intrinsic_literal_address\")\nfunc f(text: Int): Int\n",
            "@c_extern(symbol: \"joyeer_intrinsic_literal_address\")\nfunc f(text: String): Int32\n",
            "@c_extern(symbol: \"joyeer_intrinsic_literal_address\")\nfunc f(text: String, extra: Int): Int\n" }) {
        check(declaration);
        EXPECT_FALSE(checking.succeeded()) << declaration;
    }
}

TEST_F(CInteropTest, LowersTheConstantAddressToNulTerminatedReadOnlyData) {
    ASSERT_NO_FATAL_FAILURE(lower(R"(
@c_extern(symbol: "joyeer_intrinsic_literal_address")
func messageAddress(text: String): Int
func message(): Int { return messageAddress(text: "boom") }
)"));
    joyeer::llvmbackend::wire::Writer writer;
    writer(*module);
    joyeer::ir::Module decoded;
    joyeer::llvmbackend::wire::Reader reader(writer.bytes);
    reader(decoded);
    ASSERT_TRUE(reader.succeeded());
    ASSERT_TRUE(joyeer::ir::Verifier().verify(decoded).succeeded());
    const auto emitted = joyeer::llvmbackend::Emitter().emit(decoded);
    ASSERT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
    EXPECT_NE(emitted.text.find("c\"boom\\00\""), std::string::npos);
    EXPECT_NE(emitted.text.find("ptrtoint ptr @.joyeer.panic."), std::string::npos);
    EXPECT_EQ(emitted.text.find("@joyeer_intrinsic"), std::string::npos);
}

TEST_F(CInteropTest, RequiresALiteralForTheSymbolAddressIntrinsic) {
    checkModules({{"runtime.core", {parse(R"(
@c_extern(symbol: "joyeer_intrinsic_symbol_address")
func addressOfSymbol(name: String): Int
func callback(): Int { return addressOfSymbol(name: "joyeer_panic") }
)")}, true, true}});
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    check(R"(
@c_extern(symbol: "joyeer_intrinsic_symbol_address")
func addressOfSymbol(name: String): Int
func callback(name: String): Int { return addressOfSymbol(name: name) }
)");
    EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::invalidCallArguments));
    check("@c_extern(symbol: \"joyeer_intrinsic_symbol_address\")\nfunc f(name: String): Int32\n");
    EXPECT_FALSE(checking.succeeded());
}

TEST_F(CInteropTest, LowersTheSymbolAddressToTheNamedFunctionOrAnExternalSymbol) {
    ASSERT_NO_FATAL_FAILURE(lower(R"(
@c_extern(symbol: "joyeer_intrinsic_symbol_address")
func addressOfSymbol(name: String): Int
@c_export(symbol: "local_callback")
public func local(cell: Int): Void {}
func callbacks(): Int { return addressOfSymbol(name: "local_callback") + addressOfSymbol(name: "other_object") }
)"));
    joyeer::llvmbackend::wire::Writer writer;
    writer(*module);
    joyeer::ir::Module decoded;
    joyeer::llvmbackend::wire::Reader reader(writer.bytes);
    reader(decoded);
    ASSERT_TRUE(reader.succeeded());
    ASSERT_TRUE(joyeer::ir::Verifier().verify(decoded).succeeded());
    const auto emitted = joyeer::llvmbackend::Emitter().emit(decoded);
    ASSERT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
    EXPECT_NE(emitted.text.find("ptrtoint ptr @local_callback to i64"), std::string::npos);
    EXPECT_NE(emitted.text.find("ptrtoint ptr @other_object to i64"), std::string::npos);
    EXPECT_EQ(emitted.text.find("@joyeer_intrinsic"), std::string::npos);
}

TEST_F(CInteropTest, LowersFloatingPointBitPatternsToBitCasts) {
    ASSERT_NO_FATAL_FAILURE(lower(R"(
@c_extern(symbol: "joyeer_intrinsic_double_bits")
func doubleBits(value: Double): UInt64
@c_extern(symbol: "joyeer_intrinsic_float_bits")
func floatBits(value: Float): UInt32
func bits(wide: Double, narrow: Float): UInt64 { return doubleBits(value: wide) + (floatBits(value: narrow) as UInt64) }
)"));
    joyeer::llvmbackend::wire::Writer writer;
    writer(*module);
    joyeer::ir::Module decoded;
    joyeer::llvmbackend::wire::Reader reader(writer.bytes);
    reader(decoded);
    ASSERT_TRUE(reader.succeeded());
    ASSERT_TRUE(joyeer::ir::Verifier().verify(decoded).succeeded());
    const auto emitted = joyeer::llvmbackend::Emitter().emit(decoded);
    ASSERT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
    EXPECT_NE(emitted.text.find("bitcast double"), std::string::npos);
    EXPECT_NE(emitted.text.find("bitcast float"), std::string::npos);
    EXPECT_EQ(emitted.text.find("@joyeer_intrinsic"), std::string::npos);
}

TEST_F(CInteropTest, RejectsFloatingPointBitPatternsOfTheWrongType) {
    for (const auto& declaration : std::vector<std::string> {
            "@c_extern(symbol: \"joyeer_intrinsic_double_bits\")\nfunc f(value: Float): UInt64\n",
            "@c_extern(symbol: \"joyeer_intrinsic_double_bits\")\nfunc f(value: Double): Int\n",
            "@c_extern(symbol: \"joyeer_intrinsic_float_bits\")\nfunc f(value: Double): UInt32\n" }) {
        check(declaration);
        EXPECT_FALSE(checking.succeeded()) << declaration;
    }
}

TEST_F(CInteropTest, ChecksTheSystemCallAndArchitectureIntrinsics) {
    check(R"(
@c_extern(symbol: "joyeer_intrinsic_syscall")
func call(number: Int, first: Int, second: Int, third: Int, fourth: Int, fifth: Int, sixth: Int): Int
@c_extern(symbol: "joyeer_intrinsic_target_architecture")
func architecture(): Int
func use(): Int { return call(number: 1, first: 2, second: 3, third: 4, fourth: 5, fifth: 6, sixth: 7) + architecture() }
)");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    for (const auto& declaration : std::vector<std::string> {
            "@c_extern(symbol: \"joyeer_intrinsic_syscall\")\nfunc f(number: Int): Int\n",
            "@c_extern(symbol: \"joyeer_intrinsic_syscall\")\nfunc f(number: Int, first: Int, second: Int, third: Int, "
            "fourth: Int, fifth: Int, sixth: Int): Int32\n",
            "@c_extern(symbol: \"joyeer_intrinsic_target_architecture\")\nfunc f(first: Int): Int\n",
            "@c_extern(symbol: \"joyeer_intrinsic_target_architecture\")\nfunc f(): Int32\n" }) {
        check(declaration);
        EXPECT_FALSE(checking.succeeded()) << declaration;
    }
}

TEST_F(CInteropTest, LowersTheArchitectureToAConstantAndTheSystemCallToInlineAssembly) {
    ASSERT_NO_FATAL_FAILURE(lower(R"(
@c_extern(symbol: "joyeer_intrinsic_syscall")
func call(number: Int, first: Int, second: Int, third: Int, fourth: Int, fifth: Int, sixth: Int): Int
@c_extern(symbol: "joyeer_intrinsic_target_architecture")
func architecture(): Int
func use(): Int { return call(number: 1, first: 2, second: 3, third: 4, fourth: 5, fifth: 6, sixth: 7) + architecture() }
)"));
    joyeer::llvmbackend::wire::Writer writer;
    writer(*module);
    joyeer::ir::Module decoded;
    joyeer::llvmbackend::wire::Reader reader(writer.bytes);
    reader(decoded);
    ASSERT_TRUE(reader.succeeded());
    ASSERT_TRUE(joyeer::ir::Verifier().verify(decoded).succeeded());
    const auto emitted = joyeer::llvmbackend::Emitter().emit(decoded);
    ASSERT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
    EXPECT_NE(emitted.text.find("call i64 asm sideeffect"), std::string::npos);
    EXPECT_EQ(emitted.text.find("@joyeer_intrinsic"), std::string::npos);
}

TEST_F(CInteropTest, AllowsManagedTypesInOrdinaryRuntimeUnits) {
    // The same code is fine in a runtime unit that is not a core unit.
    check("func f(text: String): Int { return text.count }\n"
          "func g() { print(value: \"x\") }\n");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(CInteropTest, LowersIndirectCallIntrinsicsAsCallsThroughAnAddress) {
    ASSERT_NO_FATAL_FAILURE(lower(R"(
@c_extern(symbol: "joyeer_intrinsic_call_void1")
func callOne(function: Int, first: Int): Void
@c_extern(symbol: "joyeer_intrinsic_call_void2")
func callTwo(function: Int, first: Int, second: Int): Void
func use(function: Int, first: Int, second: Int) {
    callOne(function: function, first: first)
    callTwo(function: function, first: first, second: second)
}
)"));
    joyeer::llvmbackend::wire::Writer writer;
    writer(*module);
    joyeer::ir::Module decoded;
    joyeer::llvmbackend::wire::Reader reader(writer.bytes);
    reader(decoded);
    ASSERT_TRUE(reader.succeeded());
    ASSERT_TRUE(joyeer::ir::Verifier().verify(decoded).succeeded());
    const auto emitted = joyeer::llvmbackend::Emitter().emit(decoded);
    ASSERT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
    // The callee is an integer converted to a pointer, called with the target's
    // C convention; no function named after the intrinsic is declared.
    EXPECT_NE(emitted.text.find("inttoptr i64"), std::string::npos);
    EXPECT_NE(emitted.text.find("call void %"), std::string::npos);
    EXPECT_EQ(emitted.text.find("@joyeer_intrinsic"), std::string::npos);
}

TEST_F(CInteropTest, LowersStaticSlotsToAMergedComdatTableWithARangeCheck) {
    ASSERT_NO_FATAL_FAILURE(lower(R"(
@c_extern(symbol: "joyeer_intrinsic_static_slot")
func slot(index: Int): Int
func counter(): Int { return slot(index: 3) }
func variable(index: Int): Int { return slot(index: index) }
)"));
    joyeer::llvmbackend::wire::Writer writer;
    writer(*module);
    joyeer::ir::Module decoded;
    joyeer::llvmbackend::wire::Reader reader(writer.bytes);
    reader(decoded);
    ASSERT_TRUE(reader.succeeded());
    ASSERT_TRUE(joyeer::ir::Verifier().verify(decoded).succeeded());
    const auto emitted = joyeer::llvmbackend::Emitter().emit(decoded);
    ASSERT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
    // One zeroed table, merged across objects, indexed after an unsigned range check.
    EXPECT_NE(emitted.text.find("@joyeer_static_slots = linkonce_odr global [64 x i64] zeroinitializer"),
              std::string::npos);
    EXPECT_NE(emitted.text.find("comdat"), std::string::npos);
    EXPECT_NE(emitted.text.find("icmp uge i64"), std::string::npos);
    EXPECT_NE(emitted.text.find("getelementptr inbounds [64 x i64], ptr @joyeer_static_slots"),
              std::string::npos);
    EXPECT_NE(emitted.text.find("static slot index out of range"), std::string::npos);
    EXPECT_EQ(emitted.text.find("@joyeer_intrinsic"), std::string::npos);
}

TEST_F(CInteropTest, AcceptsNeverAsTheResultOfCBindingsThatDoNotReturn) {
    check(R"(
@c_extern(symbol: "exit")
func exitProcess(status: Int32): Never
@c_export(symbol: "runtime_fail")
func fail(code: Int32): Never { exitProcess(status: code) }
)");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    // Never is the only non-scalar result: other result types are still rejected.
    check("@c_extern(symbol: \"f\")\nfunc f(): Int?\n");
    EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::unsupportedNativeSignature));
}

TEST_F(CInteropTest, EmitsNoReturnForNeverBindingsAndEndsTheExportInUnreachable) {
    ASSERT_NO_FATAL_FAILURE(lower(R"(
@c_extern(symbol: "exit")
func exitProcess(status: Int32): Never
@c_export(symbol: "runtime_fail")
func fail(code: Int32): Never { exitProcess(status: code) }
)"));
    joyeer::llvmbackend::wire::Writer writer;
    writer(*module);
    joyeer::ir::Module decoded;
    joyeer::llvmbackend::wire::Reader reader(writer.bytes);
    reader(decoded);
    ASSERT_TRUE(reader.succeeded());
    ASSERT_TRUE(joyeer::ir::Verifier().verify(decoded).succeeded());
    const auto emitted = joyeer::llvmbackend::Emitter().emit(decoded);
    ASSERT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
    EXPECT_NE(emitted.text.find("declare void @exit(i32"), std::string::npos);
    EXPECT_NE(emitted.text.find("noreturn"), std::string::npos);
    EXPECT_NE(emitted.text.find("define void @runtime_fail(i32"), std::string::npos);
    EXPECT_NE(emitted.text.find("unreachable"), std::string::npos);
}

TEST_F(CInteropTest, OnlyCoreUnitsMayExportTheProgramEntryPoint) {
    const auto entry = [](const std::string& symbol) {
        return "@c_export(symbol: \"" + symbol + "\")\nfunc entry(argc: Int32, argv: Int): Int32 { return 0 }\n";
    };
    for (const auto* symbol : { "main", "wmain" }) {
        checkModules({{"runtime.core", {parse(entry(symbol))}, true, true}});
        EXPECT_TRUE(checking.succeeded()) << symbol << joyeer::typing::dump(checking.diagnostics);
        // A runtime unit that is not a core unit may not.
        check(entry(symbol));
        EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::invalidNativeDeclaration)) << symbol;
    }
    // No other startup symbol is ever allowed, core or not.
    for (const auto* symbol : { "_start", "mainCRTStartup", "wmainCRTStartup", "WinMain",
                                "wWinMain", "DllMain", "joyeer_main", "joyeer_main_uses_arguments" }) {
        checkModules({{"runtime.core", {parse(entry(symbol))}, true, true}});
        EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::invalidNativeDeclaration)) << symbol;
    }
}

TEST_F(CInteropTest, VerifierTiesTheEntryPointExportToACoreUnit) {
    // lower() builds a plain runtime unit, so build the core module by hand.
    checkModules({{"runtime.core", {parse(
            "@c_export(symbol: \"main\")\nfunc entry(argc: Int32, argv: Int): Int32 { return 0 }\n")}, true, true}});
    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    const auto analysis = joyeer::analysis::Analyzer().analyze(checking.model);
    ASSERT_TRUE(analysis.succeeded()) << joyeer::analysis::dump(analysis.diagnostics);
    auto lowered = joyeer::lowering::Lowerer().lower(
            checking.model, "fixture.joyeer",
            joyeer::ir::SourceInfo { "fixture.joyeer", ".", source->content.size(), source->lineStarts });
    ASSERT_TRUE(lowered.succeeded()) << joyeer::lowering::dump(lowered.diagnostics);
    auto entryModule = lowered.module;
    entryModule->isRuntimeObject = true;
    ASSERT_TRUE(joyeer::ir::Verifier().verify(*entryModule).succeeded());
    auto& exported = *std::find_if(entryModule->functions.begin(), entryModule->functions.end(),
            [](const auto& value) { return value.nativeBinding.has_value(); });
    EXPECT_TRUE(exported.coreContext);
    exported.coreContext = false;
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*entryModule).succeeded());
    exported.coreContext = true;
    exported.runtimeContext = false;
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*entryModule).succeeded());
}

TEST_F(CInteropTest, DeclaresAndLowersDataSymbolsAsAddressesNotCalls) {
    ASSERT_NO_FATAL_FAILURE(lower(R"(
@c_data(symbol: "environ")
func environAddress(): Int
func environment(): Int { return environAddress() }
)"));
    joyeer::llvmbackend::wire::Writer writer;
    writer(*module);
    joyeer::ir::Module decoded;
    joyeer::llvmbackend::wire::Reader reader(writer.bytes);
    reader(decoded);
    ASSERT_TRUE(reader.succeeded());
    ASSERT_TRUE(joyeer::ir::Verifier().verify(decoded).succeeded());
    const auto emitted = joyeer::llvmbackend::Emitter().emit(decoded);
    ASSERT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
    EXPECT_NE(emitted.text.find("@environ = external global i8"), std::string::npos);
    EXPECT_NE(emitted.text.find("ptrtoint ptr @environ to i64"), std::string::npos);
    EXPECT_EQ(emitted.text.find("@environ("), std::string::npos);
}

TEST_F(CInteropTest, RejectsMalformedDataSymbolDeclarations) {
    for (const auto& declaration : std::vector<std::string> {
            "@c_data(symbol: \"environ\")\nfunc f(value: Int): Int\n",
            "@c_data(symbol: \"environ\")\nfunc f(): Int32\n",
            "@c_data(symbol: \"environ\")\nfunc f(): Void\n",
            "@c_data(symbol: \"environ\")\nfunc f(): Int { return 0 }\n",
            "@c_data(symbol: \"main\")\nfunc f(): Int\n",
            "@c_data(symbol: \"joyeer_intrinsic_load_u8\")\nfunc f(): Int\n",
            "@c_data(symbol: \"environ\", pointer: \"bytes\")\nfunc f(): Int\n",
            "@c_data(symbol: \"environ\")\n@c_extern(symbol: \"environ\")\nfunc f(): Int\n",
            // One symbol cannot be both a function and a data symbol.
            "@c_data(symbol: \"environ\")\nfunc a(): Int\n@c_extern(symbol: \"environ\")\nfunc b(): Int\n" }) {
        check(declaration);
        EXPECT_FALSE(checking.succeeded()) << declaration;
    }
    // Like every native binding, it is runtime-only.
    check("@c_data(symbol: \"environ\")\nfunc f(): Int\n", false);
    EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::unauthorizedNativeBinding));
    // Two declarations of one data symbol agree.
    check("@c_data(symbol: \"environ\")\nfunc a(): Int\n@c_data(symbol: \"environ\")\nfunc b(): Int\n");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(CInteropTest, LowersBulkMemoryIntrinsicsToLlvmMemoryOperations) {
    ASSERT_NO_FATAL_FAILURE(lower(R"(
@c_extern(symbol: "joyeer_intrinsic_memcpy")
func copyBytes(destination: Int, source: Int, count: Int): Void
@c_extern(symbol: "joyeer_intrinsic_memmove")
func moveBytes(destination: Int, source: Int, count: Int): Void
@c_extern(symbol: "joyeer_intrinsic_memset")
func fillBytes(destination: Int, value: UInt8, count: Int): Void
func use(a: Int, b: Int, n: Int) {
    copyBytes(destination: a, source: b, count: n)
    moveBytes(destination: a, source: b, count: n)
    fillBytes(destination: a, value: 7, count: n)
}
)"));
    joyeer::llvmbackend::wire::Writer writer;
    writer(*module);
    joyeer::ir::Module decoded;
    joyeer::llvmbackend::wire::Reader reader(writer.bytes);
    reader(decoded);
    ASSERT_TRUE(reader.succeeded());
    ASSERT_TRUE(joyeer::ir::Verifier().verify(decoded).succeeded());
    const auto emitted = joyeer::llvmbackend::Emitter().emit(decoded);
    ASSERT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
    EXPECT_NE(emitted.text.find("@llvm.memcpy.p0.p0.i64"), std::string::npos);
    EXPECT_NE(emitted.text.find("@llvm.memmove.p0.p0.i64"), std::string::npos);
    EXPECT_NE(emitted.text.find("@llvm.memset.p0.i64"), std::string::npos);
    EXPECT_EQ(emitted.text.find("@joyeer_intrinsic"), std::string::npos);
}

TEST_F(CInteropTest, RejectsUnknownMismatchedOrMisplacedIntrinsics) {
    for (const auto& declaration : std::vector<std::string> {
            "@c_extern(symbol: \"joyeer_intrinsic_memcpy\")\nfunc f(destination: Int, source: Int): Void\n",
            "@c_extern(symbol: \"joyeer_intrinsic_memcpy\")\nfunc f(destination: Int, source: Int, count: Int32): Void\n",
            "@c_extern(symbol: \"joyeer_intrinsic_memmove\")\nfunc f(destination: Int, source: Int, count: Int): Int\n",
            "@c_extern(symbol: \"joyeer_intrinsic_memset\")\nfunc f(destination: Int, value: Int, count: Int): Void\n",
            "@c_extern(symbol: \"joyeer_intrinsic_memset\")\nfunc f(destination: Int, value: UInt8, count: Int, extra: Int): Void\n",
            "@c_export(symbol: \"joyeer_intrinsic_memcpy\")\nfunc f(destination: Int, source: Int, count: Int): Void { }\n",
            "@c_extern(symbol: \"joyeer_intrinsic_static_slot\")\nfunc f(index: Int32): Int\n",
            "@c_extern(symbol: \"joyeer_intrinsic_static_slot\")\nfunc f(index: Int): Int32\n",
            "@c_extern(symbol: \"joyeer_intrinsic_static_slot\")\nfunc f(): Int\n",
            "@c_extern(symbol: \"joyeer_intrinsic_static_slot\")\nfunc f(index: Int, extra: Int): Int\n",
            "@c_export(symbol: \"joyeer_intrinsic_static_slot\")\nfunc f(index: Int): Int { return index }\n",
            "@c_extern(symbol: \"joyeer_intrinsic_call_void1\")\nfunc f(function: Int): Void\n",
            "@c_extern(symbol: \"joyeer_intrinsic_call_void1\")\nfunc f(function: Int, first: Int): Int\n",
            "@c_extern(symbol: \"joyeer_intrinsic_call_void1\")\nfunc f(function: Int, first: UInt64): Void\n",
            "@c_extern(symbol: \"joyeer_intrinsic_call_void1\")\nfunc f(function: Int, first: Int, second: Int): Void\n",
            "@c_extern(symbol: \"joyeer_intrinsic_call_void2\")\nfunc f(function: Int, first: Int): Void\n",
            "@c_extern(symbol: \"joyeer_intrinsic_call_void2\")\nfunc f(function: Int, first: Int, second: Int32): Void\n",
            "@c_extern(symbol: \"joyeer_intrinsic_call_void2\")\nfunc f(function: Int, first: Int, second: Int, third: Int): Void\n",
            "@c_export(symbol: \"joyeer_intrinsic_call_void1\")\nfunc f(function: Int, first: Int): Void { }\n",
            "@c_extern(symbol: \"joyeer_intrinsic_unknown\")\nfunc f(address: Int): Int\n",
            "@c_extern(symbol: \"joyeer_intrinsic_load_u8\")\nfunc f(address: Int): Int\n",
            "@c_extern(symbol: \"joyeer_intrinsic_load_u8\")\nfunc f(address: Int, extra: Int): UInt8\n",
            "@c_extern(symbol: \"joyeer_intrinsic_load_u8\")\nfunc f(address: UInt64): UInt8\n",
            "@c_extern(symbol: \"joyeer_intrinsic_store_u16\")\nfunc f(address: Int, value: UInt32): Void\n",
            "@c_extern(symbol: \"joyeer_intrinsic_store_u16\")\nfunc f(address: Int, value: UInt16): Int\n",
            "@c_extern(symbol: \"joyeer_intrinsic_atomic_add\")\nfunc f(address: Int, delta: Int64): Int\n",
            "@c_extern(symbol: \"joyeer_intrinsic_atomic_load\")\nfunc f(address: Int, extra: Int): Int\n",
            "@c_extern(symbol: \"joyeer_intrinsic_atomic_load\")\nfunc f(address: Int): UInt64\n",
            "@c_extern(symbol: \"joyeer_intrinsic_atomic_exchange\")\nfunc f(address: Int, value: Int64): Int\n",
            "@c_extern(symbol: \"joyeer_intrinsic_atomic_cmpxchg\")\nfunc f(address: Int, expected: Int): Int\n",
            "@c_extern(symbol: \"joyeer_intrinsic_atomic_cmpxchg\")\nfunc f(address: Int, expected: Int, desired: UInt64): Int\n",
            "@c_extern(symbol: \"joyeer_intrinsic_address_of\")\nfunc f(bytes: [UInt8]): Int\n",
            "@c_extern(symbol: \"joyeer_intrinsic_address_of\", pointer: \"bytes\")\nfunc f(bytes: [UInt8]): Int\n",
            "@c_extern(symbol: \"joyeer_intrinsic_address_of\", pointer: \"bytes\")\nfunc f(bytes: [UInt8]): Int32\n",
            "@c_extern(symbol: \"joyeer_intrinsic_load_u8\", pointer: \"bytes\")\nfunc f(bytes: [UInt8]): UInt8\n",
            "@c_export(symbol: \"joyeer_intrinsic_load_u8\")\nfunc f(address: Int): UInt8 { return 0 }\n" }) {
        check(declaration);
        EXPECT_FALSE(checking.succeeded()) << declaration;
    }
    // Intrinsics are runtime-only, like every native binding.
    check("@c_extern(symbol: \"joyeer_intrinsic_load_u8\")\nfunc f(address: Int): UInt8\n", false);
    EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::unauthorizedNativeBinding));
    // A name that merely resembles the prefix is an ordinary symbol.
    check("@c_extern(symbol: \"joyeer_intrinsics_other\")\nfunc f(address: Int): UInt8\n");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(CInteropTest, VerifierRejectsTamperedIntrinsicBindings) {
    ASSERT_NO_FATAL_FAILURE(lower(R"(
@c_extern(symbol: "joyeer_intrinsic_load_u8")
func load8(address: Int): UInt8
@c_extern(symbol: "native_ordinary")
func ordinary(address: Int): UInt8
)"));
    ASSERT_TRUE(joyeer::ir::Verifier().verify(*module).succeeded());
    auto& ordinary = function("ordinary");
    ordinary.nativeBinding->symbol = "joyeer_intrinsic_load_u16";
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*module).succeeded());
    ordinary.nativeBinding->symbol = "joyeer_intrinsic_nonexistent";
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*module).succeeded());
    ordinary.nativeBinding->symbol = "native_ordinary";
    EXPECT_TRUE(joyeer::ir::Verifier().verify(*module).succeeded());
    function("load8").nativeBinding->kind = joyeer::NativeBindingKind::cExport;
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*module).succeeded());
}

// `address_of` is inout-only, because a borrowed array can be a constant in
// read-only memory. The shape stays valid as a C binding, so only the
// intrinsic rule can reject these.
TEST_F(CInteropTest, VerifierRejectsTamperedIntrinsicShapes) {
    ASSERT_NO_FATAL_FAILURE(lower(R"(
@c_extern(symbol: "joyeer_intrinsic_address_of", pointer: "bytes")
func addressOf(bytes: inout [UInt8]): Int
@c_extern(symbol: "joyeer_intrinsic_store_u16")
func store16(address: Int, value: UInt16): Void
)"));
    ASSERT_TRUE(joyeer::ir::Verifier().verify(*module).succeeded());
    auto& addressOf = function("addressOf");
    addressOf.parameters[0].isMutable = false;
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*module).succeeded());
    addressOf.parameters[0].isMutable = true;
    ASSERT_TRUE(joyeer::ir::Verifier().verify(*module).succeeded());
    auto& store16 = function("store16");
    auto removed = store16.parameters.back();
    store16.parameters.pop_back();
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*module).succeeded());
    store16.parameters.push_back(removed);
    store16.nativeBinding->pointerParameters = {0};
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*module).succeeded());
}

TEST_F(CInteropTest, AcceptsEmptyParameterListsAndExplicitUnitSpellings) {
    check(R"(
@c_extern(symbol: "native_flush")
func nativeFlush(): ()
@c_export(symbol: "runtime_flush")
func runtimeFlush(): Void { nativeFlush() }
)");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(CInteropTest, EmitsEveryScalarWithTargetCArgumentAndResultConventions) {
    struct Scalar { const char* name; const char* llvm; const char* extension; };
    for (const auto& scalar : std::vector<Scalar> {
            {"Bool", "i1", "zeroext"}, {"Int8", "i8", "signext"}, {"UInt8", "i8", "zeroext"},
            {"Int16", "i16", "signext"}, {"UInt16", "i16", "zeroext"},
            {"Int32", "i32", ""}, {"UInt32", "i32", ""}, {"Int64", "i64", ""},
            {"UInt64", "i64", ""}, {"Int", "i64", ""}, {"UInt", "i64", ""},
            {"Float", "float", ""}, {"Double", "double", ""},
        }) {
        SCOPED_TRACE(scalar.name);
        const std::string type = scalar.name;
        ASSERT_NO_FATAL_FAILURE(lower(
                "@c_extern(symbol: \"scalar_in\")\nfunc input(value: " + type + "): " + type + "\n"
                "@c_export(symbol: \"scalar_out\")\nfunc output(value: " + type + "): " + type +
                " { return input(value: value) }\n"));
        joyeer::llvmbackend::wire::Writer writer;
        writer(*module);
        joyeer::ir::Module decoded;
        joyeer::llvmbackend::wire::Reader reader(writer.bytes);
        reader(decoded);
        ASSERT_TRUE(reader.succeeded());
        ASSERT_TRUE(joyeer::ir::Verifier().verify(decoded).succeeded());
        joyeer::llvmbackend::EmitOptions options;
        options.emitLineTables = true;
        options.emitVariables = true;
        const auto emitted = joyeer::llvmbackend::Emitter().emit(decoded, options);
        ASSERT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
        std::string extension = scalar.extension;
#if (defined(_M_ARM64) || defined(__aarch64__)) && !defined(__APPLE__)
        extension.clear();
#elif defined(_WIN32)
        if (type != "Bool") extension.clear();
#endif
        const std::string result = (extension.empty() ? "" : extension + " ") + scalar.llvm;
        const std::string parameter = std::string(scalar.llvm) + (extension.empty() ? "" : " " + extension);
        EXPECT_NE(emitted.text.find("declare " + result + " @scalar_in(" + parameter + ")"), std::string::npos);
        EXPECT_NE(emitted.text.find("define " + result + " @scalar_out(" + parameter), std::string::npos);
        EXPECT_NE(emitted.text.find("call " + result + " @scalar_in(" + parameter), std::string::npos);
        EXPECT_EQ(emitted.text.find("@joyeer_main"), std::string::npos);
        EXPECT_EQ(emitted.text.find("call void @joyeer_panic"), std::string::npos);
    }
}

TEST_F(CInteropTest, AcceptsBoundaryLiteralsAndPreservesTheirWireRepresentation) {
    for (const auto& [type, literal] : std::vector<std::pair<std::string, std::string>> {
            {"Int8", "-128"}, {"Int8", "127"}, {"UInt8", "255"},
            {"Int16", "-32768"}, {"Int16", "32767"}, {"UInt16", "65535"},
            {"Int32", "-2147483648"}, {"Int32", "2147483647"}, {"UInt32", "4294967295"},
            {"Int64", "-9223372036854775808"}, {"Int64", "9223372036854775807"},
            {"Int", "-9223372036854775808"}, {"UInt64", "18446744073709551615"},
            {"UInt", "18446744073709551615"}, {"Float", "3.4028234663852886e38"},
            {"Float", "1.401298464324817e-45"}, {"Float", "-0.0"},
            {"Double", "1.7976931348623157e308"}, {"Double", "4.9406564584124654e-324"},
            {"Double", "1"}, {"Bool", "true"},
        }) {
        SCOPED_TRACE(type + ": " + literal);
        ASSERT_NO_FATAL_FAILURE(lower("@c_export(symbol: \"constant\")\nfunc value(): " +
                type + " { return " + literal + " }\n"));
        joyeer::llvmbackend::wire::Writer writer;
        writer(*module);
        joyeer::ir::Module decoded;
        joyeer::llvmbackend::wire::Reader reader(writer.bytes);
        reader(decoded);
        ASSERT_TRUE(reader.succeeded());
        EXPECT_EQ(joyeer::ir::dump(decoded), joyeer::ir::dump(*module));
        const auto emitted = joyeer::llvmbackend::Emitter().emit(decoded);
        EXPECT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
    }
}

TEST_F(CInteropTest, RejectsUnrepresentableScalarLiterals) {
    for (const auto& [type, literal] : std::vector<std::pair<std::string, std::string>> {
            {"Int8", "128"}, {"Int8", "-129"}, {"UInt8", "256"}, {"UInt8", "-1"},
            {"Int16", "32768"}, {"Int16", "-32769"}, {"UInt16", "65536"},
            {"Int32", "2147483648"}, {"Int32", "-2147483649"}, {"UInt32", "4294967296"},
            {"Int64", "9223372036854775808"}, {"Int", "-9223372036854775809"},
            {"UInt64", "-1"}, {"UInt", "-1"}, {"Float", "3.5e38"},
            {"Float", "1e-100"}, {"Double", "1e309"}, {"Double", "1e-400"},
            {"Int", "1.5"},
        }) {
        SCOPED_TRACE(type + ": " + literal);
        check("func value(): " + type + " { return " + literal + " }\n");
        EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::invalidNumericLiteral));
    }
}

TEST_F(CInteropTest, KeepsScalarSourceTypesDistinctWithoutImplicitConversions) {
    for (const auto& [sourceType, destination] : std::vector<std::pair<std::string, std::string>> {
            {"Int", "Int64"}, {"UInt", "UInt64"}, {"Int8", "Int16"}, {"Int32", "UInt32"},
            {"UInt8", "Bool"}, {"Float", "Double"}, {"Int", "Double"},
        }) {
        SCOPED_TRACE(sourceType + " to " + destination);
        check("@c_extern(symbol: \"native\")\nfunc native(value: " + destination + "): Void\n"
                "func call(value: " + sourceType + "): Void { native(value: value) }\n");
        EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::typeMismatch));
    }
}

TEST_F(CInteropTest, ContextuallyTypesScalarLiteralsInBranchesAndBlockArms) {
    for (const auto& [type, literal] : std::vector<std::pair<std::string, std::string>> {
            {"Int8", "-128"}, {"UInt64", "18446744073709551615"},
            {"Float", "1.25"}, {"Double", "1.25"},
        }) {
        SCOPED_TRACE(type);
        ASSERT_NO_FATAL_FAILURE(lower("func choose(flag: Bool): " + type + " {\n"
                "let value: " + type + " = if flag { " + literal + " } else { " + literal + " }\n"
                "return match flag { true => { value }, false => { " + literal + " } }\n}\n"));
        const auto emitted = joyeer::llvmbackend::Emitter().emit(*module);
        EXPECT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
    }
}

TEST_F(CInteropTest, ComparesNativeRepresentationsRatherThanSourceTypeIdentities) {
    lower(R"(
@c_extern(symbol: "native")
func first(value: Int): Int
@c_extern(symbol: "native")
func second(value: Int64): Int64
func use(value: Int64): Int64 { return second(value: value) }
)");
    ASSERT_NE(module, nullptr);
    const auto emitted = joyeer::llvmbackend::Emitter().emit(*module);
    EXPECT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
    for (const auto* type : {"UInt64", "Bool", "Float", "Double", "Int32"}) {
        check("@c_extern(symbol: \"native\")\nfunc first(value: Int): Int\n"
                "@c_extern(symbol: \"native\")\nfunc second(value: " + std::string(type) + "): Int\n");
        EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::conflictingNativeSymbol));
    }
}

TEST_F(CInteropTest, VerifierRejectsMalformedFloatingConstantsAndPatterns) {
    ASSERT_NO_FATAL_FAILURE(lower("func value(): Float { return 1.5 }\n"));
    bool foundConstant = false;
    for (auto& block : function("value").blocks) {
        for (auto& instruction : block.instructions) {
            if (instruction.opcode == joyeer::ir::Opcode::floatingConstant) {
                instruction.text = "1e999";
                foundConstant = true;
            }
        }
    }
    ASSERT_TRUE(foundConstant);
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*module).succeeded());
    EXPECT_FALSE(joyeer::llvmbackend::Emitter().emit(*module).succeeded());

    ASSERT_NO_FATAL_FAILURE(lower(R"(
func value(input: Float): Bool {
    return match input { 1.5 => true, _ => false }
}
)"));
    bool foundPattern = false;
    for (auto& block : function("value").blocks) {
        for (auto& instruction : block.instructions) {
            for (auto& arm : instruction.switchCases) {
                if (arm.pattern.kind == joyeer::ir::PatternKind::floatingLiteral) {
                    arm.pattern.text = "invalid";
                    foundPattern = true;
                }
            }
        }
    }
    ASSERT_TRUE(foundPattern);
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*module).succeeded());
    EXPECT_FALSE(joyeer::llvmbackend::Emitter().emit(*module).succeeded());
}

TEST_F(CInteropTest, SharesScalarRuntimeDeclarationsAndCallSiteAttributes) {
    for (const auto& [type, symbol] : std::vector<std::pair<std::string, std::string>> {
            {"Bool", "joyeer_print_bool"}, {"UInt8", "joyeer_print_byte"},
            {"UInt", "joyeer_print_uint"}, {"Float", "joyeer_print_float"},
            {"Double", "joyeer_print_double"},
        }) {
        SCOPED_TRACE(type);
        for (const bool importing : {true, false}) {
            ASSERT_NO_FATAL_FAILURE(lower(
                    std::string(importing ? "@c_extern" : "@c_export") +
                    "(symbol: \"" + symbol + "\")\nfunc native(value: " + type + "): Void" +
                    (importing ? "" : " {}") + "\nfunc probe(value: " + type +
                    "): Void { native(value: value)\n print(value: value) }\n"));
            const auto emitted = joyeer::llvmbackend::Emitter().emit(*module);
            EXPECT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
            EXPECT_EQ(emitted.text.find("@" + symbol + ".1"), std::string::npos);
        }
    }
}

TEST_F(CInteropTest, AcceptsCompatibleImportsAndImportOfAnExport) {
    check(R"(
@c_extern(symbol: "shared")
func first(value: Int): Int
@c_extern(symbol: "shared")
func second(input: Int): Int
@c_export(symbol: "shared")
private func implementation(other: Int): Int { return other }
)");
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(CInteropTest, RejectsConflictingNativeSymbolsAndDuplicateExports) {
    for (const auto& next : std::vector<std::string> {
            "@c_extern(symbol: \"shared\")\nfunc next(): Int\n",
            "@c_extern(symbol: \"shared\")\nfunc next(value: Int): Void\n",
            "@c_export(symbol: \"shared\")\nfunc next(value: Int): Int { return value }\n"
            "@c_export(symbol: \"shared\")\nfunc last(value: Int): Int { return value }\n",
        }) {
        check("@c_extern(symbol: \"shared\")\nfunc first(value: Int): Int\n" + next);
        EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::conflictingNativeSymbol));
    }
}

TEST_F(CInteropTest, RejectsExecutableEntryBindingsAndReservedExportSymbols) {
    check("@c_export(symbol: \"entry\")\nfunc main(): Void {}\n");
    EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::invalidNativeDeclaration));
    for (const auto* symbol : {"main", "wmain", "WinMain", "_start", "mainCRTStartup",
                              "wmainCRTStartup", "WinMainCRTStartup", "wWinMainCRTStartup",
                              "DllMain", "DllMainCRTStartup", "_DllMainCRTStartup",
                              "joyeer_main", "joyeer_main_uses_arguments"}) {
        check("@c_export(symbol: \"" + std::string(symbol) + "\")\nfunc entry(): Void {}\n");
        EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::invalidNativeDeclaration));
    }
}

TEST_F(CInteropTest, AllowsCheckedWrappersButNotDirectNativeCallsFromOrdinaryUnits) {
    const auto runtime = parse(R"(
@c_extern(symbol: "native_value")
public func nativeValue(): Int
public func value(): Int { return nativeValue() }
)", 1);
    checkModules({
        {"app", {parse("import runtime\nfunc main() { print(value: runtime.value()) }\n")}, false},
        {"runtime", {runtime}, true},
    });
    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    checkModules({
        {"app", {parse("import runtime\nfunc main() { print(value: runtime.nativeValue()) }\n")}, false},
        {"runtime", {runtime}, true},
    });
    EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::unauthorizedNativeBinding));
}

TEST_F(CInteropTest, DoesNotGrantRuntimeContextToDependencies) {
    const auto dependency = parse(
            "@c_extern(symbol: \"native\")\npublic func native(): Int\n", 1);
    checkModules({
        {"runtime", {parse("import dependency\nfunc wrapper(): Int { return dependency.native() }\n")}, true},
        {"dependency", {dependency}, false},
    });
    EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::unauthorizedNativeBinding));
}

TEST_F(CInteropTest, LowersNativeMetadataWithoutInventingImportBodiesOrDebugScopes) {
    lower(identitySource);
    ASSERT_NE(module, nullptr);
    const auto& imported = function("nativeIdentity");
    ASSERT_TRUE(imported.nativeBinding.has_value());
    EXPECT_EQ(imported.nativeBinding->symbol, "native_identity");
    EXPECT_TRUE(imported.isExternal);
    EXPECT_TRUE(imported.blocks.empty());
    EXPECT_EQ(imported.entry, joyeer::ir::invalidBlockId);
    EXPECT_FALSE(imported.debugScope.has_value());
    EXPECT_FALSE(imported.debugLocation.has_value());
    const auto& exported = function("runtimeIdentity");
    EXPECT_FALSE(exported.isExternal);
    EXPECT_TRUE(exported.debugScope.has_value());
    EXPECT_EQ(exported.parameters[0].label, "input");
    EXPECT_TRUE(joyeer::ir::Verifier().verify(*module).succeeded());
}

TEST_F(CInteropTest, PreservesBindingMetadataAcrossTheBackendWireFormat) {
    lower(identitySource);
    ASSERT_NE(module, nullptr);
    joyeer::llvmbackend::wire::Writer writer;
    writer(*module);
    joyeer::ir::Module decoded;
    joyeer::llvmbackend::wire::Reader reader(writer.bytes);
    reader(decoded);
    ASSERT_TRUE(reader.succeeded());
    EXPECT_TRUE(decoded.isRuntimeObject);
    EXPECT_EQ(joyeer::ir::dump(decoded), joyeer::ir::dump(*module));
    EXPECT_TRUE(joyeer::ir::Verifier().verify(decoded).succeeded());
}

TEST_F(CInteropTest, VerifierRejectsInvalidOrUnauthorizedNativeMetadata) {
    lower(identitySource);
    ASSERT_NE(module, nullptr);
    function("nativeIdentity").runtimeContext = false;
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*module).succeeded());
    function("nativeIdentity").runtimeContext = true;
    function("nativeIdentity").nativeBinding->symbol = "invalid-symbol";
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*module).succeeded());
}

TEST_F(CInteropTest, VerifierRejectsOrdinaryCallersOfNativeBindings) {
    lower(R"(
@c_extern(symbol: "native")
func native(): Int
func wrapper(): Int { return native() }
)");
    ASSERT_NE(module, nullptr);
    module->isRuntimeObject = false;
    function("wrapper").runtimeContext = false;
    const auto verification = joyeer::ir::Verifier().verify(*module);
    EXPECT_FALSE(verification.succeeded());
    EXPECT_NE(joyeer::ir::dump(verification).find("direct call to a C binding"), std::string::npos);
}

TEST_F(CInteropTest, EmitsLeafCAdaptersWithoutStartupOrManagedRuntimeDependencies) {
    lower(identitySource);
    ASSERT_NE(module, nullptr);
    joyeer::llvmbackend::EmitOptions options;
    options.emitLineTables = true;
    options.emitVariables = true;
    const auto emitted = joyeer::llvmbackend::Emitter().emit(*module, options);
    ASSERT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
    EXPECT_FALSE(emitted.hasEntryPoint);
    EXPECT_NE(emitted.text.find("declare i64 @native_identity(i64)"), std::string::npos);
    EXPECT_NE(emitted.text.find("define i64 @runtime_identity(i64"), std::string::npos);
    EXPECT_NE(emitted.text.find("define internal i64 @joyeer_fn_"), std::string::npos);
    EXPECT_EQ(emitted.text.find("@joyeer_main"), std::string::npos);
    EXPECT_EQ(emitted.text.find("@joyeer_string_clone_abi"), std::string::npos);
    EXPECT_EQ(emitted.text.find("@joyeer_clone_type_"), std::string::npos);
    EXPECT_NE(emitted.text.find("!DILocation("), std::string::npos);
}

TEST_F(CInteropTest, UsesNativeMetadataInsteadOfSpecialCasingSourceFunctionNames) {
    lower(R"(
@c_extern(symbol: "custom_identity")
func print(value: Int): Int
@c_export(symbol: "export_identity")
func identity(value: Int): Int { return print(value: value) }
)");
    ASSERT_NE(module, nullptr);
    const auto emitted = joyeer::llvmbackend::Emitter().emit(*module);
    ASSERT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
    EXPECT_NE(emitted.text.find("call i64 @custom_identity("), std::string::npos);
    EXPECT_EQ(emitted.text.find("@joyeer_print_int"), std::string::npos);
}

TEST_F(CInteropTest, DefinesAnExportThatAlsoHasCompatibleImports) {
    lower(R"(
@c_extern(symbol: "shared")
func imported(value: Int): Int
@c_extern(symbol: "shared")
func other(input: Int): Int
@c_export(symbol: "shared")
func implementation(argument: Int): Int { return argument }
)");
    ASSERT_NE(module, nullptr);
    const auto emitted = joyeer::llvmbackend::Emitter().emit(*module);
    ASSERT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
    EXPECT_NE(emitted.text.find("define i64 @shared(i64"), std::string::npos);
    EXPECT_EQ(emitted.text.find("@shared.1"), std::string::npos);
}

TEST_F(CInteropTest, RejectsNativeSignaturesConflictingWithGeneratedRuntimeCalls) {
    struct Conflict {
        const char* symbol;
        const char* signature;
        const char* body;
    };
    for (const auto& conflict : std::vector<Conflict> {
            {"joyeer_print_int", "(): Int", "print(value: 42)"},
            {"joyeer_print_int", "(value: Int): Int", "print(value: 42)"},
            {"joyeer_print_bool", "(value: Int): Void", "print(value: true)"},
            {"joyeer_panic", "(): Int", "let sum = value + 1\nprint(value: sum)"},
            {"joyeer_string_clone_abi", "(): Int", "let text = \"value\"\nprint(value: text)"},
        }) {
        for (const bool importing : {false, true}) {
            for (const bool runtimeObject : {false, true}) {
                SCOPED_TRACE(conflict.symbol);
                SCOPED_TRACE(conflict.signature);
                SCOPED_TRACE(importing);
                SCOPED_TRACE(runtimeObject);
                const auto attribute = importing ? "@c_extern" : "@c_export";
                const auto implementation = importing ? "" :
                        std::string(conflict.signature).ends_with("Int") ? " { return 0 }" : " {}";
                const auto text = std::string(attribute) + "(symbol: \"" + conflict.symbol + "\")\n" +
                        "func native" + conflict.signature + implementation + "\n" +
                        "func probe(value: Int): Void {\n" + conflict.body + "\n}\n";
                ASSERT_NO_FATAL_FAILURE(check(text));
                if (!checking.succeeded()) {
                    ASSERT_EQ(checking.diagnostics.size(), 1u);
                    EXPECT_EQ(checking.diagnostics.front().id,
                              joyeer::typing::TypeCheckingDiagnosticId::conflictingNativeSymbol);
                    EXPECT_NE(checking.diagnostics.front().message.find(conflict.symbol), std::string::npos);
                    continue;
                }
                ASSERT_NO_FATAL_FAILURE(lower(text));
                module->isRuntimeObject = runtimeObject;
                const auto span = function("native").nativeBinding->span;
                const auto emitted = joyeer::llvmbackend::Emitter().emit(*module);
                ASSERT_FALSE(emitted.succeeded());
                ASSERT_EQ(emitted.diagnostics.size(), 1u);
                EXPECT_STREQ(joyeer::llvmbackend::diagnosticName(emitted.diagnostics[0].id),
                             "llvm.conflicting-native-symbol");
                EXPECT_EQ(emitted.diagnostics[0].span.sourceId, span.sourceId);
                EXPECT_EQ(emitted.diagnostics[0].span.offset, span.offset);
                EXPECT_EQ(emitted.diagnostics[0].span.length, span.length);
                EXPECT_NE(emitted.diagnostics[0].message.find(conflict.symbol), std::string::npos);
            }
        }
    }
}

TEST_F(CInteropTest, AcceptsCompatibleNativeAndGeneratedRuntimeDeclarations) {
    for (const bool importing : {false, true}) {
        SCOPED_TRACE(importing);
        ASSERT_NO_FATAL_FAILURE(lower(
                std::string(importing ? "@c_extern" : "@c_export") +
                "(symbol: \"joyeer_print_int\")\n"
                "func native(value: Int): Void" + (importing ? "" : " {}") + "\n"
                "func probe(value: Int): Void { native(value: value)\nprint(value: value) }\n"));
        const auto emitted = joyeer::llvmbackend::Emitter().emit(*module);
        ASSERT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
        EXPECT_NE(emitted.text.find("call void @joyeer_print_int(i64"), std::string::npos);
        EXPECT_EQ(emitted.text.find("@joyeer_print_int.1"), std::string::npos);
    }
}

TEST_F(CInteropTest, KeepsTheCNameWhenItResemblesAPrivateGeneratedName) {
    lower(identitySource);
    ASSERT_NE(module, nullptr);
    const auto symbol = "joyeer_fn_" + std::to_string(function("runtimeIdentity").id);
    function("runtimeIdentity").nativeBinding->symbol = symbol;
    const auto emitted = joyeer::llvmbackend::Emitter().emit(*module);
    ASSERT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
    EXPECT_NE(emitted.text.find("define i64 @" + symbol + "(i64"), std::string::npos);
    EXPECT_NE(emitted.text.find("define internal i64 @" + symbol + ".1"), std::string::npos);
}

TEST_F(CInteropTest, VerifierRejectsUnsupportedNativeSignatures) {
    lower(identitySource);
    ASSERT_NE(module, nullptr);
    function("nativeIdentity").parameters[0].isConsuming = true;
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*module).succeeded());
    function("nativeIdentity").parameters[0].isConsuming = false;
    function("nativeIdentity").returnsValue = false;
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*module).succeeded());
}

TEST_F(CInteropTest, RejectsNativeImportsThatWouldRenameExecutableStartupAdapters) {
    lower(R"(
@c_extern(symbol: "joyeer_main")
func nativeEntry(): Int
func main(): Void { print(value: nativeEntry()) }
)");
    ASSERT_NE(module, nullptr);
    module->isRuntimeObject = false;
    const auto emitted = joyeer::llvmbackend::Emitter().emit(*module);
    ASSERT_FALSE(emitted.succeeded());
    EXPECT_EQ(emitted.diagnostics.front().id, joyeer::llvmbackend::DiagnosticId::invalidEntryPoint);
}

TEST(CInteropParserTest, RejectsMisplacedAnnotationsDefaultsAndOrdinaryBodylessDeclarations) {
    for (const auto& text : std::vector<std::string> {
            "func native(): Int\n",
            "@c_extern(symbol: \"native\")\nlet value = 1\n",
            "@c_extern(symbol: \"native\")\nstruct Value {}\n",
            "public @c_extern(symbol: \"native\") func native(): Int\n",
            "@c_extern\nfunc native(): Int\n",
            "@c_extern(symbol: \"native\")\nfunc native(value: Int = 1): Int\n",
            "@property forall n in 0..<1: true\nfunc native(): Int {}\n",
        }) {
        SCOPED_TRACE(text);
        auto source = std::make_shared<SourceFile>(text);
        Diagnostics diagnostics;
        LexParser(&diagnostics).parse(source);
        const auto result = joyeer::parser::Parser(source->tokens).parse();
        EXPECT_TRUE(diagnostics.hasFailure() || !result.succeeded());
    }
}

// The wire format carries a function type's effects and both function-value instructions (decision 0020).
TEST_F(CInteropTest, WireFormatRoundTripsFunctionValues) {
    ASSERT_NO_FATAL_FAILURE(lower(R"(
func fill(out: initializing String, text: consuming String) { &out = text }
func run(): String {
    let f: (initializing String, consuming String): () = fill
    var out: String
    f(&out, consume "x")
    return out
}
)"));
    joyeer::llvmbackend::wire::Writer writer;
    writer(*module);
    joyeer::ir::Module decoded;
    joyeer::llvmbackend::wire::Reader reader(writer.bytes);
    reader(decoded);
    ASSERT_TRUE(reader.succeeded());
    ASSERT_TRUE(joyeer::ir::Verifier().verify(decoded).succeeded());
    const auto function = std::find_if(decoded.types.begin(), decoded.types.end(),
            [](const auto& type) { return type.kind == joyeer::typing::TypeKind::function; });
    ASSERT_NE(function, decoded.types.end());
    EXPECT_EQ(function->effects, (std::vector<joyeer::syntax::AccessEffect> {
            joyeer::syntax::AccessEffect::initializing, joyeer::syntax::AccessEffect::consuming }));
    EXPECT_EQ(joyeer::ir::dump(decoded), joyeer::ir::dump(*module));
    const auto text = joyeer::ir::dump(decoded);
    EXPECT_NE(text.find("function_address"), std::string::npos);
    EXPECT_NE(text.find("call_indirect"), std::string::npos);
}
} // namespace

namespace {
const char* const layoutProgram = R"(
struct Box<T> {
    var value: T
}
func measure<T>(value: consuming T): Int {
    let address = 4096
    joyeer_intrinsic_move_in<T>(address: address, value: consume value)
    let again = joyeer_intrinsic_move_out<T>(address: address)
    return joyeer_intrinsic_size_of<T>()
}
func main() {
    let a = measure(value: consume 3)
    let b = measure(value: consume "text")
    let c = Box(value: 1)
    let d = Box(value: "s")
}
)";
}

// The layout opcodes verify only in a runtime function, with a known concrete queried type.
TEST_F(CInteropTest, VerifierChecksLayoutOpcodes) {
    lower(layoutProgram);
    ASSERT_TRUE(joyeer::ir::Verifier().verify(*module).succeeded());

    const auto find = [&](joyeer::ir::Opcode opcode) -> joyeer::ir::Instruction* {
        for (auto& value : module->functions) {
            for (auto& block : value.blocks) {
                for (auto& instruction : block.instructions) {
                    if (instruction.opcode == opcode) return &instruction;
                }
            }
        }
        return nullptr;
    };
    auto* size = find(joyeer::ir::Opcode::typeSize);
    auto* address = find(joyeer::ir::Opcode::integerAddress);
    ASSERT_NE(size, nullptr);
    ASSERT_NE(address, nullptr);

    const auto original = size->integerValue;
    size->integerValue = 1000000;
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*module).succeeded());
    size->integerValue = original;
    ASSERT_TRUE(joyeer::ir::Verifier().verify(*module).succeeded());

    address->result->category = joyeer::ir::ValueCategory::value;
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*module).succeeded());
    address->result->category = joyeer::ir::ValueCategory::address;
    ASSERT_TRUE(joyeer::ir::Verifier().verify(*module).succeeded());

    const auto operands = address->operands;
    address->operands.clear();
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*module).succeeded());
    address->operands = operands;
    ASSERT_TRUE(joyeer::ir::Verifier().verify(*module).succeeded());

    for (auto& value : module->functions) value.runtimeContext = false;
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*module).succeeded());
}

// One structure per concrete instance, named for it; a field symbol repeats across instances but not
// within one structure.
TEST_F(CInteropTest, GenericStructsLowerToOneStructurePerInstance) {
    lower(layoutProgram);
    std::vector<std::string> names;
    for (const auto& structure : module->structures) names.push_back(structure.name);
    EXPECT_NE(std::find(names.begin(), names.end(), "Box<Int>"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "Box<String>"), names.end());
    ASSERT_TRUE(joyeer::ir::Verifier().verify(*module).succeeded());

    for (auto& structure : module->structures) {
        if (structure.name != "Box<Int>") continue;
        ASSERT_FALSE(structure.fields.empty());
        structure.fields.push_back(structure.fields.front());
    }
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*module).succeeded());
}

TEST_F(CInteropTest, RuntimeSlotsDiscoverAddressOnlyGenericCallbacks) {
    ASSERT_NO_FATAL_FAILURE(lower(R"(
func entry<T>(address: consuming Int): Void {
    joyeer_intrinsic_destroy_at<T>(address: address)
}
func store<T>(value: consuming T): Int {
    let address = joyeer_intrinsic_stack_slot<T>()
    joyeer_intrinsic_move_in<T>(address: address, value: consume value)
    return joyeer_intrinsic_callback_address<T>(function: entry)
}
func main() {
    let callback = store(value: consume "text")
    let trivial = joyeer_intrinsic_needs_destroy<Int>()
    let managed = joyeer_intrinsic_needs_destroy<String>()
}
)"));
    ASSERT_NE(module, nullptr);
    EXPECT_TRUE(joyeer::ir::Verifier().verify(*module).succeeded());
    EXPECT_NE(joyeer::ir::dump(*module).find("pointer_to_integer"), std::string::npos);
    EXPECT_TRUE(std::any_of(checking.model->instances().begin(), checking.model->instances().end(),
            [](const auto& instance) { return instance.name.find("entry<String>") != std::string::npos; }));
    joyeer::llvmbackend::wire::Writer writer;
    writer(*module);
    joyeer::ir::Module decoded;
    joyeer::llvmbackend::wire::Reader reader(writer.bytes);
    reader(decoded);
    ASSERT_TRUE(reader.succeeded());
    EXPECT_EQ(joyeer::ir::dump(decoded), joyeer::ir::dump(*module));
}

TEST_F(CInteropTest, RuntimeCallbackAddressesRequireTheExactEntrySignature) {
    for (const auto& declaration : std::vector<std::string> {
            "func entry<T>(): Void {}",
            "func entry<T>(address: Int): Void {}",
            "func entry<T>(address: inout Int): Void {}",
            "func entry<T>(address: consuming Int, extra: Int): Void {}",
            "func entry<T>(address: consuming Int): Int { return address }",
    }) {
        SCOPED_TRACE(declaration);
        check(declaration + R"(
func main() {
    let callback = joyeer_intrinsic_callback_address<Int>(function: entry)
}
)");
        EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::invalidTypeArgument))
                << joyeer::typing::dump(checking.diagnostics);
    }
}

TEST_F(CInteropTest, RuntimeCallbackReferencesCheckConstraintsAndExpandingRecursion) {
    check(R"(
struct Token {
    var value: Int
    deinit() {}
}
func entry<T: Copyable>(address: consuming Int): Void {}
func main() {
    let callback = joyeer_intrinsic_callback_address<Token>(function: entry)
}
)");
    EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::unsatisfiedConstraint));
    check(R"(
func entry<T>(address: consuming Int): Void {
    let next = joyeer_intrinsic_callback_address<[T]>(function: entry)
}
func main() {
    let callback = joyeer_intrinsic_callback_address<Int>(function: entry)
}
)");
    EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::expandingGenericRecursion));
}

TEST_F(CInteropTest, VerifierRestrictsPointerIntegerConversionToRuntimeValues) {
    ASSERT_NO_FATAL_FAILURE(lower(R"(
func main() {
    let address = joyeer_intrinsic_stack_slot<Int>()
}
)"));
    ASSERT_NE(module, nullptr);
    auto& main = function("main");
    joyeer::ir::Instruction* conversion = nullptr;
    for (auto& block : main.blocks) {
        for (auto& instruction : block.instructions) {
            if (std::string(joyeer::ir::opcodeName(instruction.opcode)) == "pointer_to_integer") {
                conversion = &instruction;
            }
        }
    }
    ASSERT_NE(conversion, nullptr);
    ASSERT_TRUE(joyeer::ir::Verifier().verify(*module).succeeded());
    conversion->result->category = joyeer::ir::ValueCategory::address;
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*module).succeeded());
    conversion->result->category = joyeer::ir::ValueCategory::value;
    main.runtimeContext = false;
    EXPECT_FALSE(joyeer::ir::Verifier().verify(*module).succeeded());
}

TEST_F(CInteropTest, ConcurrencyPreludeContainsOrdinaryLibraryDeclarations) {
    ASSERT_NO_FATAL_FAILURE(check("func main() {}\n", false));
    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    const auto& semantic = checking.model->semanticModel();
    ASSERT_EQ(semantic->root()->items.size(), 1u);
    const auto* prelude = semantic->scope(semantic->preludeScope());
    ASSERT_NE(prelude, nullptr);
    ASSERT_TRUE(prelude->values.contains("spawn"));
    ASSERT_TRUE(prelude->values.contains("channel"));
    ASSERT_TRUE(prelude->types.contains("JoinHandle"));
    EXPECT_EQ(semantic->symbol(prelude->values.at("spawn"))->kind, joyeer::semantic::SymbolKind::function);
    EXPECT_EQ(semantic->symbol(prelude->values.at("channel"))->kind, joyeer::semantic::SymbolKind::function);
    const auto handle = prelude->types.at("JoinHandle");
    EXPECT_EQ(semantic->symbol(handle)->kind, joyeer::semantic::SymbolKind::structure);
    const auto instance = checking.model->types().typeForSymbol(handle, { checking.model->types().intType() });
    ASSERT_TRUE(instance.has_value());
    EXPECT_EQ(checking.model->types().type(*instance)->kind, joyeer::typing::TypeKind::structure);
    EXPECT_TRUE(checking.model->isLinear(*instance));
    EXPECT_FALSE(prelude->values.contains("taskEntry"));
}

TEST_F(CInteropTest, ExplicitRuntimeUnitsCannotClaimCompilerPreludeContracts) {
    check(R"(
@compiler_transfer()
func drop<T>(value: consuming T): Void {}
@compiler_role(task_start)
func launch(value: Int): Int { return value }
)");
    EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::invalidAnnotation));
}

TEST_F(CInteropTest, CompilerPreludeTransferParametersCannotBeDropped) {
    const auto root = parse("func main() {}\n", 0);
    const auto library = parse(R"(
@compiler_transfer()
func drop<T>(value: consuming T): Void {}
)", 1);
    checkModules({{"app", {root}}, {"runtime.fixture", {library}, true, false, true}});
    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    const auto analysis = joyeer::analysis::Analyzer().analyze(checking.model);
    EXPECT_TRUE(std::any_of(analysis.diagnostics.begin(), analysis.diagnostics.end(),
            [](const auto& diagnostic) {
                return diagnostic.id == joyeer::analysis::DiagnosticId::linearNotConsumed;
            }));
}

TEST_F(CInteropTest, TaskInputsKeepTheNamedTargetsAssignableContext) {
    ASSERT_NO_FATAL_FAILURE(check(R"(
func work(value: consuming Int?): Void {}
func main() {
    let value = 1
    let literal = spawn(function: work, input: consume 1)
    consume literal.join()
    let stored = spawn(function: work, input: consume value)
    consume stored.join()
}
)", false));
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    check(R"(
func work(value: consuming Int): Void {}
func main() {
    let h = spawn(function: work, input: consume "wrong")
    consume h.join()
}
)", false);
    ASSERT_EQ(checking.diagnostics.size(), 1u) << joyeer::typing::dump(checking.diagnostics);
    EXPECT_EQ(checking.diagnostics.front().id, joyeer::typing::TypeCheckingDiagnosticId::typeMismatch);
}

TEST_F(CInteropTest, CallbackReferencesCheckMarkersExplicitResultsAndTargetAuthority) {
    for (const auto& marker : { "&", "consume " }) {
        check(std::string(R"(
func entry<T>(address: consuming Int): Void {}
func main() {
    let callback = joyeer_intrinsic_callback_address<Int>(function: )") + marker + "entry)\n}\n");
        EXPECT_FALSE(checking.succeeded()) << marker;
    }
    check(R"(
func entry<T>(address: consuming Int) {}
func main() {
    let callback = joyeer_intrinsic_callback_address<Int>(function: entry)
}
)");
    EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::invalidTypeArgument));
    const auto bridge = parse(R"(
import callbacks
func main() {
    let callback = joyeer_intrinsic_callback_address<Int>(function: callbacks.entry)
}
)", 0);
    const auto target = parse("public func entry<T>(address: consuming Int): Void {}\n", 1);
    checkModules({{"bridge", {bridge}, true}, {"callbacks", {target}}});
    EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::unauthorizedIntrinsic));
}

TEST_F(CInteropTest, ChannelArgumentsHaveOneSpecificDiagnosticAtTheTypeOccurrence) {
    for (const auto& element : { "Never", "JoinHandle<Int>" }) {
        for (const auto& pattern : {
                "func bad(value: ChannelSender<ELEMENT>): Void {}\n",
                "func bad(): ChannelSender<ELEMENT> { return bad() }\n",
                "struct Bad { var value: ChannelSender<ELEMENT> }\n",
                "func bad(value: [ChannelSender<ELEMENT>]): Void {}\n",
            }) {
            std::string text(pattern);
            text.replace(text.find("ELEMENT"), 7, element);
            SCOPED_TRACE(text);
            check(text, false);
            ASSERT_EQ(checking.diagnostics.size(), 1u) << joyeer::typing::dump(checking.diagnostics);
            const auto& diagnostic = checking.diagnostics.front();
            EXPECT_EQ(diagnostic.id, std::string(element) == "Never"
                    ? joyeer::typing::TypeCheckingDiagnosticId::channelElementType
                    : joyeer::typing::TypeCheckingDiagnosticId::linearChannelElement);
            EXPECT_EQ(diagnostic.span.sourceId, 0u);
            EXPECT_EQ(diagnostic.span.offset, text.find("ChannelSender"));
        }
    }
    check("struct ChannelSender<T> { var value: T }\nfunc bad(value: ChannelSender<Never>) {}\n", false);
    EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::invalidTypeArgument));
    EXPECT_FALSE(has(joyeer::typing::TypeCheckingDiagnosticId::channelElementType));
    for (const auto& element : {"Never", "JoinHandle<Int>"}) {
        for (const bool wrapper : {false, true}) {
            std::string text = wrapper
                    ? "func bad(sender: initializing ChannelSender<ELEMENT>, receiver: initializing ChannelReceiver<ELEMENT>): Void {\n"
                    : "func main() {\nvar sender: ChannelSender<ELEMENT>\nvar receiver: ChannelReceiver<ELEMENT>\n";
            text += "channel(capacity: 1, sender: &sender, receiver: &receiver)\n}\n";
            for (auto position = text.find("ELEMENT"); position != std::string::npos;
                 position = text.find("ELEMENT")) {
                text.replace(position, 7, element);
            }
            SCOPED_TRACE(text);
            check(text, false);
            ASSERT_EQ(checking.diagnostics.size(), 2u) << joyeer::typing::dump(checking.diagnostics);
            for (const auto& diagnostic : checking.diagnostics) {
                EXPECT_EQ(diagnostic.id, std::string(element) == "Never"
                        ? joyeer::typing::TypeCheckingDiagnosticId::channelElementType
                        : joyeer::typing::TypeCheckingDiagnosticId::linearChannelElement);
            }
        }
    }
}

TEST_F(CInteropTest, CompilerTransferChecksBranchesCopiesOverwritesAndDestructibleFields) {
    checkPrelude(R"(
@compiler_transfer()
func partial<T>(value: consuming T, flag: Bool): Void {
    if flag { joyeer_intrinsic_move_in<T>(address: 4096, value: consume value) }
}
)");
    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    auto analysis = joyeer::analysis::Analyzer().analyze(checking.model);
    EXPECT_FALSE(analysis.succeeded());
    checkPrelude(R"(
@compiler_transfer()
func replace<T>(first: consuming T, second: consuming T): Void {
    var owned = consume first
    owned = consume second
    joyeer_intrinsic_move_in<T>(address: 4096, value: consume owned)
}
)");
    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    analysis = joyeer::analysis::Analyzer().analyze(checking.model);
    EXPECT_TRUE(std::any_of(analysis.diagnostics.begin(), analysis.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.id == joyeer::analysis::DiagnosticId::linearOverwrite;
    }));
    checkPrelude(R"(
@compiler_transfer()
func copy<T>(value: consuming T): Void { let duplicate = value }
)");
    EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::copyOfNoncopyable));
    checkPrelude(R"(
@compiler_transfer()
struct Bad<T> {
    var value: T
    deinit() {}
}
)");
    EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::linearInDeinitType));
    checkPrelude(R"(
@compiler_transfer()
func forward<T>(value: consuming T): T { return value }
@compiler_transfer()
func store<T>(value: consuming T): Void {
    joyeer_intrinsic_move_in<T>(address: 4096, value: consume value)
}
)");
    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    EXPECT_TRUE(joyeer::analysis::Analyzer().analyze(checking.model).succeeded());
}

TEST_F(CInteropTest, PendingCompilerTransferCannotEscapeBeforeTheCallCommits) {
    checkPrelude(R"(
@compiler_transfer()
func relay<T>(value: consuming T, later: Int): T { return value }
@compiler_transfer()
func lose<T>(value: consuming T): Void {
    let moved = relay(value: consume value, later: return)
    joyeer_intrinsic_move_in<T>(address: 4096, value: consume moved)
}
)");
    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    const auto analysis = joyeer::analysis::Analyzer().analyze(checking.model);
    EXPECT_TRUE(std::any_of(analysis.diagnostics.begin(), analysis.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.id == joyeer::analysis::DiagnosticId::linearNotConsumed &&
                diagnostic.message.find("enclosing call") != std::string::npos;
    })) << joyeer::analysis::dump(analysis.diagnostics);
    for (const auto& text : {
            R"(
@compiler_transfer()
struct Carrier<T> {
    var payload: T
    consuming func take(extra: Int): T {
        let owned = consume self.payload
        return owned
    }
}
@compiler_transfer()
func lose<T>(value: consuming T): Void {
    let carrier = Carrier(payload: consume value)
    let moved = consume carrier.take(extra: return)
    joyeer_intrinsic_move_in<T>(address: 4096, value: consume moved)
}
)",
            R"(
@compiler_transfer()
func lose<T>(value: consuming T): Void {
    let values: [T] = [consume value, return]
}
)",
        }) {
        SCOPED_TRACE(text);
        checkPrelude(text);
        ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
        const auto rejected = joyeer::analysis::Analyzer().analyze(checking.model);
        EXPECT_TRUE(std::any_of(rejected.diagnostics.begin(), rejected.diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.id == joyeer::analysis::DiagnosticId::linearNotConsumed &&
                    diagnostic.message.find("enclosing") != std::string::npos;
        })) << joyeer::analysis::dump(rejected.diagnostics);
    }
    checkPrelude(R"(
@compiler_transfer()
func relay<T>(value: consuming T, later: Int): T { return value }
@compiler_transfer()
func forward<T>(value: consuming T): T {
    return relay(value: consume value, later: if true {
        while true { break }
        0
    } else { 0 })
}
)");
    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    EXPECT_TRUE(joyeer::analysis::Analyzer().analyze(checking.model).succeeded());
}

TEST_F(CInteropTest, MalformedCompilerRolesAreDiagnosedBeforeTheyAreCalled) {
    for (const auto& text : {
            "@compiler_role(task_start)\npublic func launch(): Int { return 1 }\n",
            "@compiler_role(channel_sender)\npublic func wrong(): Void {}\n",
            "@compiler_transfer()\n@compiler_transfer()\nfunc duplicate<T>(value: consuming T): T { return value }\n",
            "@compiler_transfer(1)\nstruct Bad<T> { var value: T }\n",
        }) {
        SCOPED_TRACE(text);
        checkPrelude(text);
        EXPECT_TRUE(std::any_of(checking.diagnostics.begin(), checking.diagnostics.end(), [](const auto& diagnostic) {
            return std::string(joyeer::typing::diagnosticName(diagnostic.id)) == "type-checking.invalid-prelude";
        })) << joyeer::typing::dump(checking.diagnostics);
    }
}

TEST_F(CInteropTest, BundledPreludeDeclaresEachCompilerRoleExactlyOnce) {
    using joyeer::semantic::LibraryRole;
    ASSERT_NO_FATAL_FAILURE(check("func main() {}\n", false));
    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    std::map<LibraryRole, int> counts;
    for (const auto& symbol : checking.model->semanticModel()->symbols()) {
        if (symbol.libraryRole != LibraryRole::none) ++counts[symbol.libraryRole];
    }
    EXPECT_EQ(counts[LibraryRole::taskStart], 1);
    EXPECT_EQ(counts[LibraryRole::taskHandle], 1);
    EXPECT_EQ(counts[LibraryRole::channelCreate], 1);
    EXPECT_EQ(counts[LibraryRole::channelSender], 1);
    EXPECT_EQ(counts[LibraryRole::channelReceiver], 1);
    // JoinHandle.join and the close method of each channel end.
    EXPECT_EQ(counts[LibraryRole::storedConsumer], 3);
}

TEST_F(CInteropTest, CompilerMetadataOnStructsEnumsAndMethodsIsRejectedOutsideThePrelude) {
    for (const auto& text : std::vector<std::string> {
            "@compiler_role(task_handle)\nstruct Fake<R> { var value: Int }\n",
            "@compiler_transfer()\nstruct Fake<T> { var value: T }\n",
            "struct Fake {\n    var value: Int\n    @compiler_role(stored_consumer)\n    consuming func end(): Void {}\n}\n",
            "@compiler_role(channel_sender)\nenum Fake { Case }\n",
        }) {
        for (const bool preludeAnnotations : {false, true}) {
            for (const bool runtimeUnit : {false, true}) {
                SCOPED_TRACE(text + (preludeAnnotations ? " [prelude parser]" : " [user parser]") +
                        (runtimeUnit ? " [runtime unit]" : " [user unit]"));
                source = std::make_shared<SourceFile>(text);
                Diagnostics diagnostics;
                LexParser(&diagnostics).parse(source);
                ASSERT_FALSE(diagnostics.hasFailure());
                auto parsed = joyeer::parser::Parser(source->tokens, true, preludeAnnotations).parse();
                if (!parsed.succeeded()) continue;
                const auto resolution = joyeer::semantic::NameResolver().resolve(
                        {{"fixture", {parsed.root}, runtimeUnit}});
                if (!resolution.succeeded()) continue;
                const auto result = joyeer::typing::TypeChecker().check(resolution.model);
                EXPECT_FALSE(result.succeeded());
            }
        }
    }
}

TEST_F(CInteropTest, PreludeOnlyIntrinsicsAreUnauthorizedInUserUnits) {
    for (const auto& text : std::vector<std::string> {
            "func main() { let slot = joyeer_intrinsic_stack_slot<Int>() }\n",
            "func main() { let needed = joyeer_intrinsic_needs_destroy<Int>() }\n",
            "func entry(address: consuming Int): Void {}\n"
            "func main() { let callback = joyeer_intrinsic_callback_address<Int>(function: entry) }\n",
        }) {
        SCOPED_TRACE(text);
        check(text, false);
        EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::unauthorizedIntrinsic))
                << joyeer::typing::dump(checking.diagnostics);
    }
}

TEST_F(CInteropTest, PendingCompilerTransferCannotEscapeThroughBreakAndContinue) {
    const std::string prefix = R"(
@compiler_transfer()
func relay<T>(value: consuming T, later: Int): T { return value }
)";
    for (const auto& body : std::vector<std::string> {
            "    while true {\n"
            "        let moved = relay(value: consume value, later: if true { break } else { 0 })\n"
            "        joyeer_intrinsic_move_in<T>(address: 4096, value: consume moved)\n"
            "    }\n",
            "    while true {\n"
            "        let moved = relay(value: consume value, later: if true { continue } else { 0 })\n"
            "        joyeer_intrinsic_move_in<T>(address: 4096, value: consume moved)\n"
            "    }\n",
        }) {
        SCOPED_TRACE(body);
        checkPrelude(prefix + "@compiler_transfer()\nfunc lose<T>(value: consuming T): Void {\n" + body + "}\n");
        ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
        const auto analysis = joyeer::analysis::Analyzer().analyze(checking.model);
        EXPECT_TRUE(std::any_of(analysis.diagnostics.begin(), analysis.diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.id == joyeer::analysis::DiagnosticId::linearNotConsumed &&
                    diagnostic.message.find("enclosing") != std::string::npos;
        })) << joyeer::analysis::dump(analysis.diagnostics);
    }
    // A loop that lives entirely inside the later argument leaves the pending move alone.
    checkPrelude(prefix + R"(
@compiler_transfer()
func forward<T>(value: consuming T): T {
    return relay(value: consume value, later: if true {
        var index = 0
        while index < 3 {
            index = index + 1
            continue
        }
        index
    } else { 0 })
}
)");
    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    const auto accepted = joyeer::analysis::Analyzer().analyze(checking.model);
    EXPECT_TRUE(accepted.succeeded()) << joyeer::analysis::dump(accepted.diagnostics);
}

TEST_F(CInteropTest, VerifierChecksEachPointerConversionRule) {
    using joyeer::ir::Opcode;
    using joyeer::ir::ValueCategory;
    using joyeer::typing::TypeKind;
    const auto build = [](const auto& mutate) {
        joyeer::ir::Module value;
        value.sourceName = "conversion";
        value.types = {
            {0, "Int", TypeKind::integer}, {1, "Void", TypeKind::voidType},
            {2, "Double", TypeKind::float64},
            {3, "(consuming Int): Void", TypeKind::function,
             joyeer::semantic::invalidSymbolId, {0, 1}, {joyeer::syntax::AccessEffect::consuming}},
        };
        joyeer::ir::Function entry;
        entry.id = 0;
        entry.name = "entry";
        entry.resultType = 1;
        entry.runtimeContext = true;
        entry.entry = 0;
        joyeer::ir::Parameter parameter;
        parameter.value = {0, 0, ValueCategory::value};
        parameter.isConsuming = true;
        entry.parameters.push_back(parameter);
        entry.blocks = {{0, "entry", {{Opcode::returnVoid}}}};
        joyeer::ir::Function caller;
        caller.id = 1;
        caller.name = "caller";
        caller.resultType = 1;
        caller.runtimeContext = true;
        caller.entry = 0;
        joyeer::ir::Instruction address {Opcode::functionAddress};
        address.result = joyeer::ir::Value {0, 3, ValueCategory::value};
        address.callee = 0;
        joyeer::ir::Instruction conversion {Opcode::pointerToInteger};
        conversion.result = joyeer::ir::Value {1, 0, ValueCategory::value};
        conversion.operands = {0};
        caller.blocks = {{0, "entry", {address, conversion, {Opcode::returnVoid}}}};
        value.functions = {entry, caller};
        mutate(value);
        return value;
    };
    const auto rejects = [](const joyeer::ir::Module& module, const std::string& message) {
        const auto verification = joyeer::ir::Verifier().verify(module);
        return std::any_of(verification.errors.begin(), verification.errors.end(), [&](const auto& error) {
            return error.message.find(message) != std::string::npos;
        });
    };
    const std::string conversionRule = "a pointer conversion takes";
    const std::string callbackRule = "a runtime callback address requires";
    const auto valid = build([](auto&) {});
    EXPECT_FALSE(rejects(valid, conversionRule));
    EXPECT_FALSE(rejects(valid, callbackRule));
    EXPECT_TRUE(rejects(build([](auto& module) { module.functions[1].blocks[0].instructions[1].operands = {}; }),
            conversionRule));
    EXPECT_TRUE(rejects(build([](auto& module) { module.functions[1].blocks[0].instructions[1].result.reset(); }),
            conversionRule));
    EXPECT_TRUE(rejects(build([](auto& module) { module.functions[1].blocks[0].instructions[1].result->type = 2; }),
            conversionRule));
    EXPECT_TRUE(rejects(build([](auto& module) { module.functions[1].blocks[0].instructions[1].result->type = 1; }),
            conversionRule));
    // The operand must be a function value defined by a named function address.
    EXPECT_TRUE(rejects(build([](auto& module) {
        auto& caller = module.functions[1];
        joyeer::ir::Parameter parameter;
        parameter.value = {0, 3, ValueCategory::value};
        caller.parameters.push_back(parameter);
        caller.blocks[0].instructions.erase(caller.blocks[0].instructions.begin());
    }), callbackRule));
    EXPECT_TRUE(rejects(build([](auto& module) { module.functions[0].runtimeContext = false; }), callbackRule));
    EXPECT_TRUE(rejects(build([](auto& module) { module.functions[1].runtimeContext = false; }),
            "requires runtime authorization"));
    EXPECT_TRUE(rejects(build([](auto& module) { module.functions[0].isExternal = true; }), callbackRule));
}

TEST_F(CInteropTest, LinearHandleHelpUsesTheDeclaredEndingMethod) {
    check(R"(
func work(value: consuming Int): Int { return value }
func main() {
    let handle = spawn(function: work, input: consume 1)
    return
}
)", false);
    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    auto analysis = joyeer::analysis::Analyzer().analyze(checking.model);
    EXPECT_TRUE(std::any_of(analysis.diagnostics.begin(), analysis.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.id == joyeer::analysis::DiagnosticId::linearNotConsumed &&
                diagnostic.help.has_value() && diagnostic.help->find("join") != std::string::npos;
    }));
    const auto root = parse("func main() { let ticket: Ticket<Int> = Ticket(word: 4096) }\n", 0);
    const auto library = parse(R"(
@compiler_transfer()
@compiler_role(task_handle)
public linear struct Ticket<R> {
    public var word: Int
    @compiler_role(stored_consumer)
    public consuming func finish(): R {
        let address = consume self.word
        return joyeer_intrinsic_move_out<R>(address: address)
    }
}
)", 1, true, true);
    checkModules({{"app", {root}}, {"runtime.fixture", {library}, true, false, true}});
    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    analysis = joyeer::analysis::Analyzer().analyze(checking.model);
    EXPECT_TRUE(std::any_of(analysis.diagnostics.begin(), analysis.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.id == joyeer::analysis::DiagnosticId::linearNotConsumed &&
                diagnostic.help.has_value() && diagnostic.help->find("finish") != std::string::npos;
    }));
}

TEST_F(CInteropTest, VerifierChecksTheCallbackContractAfterAddressErasure) {
    using joyeer::ir::Opcode;
    using joyeer::ir::ValueCategory;
    using joyeer::typing::TypeKind;
    for (const bool native : {false, true}) {
        joyeer::ir::Module value;
        value.sourceName = "callback";
        value.types = {
            {0, "Int", TypeKind::integer}, {1, "Void", TypeKind::voidType},
            {2, "Double", TypeKind::float64},
            {3, "(consuming Double): Void", TypeKind::function,
             joyeer::semantic::invalidSymbolId, {2, 1}, {joyeer::syntax::AccessEffect::consuming}},
        };
        joyeer::ir::Function entry;
        entry.id = 0;
        entry.name = "entry";
        entry.resultType = 1;
        entry.runtimeContext = true;
        entry.entry = 0;
        joyeer::ir::Parameter parameter;
        parameter.value = {0, 2, ValueCategory::value};
        parameter.isConsuming = true;
        entry.parameters.push_back(parameter);
        entry.blocks = {{0, "entry", {{Opcode::returnVoid}}}};
        if (native) entry.nativeBinding = joyeer::NativeBinding {joyeer::NativeBindingKind::cExport, "native_entry"};
        joyeer::ir::Function caller;
        caller.id = 1;
        caller.name = "caller";
        caller.resultType = 1;
        caller.runtimeContext = true;
        caller.entry = 0;
        joyeer::ir::Instruction address {Opcode::functionAddress};
        address.result = joyeer::ir::Value {0, 3, ValueCategory::value};
        address.callee = 0;
        joyeer::ir::Instruction conversion {Opcode::pointerToInteger};
        conversion.result = joyeer::ir::Value {1, 0, ValueCategory::value};
        conversion.operands = {0};
        caller.blocks = {{0, "entry", {address, conversion, {Opcode::returnVoid}}}};
        value.functions = {entry, caller};
        const auto rejectsCallback = [](const auto& module) {
            const auto verification = joyeer::ir::Verifier().verify(module);
            return std::any_of(verification.errors.begin(), verification.errors.end(), [](const auto& error) {
                return error.message.find("runtime callback address") != std::string::npos;
            });
        };
        EXPECT_TRUE(rejectsCallback(value));
        joyeer::llvmbackend::wire::Writer writer;
        writer(value);
        joyeer::ir::Module decoded;
        joyeer::llvmbackend::wire::Reader reader(writer.bytes);
        reader(decoded);
        ASSERT_TRUE(reader.succeeded());
        EXPECT_TRUE(rejectsCallback(decoded));
        auto duplicateBlocks = value;
        auto& body = duplicateBlocks.functions[1].blocks.front().instructions;
        body[0].result->id = 1;
        body[1].operands = {1};
        body[1].result->id = 2;
        joyeer::ir::Instruction unit {Opcode::unitConstant};
        unit.result = joyeer::ir::Value {0, 1, ValueCategory::value};
        body.insert(body.begin(), unit);
        duplicateBlocks.functions[1].blocks.insert(
                duplicateBlocks.functions[1].blocks.begin(), {0, "duplicate", {{Opcode::returnVoid}}});
        EXPECT_TRUE(rejectsCallback(duplicateBlocks));
        joyeer::llvmbackend::wire::Writer duplicates;
        duplicates(duplicateBlocks);
        joyeer::llvmbackend::wire::Reader duplicateReader(duplicates.bytes);
        duplicateReader(decoded);
        ASSERT_TRUE(duplicateReader.succeeded());
        EXPECT_TRUE(rejectsCallback(decoded));
    }
}

TEST_F(CInteropTest, VerifierRejectsCopiesOfDirectAndNestedDeinitOwners) {
    using joyeer::ir::Opcode;
    using joyeer::ir::ValueCategory;
    for (const auto& type : {"Owner", "[Owner]", "Owner?", "Result<Owner, Int>", "Wrapper"}) {
        SCOPED_TRACE(type);
        ASSERT_NO_FATAL_FAILURE(lower(std::string(R"(
struct Owner {
    var value: Int
    deinit() {}
}
struct Wrapper { var owner: Owner }
func read(value: )") + type + "): Int { return 0 }\n"));
        auto& read = function("read");
        ASSERT_FALSE(read.parameters.empty());
        uint32_t nextValue = 0;
        for (const auto& block : read.blocks) {
            for (const auto& instruction : block.instructions) {
                if (instruction.result.has_value()) nextValue = std::max(nextValue, instruction.result->id + 1);
            }
        }
        joyeer::ir::Instruction copy {Opcode::copyValue};
        copy.result = joyeer::ir::Value {nextValue, read.parameters[0].value.type, ValueCategory::value};
        copy.operands = {read.parameters[0].value.id};
        read.blocks.front().instructions.insert(read.blocks.front().instructions.begin(), copy);
        const auto verification = joyeer::ir::Verifier().verify(*module);
        EXPECT_TRUE(std::any_of(verification.errors.begin(), verification.errors.end(), [](const auto& error) {
            return error.message.find("noncopyable") != std::string::npos;
        })) << joyeer::ir::dump(verification);
    }
}
