#pragma once
#include "distributed_controller.h"
#include "pagmo/archipelago.hpp"

enum class load_balancing_strategy
{
    ALL_ISLANDS_EQUAL,
    BY_PERFORMANCE
};

class distributed_solver : public logger_control
{
    load_balancing_strategy _loadBalancingStrategy;

    distributed_controller _controller;
    size_t _expectedWorkerCount;

    pagmo::archipelago _archipelago{};

    std::vector<pagmo::vector_double> _initialHints{};
    void _set_island_hints(pagmo::island& isl) const;

    /**
     * Generates a list of input parameters for each island
     * @param islandCount The number of islands we want to use
     * @param populationSize The total population size
     * @param algorithms Algorithms to be used by islands
     * @param minIslandPopSize Minimal population size for one island
     * @return Vector of tuples: [populationSize, cycleCount, preferredWorkerId]
     */
    std::vector<std::tuple<size_t, size_t, std::string, const pagmo::algorithm&>> _generate_work_plan(
        const size_t islandCount, size_t populationSize, const std::vector<pagmo::algorithm>& algorithms, size_t
        minIslandPopSize);

public:
    /**
     * - Constructs the distributed solver and its internal components
     * - The distributed_controller's server is started immediately on another thread, as this object is constructed
     *  - This means that workers can connect as soon as this object is constructed
     * @param controllerAddress URL on which the distributed controller will run (e.g. "tcp://localhost:5000")
     * @param expectedWorkerCount the expected amount of workers, the current amount connected to controller will be used if not specified
     * @param loadBalancingStrategy determines how populations are calculated for each island
     */
    explicit distributed_solver(const std::string& controllerAddress, const size_t expectedWorkerCount = 1,
                                load_balancing_strategy loadBalancingStrategy =
                                    load_balancing_strategy::BY_PERFORMANCE);

    ~distributed_solver();;

    void evolve(const pagmo::problem& problem,
                const std::vector<pagmo::algorithm>& algorithms,
                size_t populationSize,
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
