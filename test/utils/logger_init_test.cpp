#include <catch2/catch_test_macros.hpp>
#include "logger_init.h"

TEST_CASE("Library logger is separate from spdlog's default", "[utils]") {
    const auto defaultBefore = spdlog::default_logger();

    SECTION("the library has its own named logger") {
        REQUIRE(dist_logger() != nullptr);
        REQUIRE(dist_logger()->name() == "distributed");
        REQUIRE(dist_logger() != spdlog::default_logger());
    }

    SECTION("silencing the library leaves the default logger alone") {
        use_null_logger();

        REQUIRE(spdlog::default_logger() == defaultBefore);
        REQUIRE(spdlog::default_logger() != nullptr);
    }

    SECTION("configuring a file logger leaves the default logger alone") {
        use_file_logger("logs/logger_init_test.log", false);

        REQUIRE(spdlog::default_logger() == defaultBefore);
        REQUIRE(dist_logger()->name() == "distributed");

        use_null_logger();
    }

    SECTION("the logger object survives reconfiguration") {
        const auto before = dist_logger();
        use_file_logger("logs/logger_init_test.log", false);
        const auto after = dist_logger();
        use_null_logger();

        // Only the sinks are swapped, so references held elsewhere stay valid.
        REQUIRE(before == after);
    }
}
