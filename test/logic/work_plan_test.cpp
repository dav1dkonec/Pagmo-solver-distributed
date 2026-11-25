#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <numeric>
#include "work_plan.h"

namespace
{
    constexpr size_t MIN_ISLAND = 80;
    // One generation, so that the work rates below read as individuals per millisecond
    constexpr size_t GEN = 1;

    node_capability measured(const std::string& id, const size_t cores, const double nodeRate)
    {
        return {id, cores, nodeRate, true};
    }

    // Credited per core, as the solver does for a node that has not finished a unit yet
    node_capability unmeasured(const std::string& id, const size_t cores)
    {
        return {id, cores, static_cast<double>(cores), false};
    }

    // A wide node and a narrow one that is also slower per core, which is the case the planner
    // has to get right. Both figures come from a run of the real problem, per generation.
    const node_capability fast = measured("fast", 8, 8 * 0.00085);
    const node_capability slow = measured("slow", 4, 4 * 0.00038);
}

TEST_CASE("Work plan - predicted time", "[work-plan]") {
    SECTION("twice the individuals is twice the time") {
        REQUIRE(predicted_time(fast, 16 * MIN_ISLAND, GEN) == 2 * predicted_time(fast, 8 * MIN_ISLAND, GEN));
    }

    SECTION("twice the generations is twice the time") {
        REQUIRE(predicted_time(fast, 8 * MIN_ISLAND, 2 * GEN) == 2 * predicted_time(fast, 8 * MIN_ISLAND, GEN));
    }

    SECTION("a node with no measurement yet reports no time") {
        const node_capability idle{"idle", 4, 0, false};
        REQUIRE(predicted_time(idle, 320, GEN) == 0);
    }
}

TEST_CASE("Work plan - capacity within a deadline", "[work-plan]") {
    SECTION("a deadline too short for one island is worth nothing") {
        const double tooShort = predicted_time(slow, MIN_ISLAND, GEN) - 1;
        REQUIRE(capacity_within(slow, tooShort, GEN, MIN_ISLAND) == 0);
    }

    SECTION("capacity follows the deadline once it reaches one island") {
        const double deadline = predicted_time(slow, 10 * MIN_ISLAND, GEN);
        REQUIRE(capacity_within(slow, deadline, GEN, MIN_ISLAND) == Catch::Approx(10 * MIN_ISLAND).margin(1));
        REQUIRE(capacity_within(slow, 2 * deadline, GEN, MIN_ISLAND) == Catch::Approx(20 * MIN_ISLAND).margin(1));
    }
}

TEST_CASE("Work plan - generations that fit a deadline", "[work-plan]") {
    SECTION("it is the inverse of the predicted time") {
        // Equal nodes must get equal depth, so a share that exactly fits may not lose a
        // generation to rounding; 8 cores at 0.00121 with 160 individuals is a measured case
        const node_capability mac = measured("mac", 8, 8 * 0.00121);
        for (const auto& node : {fast, slow, mac}) {
            for (const size_t share : {2 * MIN_ISLAND, 4 * MIN_ISLAND, 8 * MIN_ISLAND}) {
                for (const size_t generations : {1, 2, 3, 50, 96}) {
                    const double deadline = predicted_time(node, share, generations);
                    REQUIRE(generations_within(node, share, deadline) == generations);
                }
            }
        }
    }

    SECTION("twice the deadline is twice the depth") {
        const size_t share = 4 * MIN_ISLAND;
        const double deadline = predicted_time(slow, share, 50);

        const auto once = generations_within(slow, share, deadline);
        const auto twice = generations_within(slow, share, 2 * deadline);
        REQUIRE(static_cast<double>(twice) == Catch::Approx(2.0 * static_cast<double>(once)).epsilon(0.05));
    }

    SECTION("a deadline too short for one generation is worth nothing") {
        const double tooShort = predicted_time(slow, MIN_ISLAND, GEN) / 2;
        REQUIRE(generations_within(slow, MIN_ISLAND, tooShort) == 0);
    }

    SECTION("a node with no share runs nothing") {
        REQUIRE(generations_within(fast, 0, 1000.0) == 0);
    }
}

