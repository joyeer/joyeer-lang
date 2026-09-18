#include "joyeer/diagnostic/diagnostic.h"

#include <gtest/gtest.h>

namespace {

TEST(DiagnosticsTest, ReportsDoNotCountAsFailures) {
	Diagnostics diagnostics;
	diagnostics.reportError(ErrorLevel::report, "warning");

	ASSERT_EQ(diagnostics.errors.size(), 1u);
	EXPECT_EQ(diagnostics.errors[0].level, ErrorLevel::report);
	EXPECT_FALSE(diagnostics.hasFailure());
}

TEST(DiagnosticsTest, DetectsFailuresAmongReports) {
	Diagnostics diagnostics;
	diagnostics.reportError(ErrorLevel::report, "warning");
	diagnostics.reportError(ErrorLevel::failure, "error");

	ASSERT_EQ(diagnostics.errors.size(), 2u);
	EXPECT_TRUE(diagnostics.hasFailure());
}

TEST(DiagnosticsTest, PrintsReportsAsWarnings) {
	Diagnostics diagnostics;
	diagnostics.reportError(ErrorLevel::report, 3, 7, "unreachable code");

	testing::internal::CaptureStdout();
	diagnostics.printErrors();
	const auto output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("Warning(line: 3)"), std::string::npos);
	EXPECT_EQ(output.find("SyntaxError"), std::string::npos);
}

TEST(DiagnosticsTest, PrintsStructuredSourceDiagnosticsWithOneBasedLocations) {
	Diagnostics diagnostics;
	const std::string source = "first\n  bad value\n";
	diagnostics.reportSourceDiagnostic(
			ErrorLevel::failure,
			"type-checking.type-mismatch",
			"sample.joyeer",
			source,
			{ 0, 6 },
			8,
			3,
			"cannot use this value");

	testing::internal::CaptureStdout();
	diagnostics.printErrors();
	const auto output = testing::internal::GetCapturedStdout();

	EXPECT_EQ(
			output,
			"sample.joyeer:2:3: error[type-checking.type-mismatch]: cannot use this value\n"
			"  2 |   bad value\n"
			"    |   ^~~\n");
}

TEST(DiagnosticsTest, PrintsStructuredDiagnosticsWithoutSourceContext) {
	Diagnostics diagnostics;
	diagnostics.reportDiagnostic(
			ErrorLevel::failure,
			"linker.missing-entry-point",
			"native executable requires 'func main()'");

	testing::internal::CaptureStdout();
	diagnostics.printErrors();
	const auto output = testing::internal::GetCapturedStdout();

	EXPECT_EQ(
			output,
			"error[linker.missing-entry-point]: native executable requires 'func main()'\n");
}

TEST(DiagnosticsTest, PrintsHelpAndSourceFixItEdits) {
	Diagnostics diagnostics;
	const std::string source = "take(value: text)\n";
	diagnostics.reportSourceDiagnostic(
			ErrorLevel::failure,
			"type-checking.invalid-consume-argument",
			"sample.joyeer",
			source,
			{ 0 },
			12,
			4,
			"consuming argument requires 'consume' at the call site",
			"insert 'consume' before this argument",
			DiagnosticFixIt { 12, 0, "consume " });

	testing::internal::CaptureStdout();
	diagnostics.printErrors();
	const auto output = testing::internal::GetCapturedStdout();

	EXPECT_EQ(
			output,
			"sample.joyeer:1:13: error[type-checking.invalid-consume-argument]: consuming argument requires 'consume' at the call site\n"
			"  1 | take(value: text)\n"
			"    |             ^~~~\n"
			"help: insert 'consume' before this argument\n"
			"fix-it: sample.joyeer:1:13:0: \"consume \"\n");
}

TEST(DiagnosticsTest, PrintsSecondarySourceNotesInOrder) {
	Diagnostics diagnostics;
	const std::string source = "first access\nmiddle access\nlast access\n";
	diagnostics.reportSourceDiagnostic(
			ErrorLevel::failure,
			"type-checking.overlapping-access",
			"sample.joyeer",
			source,
			{ 0, 13, 27 },
			27,
			4,
			"accesses overlap",
			std::nullopt,
			std::nullopt,
			{
				DiagnosticSourceNote { 0, 5, "first access occurs here" },
				DiagnosticSourceNote { 13, 6, "another access occurs here" },
			});

	ASSERT_EQ(diagnostics.errors.size(), 1u);
	EXPECT_TRUE(diagnostics.hasFailure());
	ASSERT_EQ(diagnostics.errors[0].notes.size(), 2u);

	testing::internal::CaptureStdout();
	diagnostics.printErrors();
	const auto output = testing::internal::GetCapturedStdout();

	EXPECT_EQ(
			output,
			"sample.joyeer:3:1: error[type-checking.overlapping-access]: accesses overlap\n"
			"  3 | last access\n"
			"    | ^~~~\n"
			"sample.joyeer:1:1: note: first access occurs here\n"
			"  1 | first access\n"
			"    | ^~~~~\n"
			"sample.joyeer:2:1: note: another access occurs here\n"
			"  2 | middle access\n"
			"    | ^~~~~~\n");
}

} // namespace
