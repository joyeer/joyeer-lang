#include "joyeer/backend/llvm.h"
#include "joyeer/compiler/hostbuiltins.h"
#include "joyeer/compiler/irlowering.h"
#include "joyeer/compiler/lexparser.h"
#include "joyeer/compiler/nameresolution.h"
#include "joyeer/compiler/parser.h"
#include "joyeer/compiler/sourcefile.h"
#include "joyeer/compiler/typechecking.h"
#include "joyeer/diagnostic/diagnostic.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr auto hostSource = R"JOYEER(
func read(path: String): Result<String, FileSystemError> {
    return readFileUtf8(path: path)
}
func prefix(path: String, maximumBytes: Int): Result<String, FileSystemError> {
    return readFilePrefix(path: path, maximumBytes: maximumBytes)
}
func write(path: String, contents: String): Result<Void, FileSystemError> {
    writeFileNew(path: path, contents: contents)?
    return .Ok(())
}
func create(path: String): Result<Void, FileSystemError> {
    return createDirectory(path: path)
}
func list(path: String): Result<[String], FileSystemError> {
    return listDirectory(path: path)
}
func kind(path: String): Result<FileKind, FileSystemError> {
    return fileKind(path: path)
}
func unlink(path: String): Result<Void, FileSystemError> {
    return removeFile(path: path)
}
func rmdir(path: String): Result<Void, FileSystemError> {
    return removeDirectory(path: path)
}
func join(base: String, path: String): Result<String, FileSystemError> {
    return joinPath(base: base, path: path)
}
func run(executable: String, arguments: [String], directory: String): Result<ProcessStatus, ProcessError> {
    return runProcess(executable: executable, arguments: arguments, workingDirectory: directory)
}
func stderr(contents: String): Result<Void, StderrError> {
    writeStderr(contents: contents)?
    return .Ok(())
}
func stderrCode(error: StderrError): Int {
    return match error { .WriteFailed(code) => code }
}
func describe(kind: FileKind): Int {
    return match kind {
        .File => 0,
        .Directory => 1,
        .Symlink => 2,
        .Other => 3,
    }
}
func statusCode(status: ProcessStatus): Int {
    return match status {
        .Exited(code) => code,
        .Signaled(signal) => signal,
    }
}
)JOYEER";

class HostBuiltinTest : public testing::Test {
protected:
    void check(const std::string& text) {
        diagnostics.errors.clear();
        source = std::make_shared<SourceFile>(text);
        LexParser(&diagnostics).parse(source);
        ASSERT_TRUE(diagnostics.errors.empty());
        const auto parsed = joyeer::parser::Parser(source->tokens).parse();
        ASSERT_TRUE(parsed.succeeded()) << joyeer::parser::dump(parsed.diagnostics);
        resolution = joyeer::semantic::NameResolver().resolve(parsed.root);
        ASSERT_TRUE(resolution.succeeded()) << joyeer::semantic::dump(resolution.diagnostics);
        checking = joyeer::typing::TypeChecker().check(resolution.model);
        ASSERT_NE(checking.model, nullptr);
    }

    Diagnostics diagnostics;
    SourceFile::Ptr source;
    joyeer::semantic::NameResolutionResult resolution;
    joyeer::typing::TypeCheckingResult checking;
};

TEST_F(HostBuiltinTest, AcceptsExactPublicSignaturesAndUnitPropagation) {
    ASSERT_NO_FATAL_FAILURE(check(hostSource));
    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    const auto* prelude = resolution.model->scope(resolution.model->preludeScope());
    ASSERT_NE(prelude, nullptr);
    for (const auto& descriptor : joyeer::hostbuiltins::functions) {
        const auto found = prelude->values.find(std::string(descriptor.name));
        ASSERT_NE(found, prelude->values.end()) << descriptor.name;
        const auto* symbol = resolution.model->symbol(found->second);
        ASSERT_NE(symbol, nullptr);
        ASSERT_TRUE(symbol->callable.has_value());
        EXPECT_EQ(symbol->callable->parameters.size(), descriptor.parameters.size());
        for (size_t index = 0; index < descriptor.parameters.size(); ++index) {
            EXPECT_EQ(symbol->callable->parameters[index].label,
                      std::string(descriptor.parameters[index].name));
            EXPECT_EQ(symbol->callable->parameters[index].access,
                      joyeer::syntax::AccessEffect::borrowing);
        }
    }
}

