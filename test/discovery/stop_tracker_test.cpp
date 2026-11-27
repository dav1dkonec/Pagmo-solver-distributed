#include <catch2/catch_test_macros.hpp>
#include "stop_tracker.h"

#include <atomic>
#include <limits>
#include <thread>
#include <vector>

namespace
{
    // A fresh candidate for every call, so that none of them matches a received individual
    std::vector<double> next_x()
    {
        static std::atomic<int> counter{0};
        return {static_cast<double>(++counter)};
    }
}

TEST_CASE("Stop tracker - stopping point", "[stop-tracker]")
{
    SECTION("finds the last window that still improved by epsilon") {
        // Window 2, eps 1 %: f(0..7) = 10, 10, 10, 9, 8, 8, 8, 8; last significant window is f(3)=9 -> f(5)=8
        stop_tracker tracker{10.0, 2, 0.01};
        for (const double f : {10.0, 10.0, 9.0, 8.0, 8.0, 8.0, 8.0})
        {
            tracker.record(next_x(), f);
        }

        const auto point = tracker.result();
        REQUIRE(point.evaluations == 7);
        REQUIRE(point.stopEvaluation == 5);
        REQUIRE(point.converged);
    }

    SECTION("an early plateau does not end the search") {
        // The old first-plateau rule would stop at e = 2; the later improvement moves the point
        stop_tracker tracker{10.0, 2, 0.01};
        for (const double f : {10.0, 10.0, 10.0, 10.0, 5.0, 5.0, 5.0})
        {
            tracker.record(next_x(), f);
        }
        // Window ending at 6 is f(4)=10 -> f(6)=5, and only one quiet evaluation followed
        REQUIRE(tracker.result().stopEvaluation == 6);
        REQUIRE_FALSE(tracker.result().converged);
    }

    SECTION("a unit that keeps improving has not converged") {
        stop_tracker tracker{100.0, 3, 0.001};
        double f = 100.0;
        for (int i = 0; i < 50; ++i)
        {
            f *= 0.9;
            tracker.record(next_x(), f);
        }

        REQUIRE(tracker.result().stopEvaluation == 50);
        REQUIRE_FALSE(tracker.result().converged);
    }

    SECTION("it has converged only after a full window without improvement") {
        stop_tracker tracker{2.0, 3, 0.001};
        tracker.record(next_x(), 1.0);
        tracker.record(next_x(), 1.0);
        tracker.record(next_x(), 1.0);
        // The window ending at 3 still reaches back to f(0) = 2
        REQUIRE(tracker.result().stopEvaluation == 3);

        for (int i = 0; i < 2; ++i)
        {
            tracker.record(next_x(), 1.0);
        }
        REQUIRE_FALSE(tracker.result().converged);
        tracker.record(next_x(), 1.0);
        REQUIRE(tracker.result().converged);
    }

    SECTION("no improvement at all leaves no point") {
        stop_tracker tracker{5.0, 2, 0.001};
        for (const double f : {7.0, 6.0, 5.0, 5.0})
        {
            tracker.record(next_x(), f);
        }
        REQUIRE(tracker.result().stopEvaluation == 0);
        REQUIRE(tracker.result().converged);
    }

    SECTION("a zero fitness does not count as improving") {
        stop_tracker tracker{0.0, 1, 0.001};
        tracker.record(next_x(), 0.0);
        REQUIRE(tracker.result().stopEvaluation == 0);
    }

    SECTION("an unevaluable candidate counts but cannot be the best") {
        stop_tracker tracker{5.0, 2, 0.001};
        tracker.record(next_x(), std::numeric_limits<double>::quiet_NaN());
        REQUIRE(tracker.result().stopEvaluation == 0);
        tracker.record(next_x(), 4.0);
        REQUIRE(tracker.result().evaluations == 2);
        REQUIRE(tracker.result().stopEvaluation == 2);
    }

    SECTION("the first finite fitness after an infinite start is significant") {
        stop_tracker tracker{std::numeric_limits<double>::infinity(), 1, 0.001};
        tracker.record(next_x(), 3.0);
        REQUIRE(tracker.result().stopEvaluation == 1);
        tracker.record(next_x(), 3.0);
        REQUIRE(tracker.result().stopEvaluation == 1);
        REQUIRE(tracker.result().converged);
    }

    SECTION("re-evaluating a received individual is not counted") {
        const std::vector<std::vector<double>> received{{-1.0}, {-2.0}};
        stop_tracker tracker{5.0, 2, 0.001, received};
        tracker.record({-1.0}, 5.0);
        tracker.record({-2.0}, 6.0);
        REQUIRE(tracker.result().evaluations == 0);

        tracker.record(next_x(), 4.0);
        REQUIRE(tracker.result().evaluations == 1);
    }

    SECTION("the curve samples the best every stride evaluations, from f(0)") {
        stop_tracker tracker{10.0, 2, 0.001, {}, 2};
        for (const double f : {9.0, 11.0, 8.0, 8.5, 7.0})
        {
            tracker.record(next_x(), f);
        }
        REQUIRE(tracker.result().bestCurve == std::vector<double>{10.0, 9.0, 8.0});
    }

    SECTION("a zero window is refused") {
        REQUIRE_THROWS_AS((stop_tracker{1.0, 0, 0.001}), std::invalid_argument);
    }
}

TEST_CASE("Stop tracker - concurrent evaluations", "[stop-tracker]")
{
    // Parallel batch evaluation records from several threads; every evaluation must count
    stop_tracker tracker{2.0, 1000, 0.001};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
    {
        threads.emplace_back([&tracker] {
            for (int i = 0; i < 2500; ++i)
            {
                tracker.record(next_x(), 1.0);
            }
        });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }

    // Only the windows ending at e <= W reach back to f(0) = 2
    REQUIRE(tracker.result().evaluations == 10000);
    REQUIRE(tracker.result().stopEvaluation == 1000);
    REQUIRE(tracker.result().converged);
}
