#pragma once
#include <cstddef>
#include <string>
#include <vector>

/**
 * What the planner knows about one node: how fast it evolves its island with all its cores. The
 * rate is in individuals times generations per millisecond, because that product is the work a
 * unit carries - the same population over twice the generations is twice the computing.
 */
struct node_capability
{
    std::string workerId;
    size_t coreCount;
    double nodeWorkRate;
    // False while nodeWorkRate is a guess: a node that has not finished a unit yet is credited
    // with the average of the nodes that have, so that it can be planned for at all.
    bool measured;
};

// Milliseconds the node needs for this share at this generation count
double predicted_time(const node_capability& node, size_t share, size_t generationCount);

/**
 * The most individuals this node can evolve by the deadline, at this generation count. Fewer than
 * the minimum island size count as none, since such an island cannot be formed.
 */
size_t capacity_within(const node_capability& node, double deadlineMs, size_t generationCount,
                       size_t minIslandPopSize);

/**
 * Generations the node can evolve this share for by the deadline. The dual of capacity_within():
 * there the generation count is given and the individuals follow, here it is the other way round.
 */
size_t generations_within(const node_capability& node, size_t share, double deadlineMs);

/**
 * Shares that give every node the same number of individuals, with the remainder left to the
 * leader. Nodes the population cannot reach with a whole island each are left with nothing,
 * starting from the slowest.
 * @param nodes Ordered by descending nodeWorkRate; the first one leads
 * @param populationSize Individuals to divide, which the shares add up to exactly
 * @param minIslandPopSize Smallest population an island can hold
 * @return One share per node, in the order the nodes were given
 */
std::vector<size_t> equal_shares(const std::vector<node_capability>& nodes, size_t populationSize,
                                 size_t minIslandPopSize);

/**
 * Shares that have every node finish by one common deadline, as short a one as the nodes can keep
 * between them. A node too slow to fill a whole island by then is left with nothing, because it
 * could only become the makespan it was meant to reduce.
 * @param nodes Ordered by descending nodeWorkRate; the first one leads
 * @param populationSize Individuals to divide, which the shares add up to exactly
 * @param generationCount Generations every share is evolved for
 * @param minIslandPopSize Smallest population an island can hold
 * @return One share per node, in the order the nodes were given
 */
std::vector<size_t> balanced_shares(const std::vector<node_capability>& nodes, size_t populationSize,
                                    size_t generationCount, size_t minIslandPopSize);
