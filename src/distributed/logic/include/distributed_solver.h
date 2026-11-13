#pragma once
#include <functional>

#include "distributed_controller.h"
#include "pagmo/archipelago.hpp"

enum class load_balancing_strategy
{
    ALL_ISLANDS_EQUAL,
    BY_PERFORMANCE
};

/**
 * How a node too slow to keep the cluster's pace is held back. An island cannot go below its
 * minimum population, so the generation count is the only one of the two that can make a node's
 * unit cheaper than that floor.
 */
enum class heterogeneity_policy
{
    // Shares are sized so that every node runs the full generation count; a node that cannot fill
    // even its smallest island by the deadline is left out
    BY_POPULATION,
    // As above, except that such a node keeps its smallest island, which the leader gives up from
    // its own share, and runs fewer generations instead of being left out
    BY_BOTH,
    // Every node gets the same share and the generation count alone is fitted to the deadline
    BY_GENERATIONS
};

/**
 * Builds the algorithm portfolio for a generation count. It has to return the same algorithms in
 * the same order whatever the count, because islands are matched to the portfolio by index.
 */
using algorithm_factory = std::function<std::vector<pagmo::algorithm>(size_t generationCount)>;

/**
 * One island's worth of the plan. The algorithm is named by its index rather than held, because
 * the island's generation count decides which instance of it the island gets.
 */
struct island_assignment
{
    size_t populationSize;
    size_t cycleCount;
    size_t generationCount;
    std::string preferredWorkerId;
    size_t algorithmIndex;
};

class distributed_solver : public logger_control
{
    load_balancing_strategy _loadBalancingStrategy;

    heterogeneity_policy _heterogeneityPolicy;

    distributed_controller _controller;
    size_t _expectedWorkerCount;

    pagmo::archipelago _archipelago{};

    std::vector<pagmo::vector_double> _initialHints{};
    void _set_island_hints(pagmo::island& isl) const;

    /**
     * Decides how the population is divided for this evolution cycle: one island per node that
     * can shorten the cycle, each sized so that the nodes finish at about the same time.
     * @param populationSize The total population size
     * @param generationCount Generations every share is evolved for
     * @param algorithmNames The portfolio, in the order islands are handed indices into it
     * @param minIslandPopSize Minimal population size for one island
     * @return One assignment per island the plan admits
     */
    std::vector<island_assignment> _generate_work_plan(size_t populationSize, size_t generationCount,
                                                       const std::vector<std::string>& algorithmNames,
                                                       size_t minIslandPopSize);

public:
    /**
     * - Constructs the distributed solver and its internal components
     * - The distributed_controller's server is started immediately on another thread, as this object is constructed
     *  - This means that workers can connect as soon as this object is constructed
     * @param controllerAddress URL on which the distributed controller will run (e.g. "tcp://localhost:5555")
     * @param expectedWorkerCount the expected amount of workers, the current amount connected to controller will be used if not specified
     * @param loadBalancingStrategy determines how populations are calculated for each island
     * @param heterogeneityPolicy determines what a node too slow to keep the pace gives up
     */
    explicit distributed_solver(const std::string& controllerAddress, const size_t expectedWorkerCount = 1,
                                load_balancing_strategy loadBalancingStrategy =
                                    load_balancing_strategy::BY_PERFORMANCE,
                                heterogeneity_policy heterogeneityPolicy =
                                    heterogeneity_policy::BY_POPULATION);

    ~distributed_solver();;

    void evolve(const pagmo::problem& problem,
                const algorithm_factory& makeAlgorithms,
                size_t populationSize,
                size_t generationCount,
                size_t cycleCount = 1,
                size_t minIslandPopSize = 80
    );

    void set_initial_hints(const std::vector<pagmo::vector_double>& hints);

    pagmo::vector_double wait_until_completion();

    pagmo::vector_double get_best_individual();

    pagmo::evolve_status get_status() const;

    std::vector<pagmo::vector_double> get_best_N_individuals(size_t N);

    void wait_until_workers_connect(size_t workerCount);

    void wait_until_workers_connect();

    // Same, but gives up after timeout. Returns whether all expected workers connected.
    bool wait_until_workers_connect(size_t workerCount, std::chrono::milliseconds timeout);

    bool wait_until_workers_connect(std::chrono::milliseconds timeout);

    size_t get_current_worker_count();

    size_t get_expected_worker_count() const;

protected:
    std::string get_default_log_name() const override
    {
        return "./logs/solver";
    }
};
