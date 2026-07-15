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

} // namespace