TEST_F(HostBuiltinTest, PreservesLegacyFileInputErrorCases) {
    ASSERT_NO_FATAL_FAILURE(check(
            "func old(path: String): Result<String, IOError> { return readFile(path: path) }\n"
            "func code(error: IOError): Int { return match error {\n"
            ".NotFound(n) => n, .PermissionDenied(n) => n,\n"
            ".InvalidPath(n) => n, .Other(n) => n } }\n"));
    EXPECT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
}

TEST_F(HostBuiltinTest, ProvidesConcretePrefixSignatureAndPreservesFilesystemCases) {
    ASSERT_NO_FATAL_FAILURE(check(
            "func bounded(path: String, maximumBytes: Int): Result<String, FileSystemError> {\n"
            "return .Ok(readFilePrefix(path: path, maximumBytes: maximumBytes)?)\n}\n"
            "func code(error: FileSystemError): Int { return match error {\n"
            ".InvalidPath(n) => n, .NotFound(n) => n, .PermissionDenied(n) => n,\n"
            ".AlreadyExists(n) => n, .NotDirectory(n) => n, .IsDirectory(n) => n,\n"
            ".Other(n) => n } }\n"));
    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    const auto* descriptor = joyeer::hostbuiltins::findFunction("readFilePrefix");
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->runtimeName, "joyeer_fs_read_file_prefix_abi");
    EXPECT_EQ(descriptor->result, joyeer::hostbuiltins::ValueKind::string);
    EXPECT_EQ(descriptor->error, "FileSystemError");
    ASSERT_EQ(descriptor->parameters.size(), 2u);
    EXPECT_EQ(descriptor->parameters[0].name, "path");
    EXPECT_EQ(descriptor->parameters[0].kind, joyeer::hostbuiltins::ValueKind::string);
    EXPECT_EQ(descriptor->parameters[1].name, "maximumBytes");
    EXPECT_EQ(descriptor->parameters[1].kind, joyeer::hostbuiltins::ValueKind::integer);

    const auto* prelude = resolution.model->scope(resolution.model->preludeScope());
    ASSERT_NE(prelude, nullptr);
    const auto found = prelude->values.find("readFilePrefix");
    ASSERT_NE(found, prelude->values.end());
    const auto* callable = checking.model->callable(found->second);
    ASSERT_NE(callable, nullptr);
    EXPECT_EQ(callable->parameters,
              (std::vector<joyeer::typing::TypeId> {
                  checking.model->types().stringType(), checking.model->types().intType(),
              }));
    EXPECT_EQ(checking.model->types().displayName(callable->result),
              "Result<String, FileSystemError>");
    const auto* filesystem = joyeer::hostbuiltins::findEnumeration("FileSystemError");
    ASSERT_NE(filesystem, nullptr);
    EXPECT_EQ(filesystem->cases.size(), 7u);
    EXPECT_TRUE(filesystem->integerPayload);
    EXPECT_EQ(filesystem->firstAbiTag, 1);
}

TEST_F(HostBuiltinTest, ProvidesConcreteStderrSignatureAndErrorPayload) {
    ASSERT_NO_FATAL_FAILURE(check(
            "func f(contents: String): Result<Void, StderrError> {\n"
            "return writeStderr(contents: contents)\n}\n"
            "func failed(code: Int): Result<Void, StderrError> {\n"
            "return .Err(.WriteFailed(code))\n}\n"));
    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    const auto* prelude = resolution.model->scope(resolution.model->preludeScope());
    ASSERT_NE(prelude, nullptr);
    const auto found = prelude->values.find("writeStderr");
    ASSERT_NE(found, prelude->values.end());
    const auto* callable = checking.model->callable(found->second);
    ASSERT_NE(callable, nullptr);
    EXPECT_EQ(callable->parameters,
              (std::vector<joyeer::typing::TypeId> { checking.model->types().stringType() }));
    EXPECT_EQ(checking.model->types().displayName(callable->result),
              "Result<Void, StderrError>");
    const auto* descriptor = joyeer::hostbuiltins::findEnumeration("StderrError");
    ASSERT_NE(descriptor, nullptr);
    ASSERT_EQ(descriptor->cases.size(), 1u);
    EXPECT_EQ(descriptor->cases[0], "WriteFailed");
    EXPECT_TRUE(descriptor->integerPayload);
    EXPECT_EQ(descriptor->firstAbiTag, 1);
}

