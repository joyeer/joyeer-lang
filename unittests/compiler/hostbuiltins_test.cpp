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

#include <array>
#include <memory>
#include <string>
#include <string_view>

namespace {

constexpr auto hostSource = R"JOYEER(
func read(path: String): Result<String, FileSystemError> {
    return readFileUtf8(path: path)
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

TEST_F(HostBuiltinTest, RejectsIncorrectInputsResultsAndImplicitErrorConversion) {
    for (const auto* text : {
            "func f() { readFileUtf8(path: 42) }\n",
            "func f() { writeFileNew(path: \"x\", contents: [b'a']) }\n",
            "func f() { runProcess(executable: \"x\", arguments: [1], workingDirectory: \".\") }\n",
            "func f() { runProcess(executable: \"x\", arguments: [], workingDirectory: 1) }\n",
            "func f(): Result<Int, FileSystemError> { return fileKind(path: \"x\") }\n",
            "func f(): Result<String, IOError> { return .Ok(readFileUtf8(path: \"x\")?) }\n",
            "func f(): Result<String, FileSystemError> { return .Ok(readFile(path: \"x\")?) }\n",
            "func f(): Result<Void, FileSystemError> {\nrunProcess(executable: \"x\", arguments: [], workingDirectory: \".\")?\nreturn .Ok(())\n}\n",
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
        for (const auto signature : std::array<std::string_view, 5> {
                "declare i32 @joyeer_fs_read_file_abi(ptr, ptr, ptr, i64)",
                "declare i32 @joyeer_fs_write_file_new_abi(ptr, ptr, i64, ptr, i64)",
                "declare i32 @joyeer_fs_list_directory_abi(ptr, ptr, ptr, i64)",
                "declare i32 @joyeer_fs_create_directory_abi(ptr, ptr, i64)",
                "declare i32 @joyeer_run_process_abi(ptr, ptr, ptr, ptr, i64, ptr, i64, ptr, i64)",
             }) {
            EXPECT_NE(emitted.text.find(signature), std::string::npos) << signature;
        }
        EXPECT_EQ(emitted.text.find("load void"), std::string::npos);
        EXPECT_EQ(emitted.text.find("store void"), std::string::npos);
        if (fullDebug) EXPECT_NE(emitted.text.find("!DILocation("), std::string::npos);
    }
}

}
