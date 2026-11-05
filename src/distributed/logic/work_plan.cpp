#include "work_plan.h"

#include <algorithm>
#include <limits>

namespace
{
    // A node that has not shown what it can do is risked with no more than one island per core:
    // enough to measure it on real work, and not so much that a wrong guess decides the cycle.
    size_t share_ceiling(const node_capability& node, const size_t minIslandPopSize)
    {
        return node.measured ? std::numeric_limits<size_t>::max() : node.coreCount * minIslandPopSize;
    }
}

double throughput_with_all_cores(const node_capability& node)
{
    return node.islandThroughput * static_cast<double>(std::max<size_t>(1, node.coreCount));
}

double predicted_time(const node_capability& node, const size_t share, const size_t minIslandPopSize)
{
    const size_t islands = std::clamp<size_t>(share / minIslandPopSize, 1, std::max<size_t>(1, node.coreCount));
    const double nodeThroughput = node.islandThroughput * static_cast<double>(islands);

    return nodeThroughput > 0 ? static_cast<double>(share) / nodeThroughput : 0;
}

size_t capacity_within(const node_capability& node, const double deadlineMs, const size_t minIslandPopSize)
{
    size_t capacity = 0;
    for (size_t islands = 1; islands <= node.coreCount; ++islands)
    {
        const auto reachable = static_cast<size_t>(deadlineMs * node.islandThroughput * static_cast<double>(islands));

        // Any fewer and one of the islands would be under the minimum size
        if (reachable < islands * minIslandPopSize)
        {
            continue;
        }

        // Any more and the node opens another island, which the next step of the loop covers
        const size_t stepLimit = islands < node.coreCount
                                     ? (islands + 1) * minIslandPopSize - 1
                                     : std::numeric_limits<size_t>::max();

        capacity = std::max(capacity, std::min(reachable, stepLimit));
    }

    return capacity;
}

std::vector<size_t> balanced_shares(const std::vector<node_capability>& nodes, const size_t populationSize,
                                    const size_t minIslandPopSize)
{
    if (nodes.empty())
    {
        return {};
    }

    // The leader can always take the whole population alone, so its time for that is the ceiling
    double feasible = predicted_time(nodes.front(), populationSize, minIslandPopSize) + 1;
    double tooShort = 0;

    const auto fits = [&](const double deadline)
    {
        // The leader is not held to a ceiling, because it carries whatever the others cannot
        size_t total = capacity_within(nodes.front(), deadline, minIslandPopSize);
        for (size_t i = 1; i < nodes.size(); ++i)
        {
            total += std::min(capacity_within(nodes.at(i), deadline, minIslandPopSize),
                              share_ceiling(nodes.at(i), minIslandPopSize));
        }

        return total >= populationSize;
    };

    // 30 halvings narrow the deadline down to well under a second, finer than the shares need
    for (int i = 0; i < 30; ++i)
    {
        const double deadline = (tooShort + feasible) / 2;
        if (fits(deadline))
        {
            feasible = deadline;
        }
        else
        {
            tooShort = deadline;
        }
    }

    // The leader takes its own capacity first. Where the cluster has more capacity than the
    // population needs, the node served last gets nothing - that must not be the fastest one.
    std::vector<size_t> shares(nodes.size(), 0);
    shares.front() = std::min(capacity_within(nodes.front(), feasible, minIslandPopSize), populationSize);
    size_t unassigned = populationSize - shares.front();

    for (size_t i = 1; i < nodes.size() && unassigned > 0; ++i)
    {
        const size_t capacity = std::min(capacity_within(nodes.at(i), feasible, minIslandPopSize),
                                         share_ceiling(nodes.at(i), minIslandPopSize));

        shares.at(i) = std::min(capacity, unassigned);
        unassigned -= shares.at(i);
    }

    // What the ceilings turned down goes back to the leader, the one node held to no ceiling
    shares.front() += unassigned;

    return shares;
}