TEST_F(HostBuiltinTest, RejectsIncorrectInputsResultsAndImplicitErrorConversion) {
    for (const auto* text : {
            "func f() { readFileUtf8(path: 42) }\n",
            "func f() { readFilePrefix(path: 42, maximumBytes: 1) }\n",
            "func f() { readFilePrefix(path: \"x\", maximumBytes: \"1\") }\n",
            "func f() { readFilePrefix(path: \"x\", maximumBytes: b'a') }\n",
            "func f(): Result<Int, FileSystemError> { return readFilePrefix(path: \"x\", maximumBytes: 1) }\n",
            "func f(): Result<String, IOError> { return .Ok(readFilePrefix(path: \"x\", maximumBytes: 1)?) }\n",
            "func f() { writeFileNew(path: \"x\", contents: [b'a']) }\n",
            "func f() { runProcess(executable: \"x\", arguments: [1], workingDirectory: \".\") }\n",
            "func f() { runProcess(executable: \"x\", arguments: [], workingDirectory: 1) }\n",
            "func f(): Result<Int, FileSystemError> { return fileKind(path: \"x\") }\n",
            "func f(): Result<String, IOError> { return .Ok(readFileUtf8(path: \"x\")?) }\n",
            "func f(): Result<String, FileSystemError> { return .Ok(readFile(path: \"x\")?) }\n",
            "func f(): Result<Void, FileSystemError> {\nrunProcess(executable: \"x\", arguments: [], workingDirectory: \".\")?\nreturn .Ok(())\n}\n",
            "func f() { writeStderr(contents: 42) }\n",
            "func f() { writeStderr(contents: [b'a']) }\n",
            "func f(): Result<String, StderrError> { return writeStderr(contents: \"x\") }\n",
            "func f(): Result<Void, IOError> { return writeStderr(contents: \"x\") }\n",
            "func f(): Result<Void, FileSystemError> {\nwriteStderr(contents: \"x\")?\nreturn .Ok(())\n}\n",
            "func f(): Result<Void, StderrError> {\nwriteFileNew(path: \"x\", contents: \"y\")?\nreturn .Ok(())\n}\n",
            "func f(): StderrError { return .WriteFailed(\"not an Int\") }\n",
        }) {
        SCOPED_TRACE(text);
        ASSERT_NO_FATAL_FAILURE(check(text));
        EXPECT_FALSE(checking.succeeded());
    }
}

TEST_F(HostBuiltinTest, EmitsEveryStableHostAbiAndFullDebugLocations) {
    ASSERT_NO_FATAL_FAILURE(check(hostSource));
    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    const auto lowered = joyeer::lowering::Lowerer().lower(
            checking.model, "host.joyeer",
            joyeer::ir::SourceInfo {
                "host.joyeer", ".", static_cast<uint64_t>(source->content.size()),
                source->lineStarts,
            });
    ASSERT_TRUE(lowered.succeeded()) << joyeer::lowering::dump(lowered.diagnostics);
    for (const auto fullDebug : { false, true }) {
        SCOPED_TRACE(fullDebug);
        const auto emitted = joyeer::llvmbackend::Emitter().emit(
                *lowered.module, joyeer::llvmbackend::EmitOptions {
                    fullDebug, joyeer::DebugInfoFormat::dwarf,
                    joyeer::OptimizationLevel::O0, fullDebug,
                });
        ASSERT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
        for (const auto& descriptor : joyeer::hostbuiltins::functions) {
            EXPECT_NE(emitted.text.find("call i32 @" + std::string(descriptor.runtimeName) + "("),
                      std::string::npos) << descriptor.name;
        }
        for (const auto signature : std::array<std::string_view, 7> {
                "declare i32 @joyeer_fs_read_file_abi(ptr, ptr, ptr, i64)",
                "declare i32 @joyeer_fs_read_file_prefix_abi(ptr, ptr, ptr, i64, i64)",
                "declare i32 @joyeer_fs_write_file_new_abi(ptr, ptr, i64, ptr, i64)",
                "declare i32 @joyeer_fs_list_directory_abi(ptr, ptr, ptr, i64)",
                "declare i32 @joyeer_fs_create_directory_abi(ptr, ptr, i64)",
                "declare i32 @joyeer_run_process_abi(ptr, ptr, ptr, ptr, i64, ptr, i64, ptr, i64)",
                "declare i32 @joyeer_write_stderr_abi(ptr, ptr, i64)",
             }) {
            EXPECT_NE(emitted.text.find(signature), std::string::npos) << signature;
        }
        EXPECT_EQ(emitted.text.find("load void"), std::string::npos);
        EXPECT_EQ(emitted.text.find("store void"), std::string::npos);
        if (fullDebug) EXPECT_NE(emitted.text.find("!DILocation("), std::string::npos);
    }
}

