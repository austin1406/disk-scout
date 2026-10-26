#include <gtest/gtest.h>

#include "format.hpp"

using diskscout::format_bytes;
using diskscout::json_escape;

TEST(FormatBytes, SmallValuesAreExactBytes) {
    EXPECT_EQ(format_bytes(0), "0 B");
    EXPECT_EQ(format_bytes(1023), "1023 B");
}

TEST(FormatBytes, UsesOneDecimalAndBinaryUnits) {
    EXPECT_EQ(format_bytes(1024), "1.0 KB");
    EXPECT_EQ(format_bytes(1536), "1.5 KB");
    EXPECT_EQ(format_bytes(1024ull * 1024), "1.0 MB");
    EXPECT_EQ(format_bytes(5ull * 1024 * 1024 * 1024), "5.0 GB");
}

TEST(JsonEscape, EscapesQuotesBackslashesAndControlCharacters) {
    EXPECT_EQ(json_escape("plain"), "plain");
    EXPECT_EQ(json_escape("say \"hi\""), "say \\\"hi\\\"");
    EXPECT_EQ(json_escape("C:\\Users\\me"), "C:\\\\Users\\\\me");
    EXPECT_EQ(json_escape("a\nb\tc"), "a\\nb\\tc");
    EXPECT_EQ(json_escape(std::string("x\x01y")), "x\\u0001y");
}
