#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <numeric>
#include "work_plan.h"

namespace
{
    constexpr size_t MIN_ISLAND = 80;
    // One generation, so that the work rates below read as individuals per millisecond
    constexpr size_t GEN = 1;

    node_capability measured(const std::string& id, const size_t cores, const double perIsland)
    {
        return {id, cores, perIsland, true};
    }

    node_capability unmeasured(const std::string& id, const size_t cores)
    {
        return {id, cores, 1.0, false};
    }

    // A wide node and a narrow one that is also slower per core, which is the case the planner
    // has to get right. Both figures come from a run of the real problem, per generation.
    const node_capability fast = measured("fast", 8, 0.00085);
    const node_capability slow = measured("slow", 4, 0.00038);
}

TEST_CASE("Work plan - predicted time", "[work-plan]") {
    SECTION("more individuals can mean less time, because they open another island") {
        // One short of the second island they all queue behind one core; one more and two run
        REQUIRE(predicted_time(fast, 2 * MIN_ISLAND - 1, GEN, MIN_ISLAND)
                > predicted_time(fast, 2 * MIN_ISLAND, GEN, MIN_ISLAND));
    }

    SECTION("no more islands than cores") {
        // Both shares run on eight islands, so the larger one takes proportionally longer
        const double eight = predicted_time(fast, 8 * MIN_ISLAND, GEN, MIN_ISLAND);
        REQUIRE(predicted_time(fast, 16 * MIN_ISLAND, GEN, MIN_ISLAND) == 2 * eight);
    }

    SECTION("twice the generations is twice the time") {
        REQUIRE(predicted_time(fast, 8 * MIN_ISLAND, 2 * GEN, MIN_ISLAND)
                == 2 * predicted_time(fast, 8 * MIN_ISLAND, GEN, MIN_ISLAND));
    }

    SECTION("a node with no measurement yet reports no time") {
        const node_capability idle{"idle", 4, 0, false};
        REQUIRE(predicted_time(idle, 320, GEN, MIN_ISLAND) == 0);
    }
}

TEST_CASE("Work plan - capacity within a deadline", "[work-plan]") {
    SECTION("a deadline too short for one island is worth nothing") {
        const double tooShort = predicted_time(slow, MIN_ISLAND, GEN, MIN_ISLAND) - 1;
        REQUIRE(capacity_within(slow, tooShort, GEN, MIN_ISLAND) == 0);
    }

    SECTION("right below a step the node is worth nothing at all") {
        // The deadline reaches 300 individuals across four islands, but four of them need 320,
        // and three, two or one of them cannot carry even their own minimum by then either
        const double deadline = 300.0 / (4 * 0.00038);
        REQUIRE(capacity_within(slow, deadline, GEN, MIN_ISLAND) == 0);
    }

    SECTION("opening another island needs individuals to fill it") {
        // Right below the step, the extra core cannot be used and the capacity stays behind it
        const double justUnder = (4 * MIN_ISLAND - 1) / (4 * 0.00038);
        REQUIRE(capacity_within(slow, justUnder, GEN, MIN_ISLAND) < 4 * MIN_ISLAND);

        const double justOver = (4 * MIN_ISLAND) / (4 * 0.00038);
        REQUIRE(capacity_within(slow, justOver, GEN, MIN_ISLAND) >= 4 * MIN_ISLAND);
    }

    SECTION("capacity is not capped once every core carries an island") {
        const double deadline = 10 * MIN_ISLAND / (4 * 0.00038);
        REQUIRE(capacity_within(slow, deadline, GEN, MIN_ISLAND) > 4 * MIN_ISLAND);
    }
}

TEST_CASE("Work plan - generations that fit a deadline", "[work-plan]") {
    SECTION("it is the inverse of the predicted time") {
        const size_t share = 8 * MIN_ISLAND;
        const double deadline = predicted_time(fast, share, 50, MIN_ISLAND);

        // Floored to whole generations, so the last one may not fit
        const size_t generations = generations_within(fast, share, deadline, MIN_ISLAND);
        REQUIRE(generations >= 49);
        REQUIRE(generations <= 50);
    }

    SECTION("twice the deadline is twice the depth") {
        const size_t share = 4 * MIN_ISLAND;
        const double deadline = predicted_time(slow, share, 50, MIN_ISLAND);

        const auto once = generations_within(slow, share, deadline, MIN_ISLAND);
        const auto twice = generations_within(slow, share, 2 * deadline, MIN_ISLAND);
        REQUIRE(static_cast<double>(twice) == Catch::Approx(2.0 * static_cast<double>(once)).epsilon(0.05));
    }

    SECTION("a deadline too short for one generation is worth nothing") {
        const double tooShort = predicted_time(slow, MIN_ISLAND, GEN, MIN_ISLAND) / 2;
        REQUIRE(generations_within(slow, MIN_ISLAND, tooShort, MIN_ISLAND) == 0);
    }

    SECTION("a node with no share runs nothing") {
        REQUIRE(generations_within(fast, 0, 1000.0, MIN_ISLAND) == 0);
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
        std::vector<node_capability> cluster(4, measured("node", 4, 0.0004));
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
        // The slow node needs 210 s for the one island it cannot go below, while the fast node
        // runs the whole population in 141 s
        const auto shares = balanced_shares({fast, slow}, 960, GEN, MIN_ISLAND);

        REQUIRE(shares.at(0) == 960);
        REQUIRE(shares.at(1) == 0);
    }

    SECTION("both nodes are used once the population is large enough") {
        const auto shares = balanced_shares({fast, slow}, 1920, GEN, MIN_ISLAND);

        REQUIRE(shares.at(1) >= 4 * MIN_ISLAND);
        REQUIRE(predicted_time(fast, shares.at(0), GEN, MIN_ISLAND)
                == Catch::Approx(predicted_time(slow, shares.at(1), GEN, MIN_ISLAND)).epsilon(0.05));
    }

    SECTION("a share reaches the step instead of stopping below it") {
        // Dividing 1920 in proportion to power gave the slow node 318, which fills three of its
        // four islands and leaves a core idle; it has to reach 320 for the fourth one to open
        const auto shares = balanced_shares({fast, slow}, 1920, GEN, MIN_ISLAND);
        REQUIRE(shares.at(1) / MIN_ISLAND == 4);
    }

    SECTION("an unmeasured node is risked with one island per core and no more") {
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
        std::vector<node_capability> cluster(4, measured("node", 4, 0.0004));
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