TEST_F(HostBuiltinTest, LowersPrefixLimitAsOneScalarAfterPathBytes) {
    ASSERT_NO_FATAL_FAILURE(check(
            "func bounded(path: String): Result<String, FileSystemError> {\n"
            "return readFilePrefix(path: path, maximumBytes: 65537)\n}\n"));
    ASSERT_TRUE(checking.succeeded()) << joyeer::typing::dump(checking.diagnostics);
    const auto lowered = joyeer::lowering::Lowerer().lower(
            checking.model, "prefix.joyeer",
            joyeer::ir::SourceInfo {
                "prefix.joyeer", ".", static_cast<uint64_t>(source->content.size()),
                source->lineStarts,
            });
    ASSERT_TRUE(lowered.succeeded()) << joyeer::lowering::dump(lowered.diagnostics);
    const auto verified = joyeer::ir::Verifier().verify(*lowered.module);
    ASSERT_TRUE(verified.succeeded()) << joyeer::ir::dump(verified);
    const auto external = std::find_if(
            lowered.module->functions.begin(), lowered.module->functions.end(),
            [](const auto& function) { return function.name == "readFilePrefix"; });
    ASSERT_NE(external, lowered.module->functions.end());
    EXPECT_TRUE(external->isExternal);
    ASSERT_EQ(external->parameters.size(), 2u);
    EXPECT_EQ(external->parameters[0].value.type, checking.model->types().stringType());
    EXPECT_EQ(external->parameters[1].value.type, checking.model->types().intType());

    bool foundCall = false;
    for (const auto& function : lowered.module->functions) {
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (instruction.opcode != joyeer::ir::Opcode::call ||
                    instruction.callee != external->id) continue;
                foundCall = true;
                ASSERT_EQ(instruction.operands.size(), 2u);
                const auto limit = std::find_if(
                        block.instructions.begin(), block.instructions.end(),
                        [&](const auto& candidate) {
                            return candidate.result.has_value() &&
                                   candidate.result->id == instruction.operands[1];
                        });
                ASSERT_NE(limit, block.instructions.end());
                EXPECT_EQ(limit->opcode, joyeer::ir::Opcode::integerConstant);
                EXPECT_EQ(limit->integerValue, 65537);
            }
        }
    }
    EXPECT_TRUE(foundCall);
    for (const auto fullDebug : { false, true }) {
        const auto emitted = joyeer::llvmbackend::Emitter().emit(
                *lowered.module, joyeer::llvmbackend::EmitOptions {
                    fullDebug, joyeer::DebugInfoFormat::dwarf,
                    joyeer::OptimizationLevel::O0, fullDebug,
                });
        ASSERT_TRUE(emitted.succeeded()) << joyeer::llvmbackend::dump(emitted.diagnostics);
        EXPECT_NE(emitted.text.find(
                "declare i32 @joyeer_fs_read_file_prefix_abi(ptr, ptr, ptr, i64, i64)"),
                std::string::npos);
        const auto call = emitted.text.find("call i32 @joyeer_fs_read_file_prefix_abi(");
        ASSERT_NE(call, std::string::npos);
        const auto callEnd = emitted.text.find(')', call);
        ASSERT_NE(callEnd, std::string::npos);
        const auto callText = emitted.text.substr(call, callEnd - call + 1);
        EXPECT_NE(callText.find(", i64 65537)"), std::string::npos) << callText;
        EXPECT_EQ(std::count(callText.begin(), callText.end(), ','), 4);
        if (fullDebug) EXPECT_NE(emitted.text.find("!DILocation("), std::string::npos);
    }
}

}
