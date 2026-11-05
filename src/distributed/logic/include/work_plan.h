#pragma once
#include <cstddef>
#include <string>
#include <vector>

/**
 * What the planner knows about one node: how fast one of its islands is, and how many of them
 * it can run at once.
 */
struct node_capability
{
    std::string workerId;
    size_t coreCount;
    double islandThroughput;
    // False while islandThroughput is an assumption rather than something the node has shown
    bool measured;
};

/**
 * Individuals per millisecond the node reaches with every core carrying an island, which is the
 * most it can do and therefore how nodes are ranked against each other.
 */
double throughput_with_all_cores(const node_capability& node);

/**
 * Milliseconds the node needs for this share. Not linear in the share: one too small to give
 * every core an island of the minimum size leaves part of the hardware idle.
 */
double predicted_time(const node_capability& node, size_t share, size_t minIslandPopSize);

/**
 * The most this node can evolve by the deadline. More islands put more cores to work, but an
 * island cannot go below the minimum size, so the capacity grows in steps of it rather than
 * smoothly - and right below a step the node is at its least useful.
 */
size_t capacity_within(const node_capability& node, double deadlineMs, size_t minIslandPopSize);

/**
 * Shares that have every node finish by one common deadline, as short a one as the nodes can keep
 * between them. A node too slow to fill a whole island by then is left with nothing, because it
 * could only become the makespan it was meant to reduce.
 * @param nodes Ordered by descending throughput_with_all_cores(); the first one leads
 * @param populationSize Individuals to divide, which the shares add up to exactly
 * @param minIslandPopSize Smallest population an island can hold
 * @return One share per node, in the order the nodes were given
 */
std::vector<size_t> balanced_shares(const std::vector<node_capability>& nodes, size_t populationSize,
                                    size_t minIslandPopSize);
