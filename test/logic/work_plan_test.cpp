#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <numeric>
#include "work_plan.h"

namespace
{
    constexpr size_t MIN_ISLAND = 80;

    node_capability measured(const std::string& id, const size_t cores, const double perIsland)
    {
        return {id, cores, perIsland, perIsland * static_cast<double>(cores), true};
    }

    node_capability unmeasured(const std::string& id, const size_t cores)
    {
        return {id, cores, 1.0, static_cast<double>(cores), false};
    }

    // A wide node and a narrow one that is also slower per core, which is the case the planner
    // has to get right. Both figures come from a run of the real problem.
    const node_capability fast = measured("fast", 8, 0.00085);
    const node_capability slow = measured("slow", 4, 0.00038);
}

TEST_CASE("Work plan - predicted time", "[work-plan]") {
    SECTION("more individuals can mean less time, because they open another island") {
        // One short of the second island they all queue behind one core; one more and two run
        REQUIRE(predicted_time(fast, 2 * MIN_ISLAND - 1, MIN_ISLAND)
                > predicted_time(fast, 2 * MIN_ISLAND, MIN_ISLAND));
    }

    SECTION("no more islands than cores") {
        // Both shares run on eight islands, so the larger one takes proportionally longer
        const double eight = predicted_time(fast, 8 * MIN_ISLAND, MIN_ISLAND);
        REQUIRE(predicted_time(fast, 16 * MIN_ISLAND, MIN_ISLAND) == 2 * eight);
    }

    SECTION("a node with no measurement yet reports no time") {
        const node_capability idle{"idle", 4, 0, 0, false};
        REQUIRE(predicted_time(idle, 320, MIN_ISLAND) == 0);
    }
}

TEST_CASE("Work plan - capacity within a deadline", "[work-plan]") {
    SECTION("a deadline too short for one island is worth nothing") {
        const double tooShort = predicted_time(slow, MIN_ISLAND, MIN_ISLAND) - 1;
        REQUIRE(capacity_within(slow, tooShort, MIN_ISLAND) == 0);
    }

    SECTION("right below a step the node is worth nothing at all") {
        // The deadline reaches 300 individuals across four islands, but four of them need 320,
        // and three, two or one of them cannot carry even their own minimum by then either
        const double deadline = 300.0 / (4 * 0.00038);
        REQUIRE(capacity_within(slow, deadline, MIN_ISLAND) == 0);
    }

    SECTION("opening another island needs individuals to fill it") {
        // Right below the step, the extra core cannot be used and the capacity stays behind it
        const double justUnder = (4 * MIN_ISLAND - 1) / (4 * 0.00038);
        REQUIRE(capacity_within(slow, justUnder, MIN_ISLAND) < 4 * MIN_ISLAND);

        const double justOver = (4 * MIN_ISLAND) / (4 * 0.00038);
        REQUIRE(capacity_within(slow, justOver, MIN_ISLAND) >= 4 * MIN_ISLAND);
    }

    SECTION("capacity is not capped once every core carries an island") {
        const double deadline = 10 * MIN_ISLAND / (4 * 0.00038);
        REQUIRE(capacity_within(slow, deadline, MIN_ISLAND) > 4 * MIN_ISLAND);
    }
}

TEST_CASE("Work plan - balanced shares", "[work-plan]") {
    SECTION("the shares add up to the whole population") {
        for (const size_t population : {240, 960, 1920, 5000}) {
            const auto shares = balanced_shares({fast, slow}, population, MIN_ISLAND);
            REQUIRE(std::accumulate(shares.begin(), shares.end(), size_t{0}) == population);
        }
    }

    SECTION("a node that cannot shorten the cycle is left with nothing") {
        // The slow node needs 210 s for the one island it cannot go below, while the fast node
        // runs the whole population in 141 s
        const auto shares = balanced_shares({fast, slow}, 960, MIN_ISLAND);

        REQUIRE(shares.at(0) == 960);
        REQUIRE(shares.at(1) == 0);
    }

    SECTION("both nodes are used once the population is large enough") {
        const auto shares = balanced_shares({fast, slow}, 1920, MIN_ISLAND);

        REQUIRE(shares.at(1) >= 4 * MIN_ISLAND);
        REQUIRE(predicted_time(fast, shares.at(0), MIN_ISLAND)
                == Catch::Approx(predicted_time(slow, shares.at(1), MIN_ISLAND)).epsilon(0.05));
    }

    SECTION("a share reaches the step instead of stopping below it") {
        // Dividing 1920 in proportion to power gave the slow node 318, which fills three of its
        // four islands and leaves a core idle; it has to reach 320 for the fourth one to open
        const auto shares = balanced_shares({fast, slow}, 1920, MIN_ISLAND);
        REQUIRE(shares.at(1) / MIN_ISLAND == 4);
    }

    SECTION("an unmeasured node is risked with one island per core and no more") {
        const auto shares = balanced_shares({unmeasured("fast", 8), unmeasured("slow", 4)}, 1920, MIN_ISLAND);

        REQUIRE(shares.at(1) == 4 * MIN_ISLAND);
        REQUIRE(shares.at(0) == 1920 - 4 * MIN_ISLAND);
    }

    SECTION("a measured node is held to no such ceiling") {
        const auto shares = balanced_shares({fast, slow}, 5000, MIN_ISLAND);
        REQUIRE(shares.at(1) > 4 * MIN_ISLAND);
    }

    SECTION("the fastest node is served first when the cluster outnumbers the population") {
        // Five narrow nodes could take 5 x 320 between them, more than the population, and the
        // leader must not be the one left out
        std::vector<node_capability> cluster{unmeasured("lead", 4)};
        for (int i = 0; i < 4; ++i) {
            cluster.push_back(unmeasured("node" + std::to_string(i), 4));
        }

        const auto shares = balanced_shares(cluster, 960, MIN_ISLAND);
        REQUIRE(shares.at(0) >= MIN_ISLAND);
        REQUIRE(std::accumulate(shares.begin(), shares.end(), size_t{0}) == 960);
    }

    SECTION("nodes of equal capability are given equal shares") {
        std::vector<node_capability> cluster(4, measured("node", 4, 0.0004));
        const auto shares = balanced_shares(cluster, 1920, MIN_ISLAND);

        for (const auto share : shares) {
            REQUIRE(share == Catch::Approx(static_cast<double>(shares.front())).epsilon(0.05));
        }
    }

    SECTION("a lone node takes everything") {
        const auto shares = balanced_shares({fast}, 960, MIN_ISLAND);
        REQUIRE(shares.size() == 1);
        REQUIRE(shares.at(0) == 960);
    }
}
