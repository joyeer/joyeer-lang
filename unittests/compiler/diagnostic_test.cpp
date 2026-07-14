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

} // namespace