TEST_CASE("Work plan - equal shares", "[work-plan]") {
    SECTION("the shares add up to the whole population") {
        for (const size_t population : {240, 960, 1920, 5000}) {
            const auto shares = equal_shares({fast, slow}, population, MIN_ISLAND);
            REQUIRE(std::accumulate(shares.begin(), shares.end(), size_t{0}) == population);
        }
    }

    SECTION("capability makes no difference to the size of a share") {
        const auto shares = equal_shares({fast, slow}, 1920, MIN_ISLAND);
        REQUIRE(shares.at(0) == shares.at(1));
    }

    SECTION("the slowest nodes drop out when the population cannot fill an island each") {
        // 160 individuals reach two islands of 80, so the last two of the four get nothing
        std::vector<node_capability> cluster(4, measured("node", 4, 4 * 0.0004));
        const auto shares = equal_shares(cluster, 2 * MIN_ISLAND, MIN_ISLAND);

        REQUIRE(shares.at(0) == MIN_ISLAND);
        REQUIRE(shares.at(1) == MIN_ISLAND);
        REQUIRE(shares.at(2) == 0);
        REQUIRE(shares.at(3) == 0);
    }

    SECTION("a lone node takes everything") {
        const auto shares = equal_shares({fast}, 960, MIN_ISLAND);
        REQUIRE(shares.size() == 1);
        REQUIRE(shares.at(0) == 960);
    }
}

TEST_CASE("Work plan - generations against the deadline", "[work-plan]") {
    SECTION("twice the generations fits half as many individuals into the same deadline") {
        const double deadline = 10 * MIN_ISLAND / (4 * 0.00038);
        REQUIRE(capacity_within(slow, deadline, 2 * GEN, MIN_ISLAND)
                < capacity_within(slow, deadline, GEN, MIN_ISLAND));
    }

    SECTION("the same generation count on every node does not change the split") {
        const auto one = balanced_shares({fast, slow}, 1920, GEN, MIN_ISLAND);
        const auto fifty = balanced_shares({fast, slow}, 1920, 50 * GEN, MIN_ISLAND);

        REQUIRE(fifty.at(1) == Catch::Approx(static_cast<double>(one.at(1))).epsilon(0.01));
    }
}

TEST_CASE("Work plan - balanced shares", "[work-plan]") {
    SECTION("the shares add up to the whole population") {
        for (const size_t population : {240, 960, 1920, 5000}) {
            const auto shares = balanced_shares({fast, slow}, population, GEN, MIN_ISLAND);
            REQUIRE(std::accumulate(shares.begin(), shares.end(), size_t{0}) == population);
        }
    }

    SECTION("a node that cannot shorten the cycle is left with nothing") {
        // The crawling node needs 8000 s for the one island it cannot go below, while the fast
        // node runs the whole population in 141 s
        const auto shares = balanced_shares({fast, measured("crawl", 4, 0.00001)}, 960, GEN, MIN_ISLAND);

        REQUIRE(shares.at(0) == 960);
        REQUIRE(shares.at(1) == 0);
    }

    SECTION("both nodes are used once the population is large enough") {
        const auto shares = balanced_shares({fast, slow}, 1920, GEN, MIN_ISLAND);

        REQUIRE(shares.at(1) >= 4 * MIN_ISLAND);
        REQUIRE(predicted_time(fast, shares.at(0), GEN)
                == Catch::Approx(predicted_time(slow, shares.at(1), GEN)).epsilon(0.05));
    }

    SECTION("an unmeasured node is risked with a minimum island per core and no more") {
        const auto shares = balanced_shares({unmeasured("fast", 8), unmeasured("slow", 4)}, 1920, GEN, MIN_ISLAND);

        REQUIRE(shares.at(1) == 4 * MIN_ISLAND);
        REQUIRE(shares.at(0) == 1920 - 4 * MIN_ISLAND);
    }

    SECTION("a measured node is held to no such ceiling") {
        const auto shares = balanced_shares({fast, slow}, 5000, GEN, MIN_ISLAND);
        REQUIRE(shares.at(1) > 4 * MIN_ISLAND);
    }

    SECTION("the fastest node is served first when the cluster outnumbers the population") {
        // Five narrow nodes could take 5 x 320 between them, more than the population, and the
        // leader must not be the one left out
        std::vector<node_capability> cluster{unmeasured("lead", 4)};
        for (int i = 0; i < 4; ++i) {
            cluster.push_back(unmeasured("node" + std::to_string(i), 4));
        }

        const auto shares = balanced_shares(cluster, 960, GEN, MIN_ISLAND);
        REQUIRE(shares.at(0) >= MIN_ISLAND);
        REQUIRE(std::accumulate(shares.begin(), shares.end(), size_t{0}) == 960);
    }

    SECTION("nodes of equal capability are given equal shares") {
        std::vector<node_capability> cluster(4, measured("node", 4, 4 * 0.0004));
        const auto shares = balanced_shares(cluster, 1920, GEN, MIN_ISLAND);

        for (const auto share : shares) {
            REQUIRE(share == Catch::Approx(static_cast<double>(shares.front())).epsilon(0.05));
        }
    }

    SECTION("a lone node takes everything") {
        const auto shares = balanced_shares({fast}, 960, GEN, MIN_ISLAND);
        REQUIRE(shares.size() == 1);
        REQUIRE(shares.at(0) == 960);
    }
}
