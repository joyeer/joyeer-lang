#include "compiler/codegen/llvm.h"
#include "compiler/frontend/lexparser.h"
#include "compiler/frontend/nameresolution.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/semanticanalysis.h"
#include "compiler/frontend/typechecking.h"
#include "compiler/ir/irlowering.h"
#include "backend/ir_wire.h"

#include <gtest/gtest.h>

#include <algorithm>
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
    joyeer::syntax::SourceFileSyntax::Ptr parse(const std::string& text, uint32_t sourceId = 0) {
        source = std::make_shared<SourceFile>(text);
        Diagnostics diagnostics;
        LexParser(&diagnostics).parse(source);
        EXPECT_FALSE(diagnostics.hasFailure());
        for (auto& token : source->tokens) token->span.sourceId = sourceId;
        auto result = joyeer::parser::Parser(source->tokens).parse();
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

    bool has(joyeer::typing::TypeCheckingDiagnosticId id) const {
        return std::any_of(checking.diagnostics.begin(), checking.diagnostics.end(),
                          [id](const auto& diagnostic) { return diagnostic.id == id; });
    }

    void lower(const std::string& text) {
        check(text);
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
            "String", "Void", "()", "Int?", "[Int]",
            "Result<Int, IOError>", "inout Int", "consuming Int", "initializing Int",
        }) {
        SCOPED_TRACE(parameter);
        check("@c_extern(symbol: \"native\")\nfunc native(value: " + parameter + "): Int\n");
        EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::unsupportedNativeSignature));
    }
    for (const auto& result : {"String", "Int?", "Never"}) {
        SCOPED_TRACE(result);
        check("@c_extern(symbol: \"native\")\nfunc native(): " + std::string(result) + "\n");
        EXPECT_TRUE(has(joyeer::typing::TypeCheckingDiagnosticId::unsupportedNativeSignature));
    }
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
        EXPECT_EQ(emitted.text.find("@joyeer_panic"), std::string::npos);
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
            {"joyeer_panic", "(value: Int): Void", "let sum = value + 1\nprint(value: sum)"},
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
                ASSERT_NO_FATAL_FAILURE(lower(
                        std::string(attribute) + "(symbol: \"" + conflict.symbol + "\")\n" +
                        "func native" + conflict.signature + implementation + "\n" +
                        "func probe(value: Int): Void {\n" + conflict.body + "\n}\n"));
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

} // namespace
