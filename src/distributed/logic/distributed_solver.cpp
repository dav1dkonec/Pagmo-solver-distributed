#include "distributed_solver.h"

#include "logger_init.h"
#include "distributed_island.h"
#include "number_utils.h"
#include "population_tools.h"
#include "pagmo/topologies/fully_connected.hpp"
#include "pagmo/utils/multi_objective.hpp"

void distributed_solver::_set_island_hints(pagmo::island& isl) const
{
    if (!_initialHints.empty())
    {
        auto pop = isl.get_population();
        const size_t popSize = pop.size();
        for (size_t i = 0; i < std::min(_initialHints.size(), popSize); i++)
        {
            pop.set_x(i, _initialHints.at(i));
        }
        isl.set_population(pop);
    }
}

void distributed_solver::_calibrate_unmeasured_workers(const pagmo::problem& problem,
                                                       const std::vector<pagmo::algorithm>& algorithms,
                                                       const size_t populationSize)
{
    if (algorithms.empty())
    {
        return;
    }

    auto& repo = _controller.get_worker_info_repository();
    const std::unordered_set<std::string> connectedWorkers = repo.get_connected_workers();

    // A lone worker gets the whole population whatever its throughput is, and the unit it is about
    // to run measures it anyway, so probing first would only evolve a population and discard it
    if (connectedWorkers.size() < 2)
    {
        return;
    }

    std::vector<std::string> unmeasured;
    for (const auto& workerId : connectedWorkers)
    {
        const auto info = repo.get_worker_info(workerId);
        if (info && info->totalStats.samples == 0)
        {
            unmeasured.push_back(workerId);
        }
    }

    if (unmeasured.empty())
    {
        return;
    }

    // One population for all of them; throughput of unequal units is not comparable.
    const pagmo::population probe{problem, populationSize};

    std::vector<pagmo::island> probeIslands;
    probeIslands.reserve(unmeasured.size());
    for (const auto& workerId : unmeasured)
    {
        dist_logger()->debug("Calibrating {} with {} individuals", workerId, populationSize);
        probeIslands.emplace_back(distributed_island{workerId, 1}, algorithms.front(), probe);
    }

    // All at once: the islands evolve on their own threads, so this costs one unit of wall clock.
    for (auto& island : probeIslands)
    {
        island.evolve(1);
    }

    for (size_t i = 0; i < probeIslands.size(); ++i)
    {
        try
        {
            probeIslands.at(i).wait_check();
        }
        catch (const std::exception& e)
        {
            dist_logger()->warn("Calibration of {} failed: {}", unmeasured.at(i), e.what());
        }
    }
}


std::vector<std::tuple<size_t, size_t, std::string, const pagmo::algorithm&>> distributed_solver::_generate_work_plan(
    const size_t islandCount, const size_t populationSize, const std::vector<pagmo::algorithm>& algorithms, const size_t
    minIslandPopSize)
{
    std::vector<std::tuple<size_t, size_t, std::string, const pagmo::algorithm&>> output{};

    // Helper lambda - "round-robin" algorithm chooser
    auto algorithmPtr = algorithms.begin();
    auto getAlgorithm = [&]() -> const pagmo::algorithm&
    {
        if (algorithmPtr == algorithms.end())
            algorithmPtr = algorithms.begin();

        const auto& alg = *algorithmPtr++;
        dist_logger()->debug("Choosing algorithm: {}", alg.get_name());
        return alg;
    };

    // Prepare worker-related vars
    auto& repo = _controller.get_worker_info_repository();
    std::unordered_set<std::string> connectedWorkers = repo.get_connected_workers();
    const size_t workerCount = connectedWorkers.size();

    // OPTION 1: There are more islands than connected workers (they've not connected yet), so divide work equally
    if (islandCount > workerCount || _loadBalancingStrategy == load_balancing_strategy::ALL_ISLANDS_EQUAL)
    {
        size_t islandPop = std::max(minIslandPopSize, populationSize / islandCount);
        // Make sure it's divisible by 4 (some algorithms require this)
        islandPop = ceil_to_multiple(islandPop, 4);

        for (size_t i = 0; i < islandCount; ++i)
        {
            // Empty worker id, it doesn't matter because all workers get the same amount of work
            output.emplace_back(islandPop, 1, "", getAlgorithm());
        }
    }

    // OPTION 2: All expected workers have connected, we can divide work by their performance, there can be extra workers too
    else
    {
        // A vector of [workerId, algorithm, performanceAvgMetric], this is used to calculate island populations
        std::vector<std::tuple<std::string, const pagmo::algorithm&, double>> workersPreprocessed{};
        workersPreprocessed.reserve(islandCount);

        // 1) Assign workers to algorithms, picking the best-fit worker per island
        for (size_t i = 0; i < islandCount; ++i)
        {
            const auto& currentAlgorithm = getAlgorithm();
            const auto& algoName = currentAlgorithm.get_name();

            double bestTotalAvg = 0;
            std::string bestTotalWorkerId;
            double bestAlgoAvg = 0;
            std::string bestAlgoWorkerId;

            // Find the best worker in total together with the best worker for this algorithm
            for (const auto& workerId : connectedWorkers)
            {
                const auto& wInfo = repo.get_worker_info(workerId);

                // Computation time only; the round trip also holds the one-off blob transfer.
                const double wTotalAvg = wInfo->totalStats.throughput;

                double wAlgoAvg = 0;
                if (wInfo->statsByAlgorithm.contains(algoName))
                {
                    wAlgoAvg = wInfo->statsByAlgorithm.at(algoName).throughput;
                }

                if (wTotalAvg >= bestTotalAvg)
                {
                    bestTotalAvg = wTotalAvg;
                    bestTotalWorkerId = workerId;
                }

                if (wAlgoAvg >= bestAlgoAvg)
                {
                    bestAlgoAvg = wAlgoAvg;
                    bestAlgoWorkerId = workerId;
                }
            }

            // We prefer to choose by algorithm stats, we choose by total only if it's 2x better
            const bool preferTotal = (bestTotalAvg > 2 * bestAlgoAvg);
            const auto& chosenWorkerId = preferTotal ? bestTotalWorkerId : bestAlgoWorkerId;
            const double chosenAvg = preferTotal ? bestTotalAvg : bestAlgoAvg;

            workersPreprocessed.emplace_back(chosenWorkerId, currentAlgorithm, chosenAvg);
            // Remove the chosen worker from the set so it cannot be assigned to another island
            connectedWorkers.erase(chosenWorkerId);
        }

        // 2) Calculate the total performance of the worker cluster
        double totalPerformance = 0;
        for (const auto& [workerId, algorithm, perfMetric] : workersPreprocessed)
        {
            totalPerformance += perfMetric;
        }

        // 3) Calculate the percentage of performance committed by each worker, pop size is then derived from this
        for (const auto& [workerId, algorithm, perfMetric] : workersPreprocessed)
        {
            double workerPerfPercentage;
            if (totalPerformance > 0)
            {
                workerPerfPercentage = perfMetric / totalPerformance;
            }
            else
            {
                // If all metrics are zero fall back to an equal distribution to avoid division by zero
                workerPerfPercentage = 1.0 / static_cast<double>(workersPreprocessed.size());
            }

            // The population size of each worker is proportional to the percentage of performance it has
            const auto workerPopSize = static_cast<size_t>(static_cast<double>(populationSize) * workerPerfPercentage);
            auto finalWorkerPopSize = std::max(minIslandPopSize, workerPopSize);
            // Make sure it's divisible by 4 (some algorithms require this)
            finalWorkerPopSize = ceil_to_multiple(finalWorkerPopSize, 4);

            dist_logger()->debug("{} provides {}% of total worker cluster processing power", workerId, workerPerfPercentage * 100);
            dist_logger()->debug("{} has been assigned {} population size", workerId, finalWorkerPopSize);

            output.emplace_back(finalWorkerPopSize, 1, workerId, algorithm);
        }
    }

    return output;
}


distributed_solver::distributed_solver(const std::string& controllerAddress, const size_t expectedWorkerCount,
    const load_balancing_strategy loadBalancingStrategy):
    _controller(controllerAddress), _expectedWorkerCount(expectedWorkerCount), _loadBalancingStrategy(loadBalancingStrategy)
{
    _controller.run_server();
}

distributed_solver::~distributed_solver()
{
}

void distributed_solver::evolve(const pagmo::problem& problem, const std::vector<pagmo::algorithm>& algorithms,
                                const size_t populationSize, const size_t cycleCount, const size_t minIslandPopSize)
{
    // This will prevent interrupting the archipelago if it's already evolving
    if (_archipelago.status() == pagmo::evolve_status::busy ||
        _archipelago.status() == pagmo::evolve_status::busy_error)
    {
        throw std::runtime_error("Cannot start a new evolution while the previous one is still running.");
    }

    // Archipelago using fully connected topology between the islands
    _archipelago = pagmo::archipelago{pagmo::fully_connected{}};

    // Without a throughput sample per worker the plan below can only divide the work evenly
    _calibrate_unmeasured_workers(problem, algorithms, minIslandPopSize);

    // One island per worker that is still here once calibration is over. Asking for more is what
    // makes the plan fall back to an equal split that ignores how fast each node actually is.
    const size_t optimalIslandCount =
        std::max<size_t>(1, _controller.get_worker_info_repository().get_worker_count());

    // Work plan ensures correct worker load balancing
    const auto& workPlan = _generate_work_plan(optimalIslandCount, populationSize, algorithms, minIslandPopSize);

    for (int i = 0; i < optimalIslandCount; ++i)
    {
        const auto [islandPopSize,islandCycleCount,preferredWorker, algorithm] = workPlan.at(i);

        distributed_island dist_island{preferredWorker, islandCycleCount};
        auto isl = pagmo::island{dist_island, algorithm, problem, islandPopSize};
        // We set the initial population to this island (hints) if there are any
        _set_island_hints(isl);
        _archipelago.push_back(isl);
    }

    // Start evolving
    _archipelago.evolve(cycleCount);
    // Clear the hints so they're not re-used next time
    _initialHints.clear();
}

void distributed_solver::set_initial_hints(const std::vector<pagmo::vector_double>& hints)
{
    _initialHints = hints;
}

pagmo::vector_double distributed_solver::wait_until_completion()
{
    if (_archipelago.status() == pagmo::evolve_status::idle ||
        _archipelago.status() == pagmo::evolve_status::idle_error)
    {
        throw std::runtime_error("Cannot wait until completion, no evolution is running at the moment.");
    }

    _archipelago.wait_check();

    dist_logger()->info("Main Archipelago: Evolution finished");
    pagmo::vector_double bestIndividual = get_best_individual();

    std::string bestStr = "[";
    for (const double n : bestIndividual)
    {
        bestStr += std::to_string(n);
        bestStr += ", ";
    }
    bestStr += "]";
    dist_logger()->debug("Best individual: {}", bestStr);

    return bestIndividual;
}

pagmo::vector_double distributed_solver::get_best_individual()
{
    const auto best = get_best_N_individuals(1);
    return (!best.empty()) ? best.at(0) : pagmo::vector_double{};
}

pagmo::evolve_status distributed_solver::get_status() const
{
    return _archipelago.status();
}


std::vector<pagmo::vector_double> distributed_solver::get_best_N_individuals(const size_t N)
{
    if (_archipelago.size() == 0)
    {
        return {};
    }

    auto [allPopulations, allFitnesses] = merge_populations(_archipelago);
    auto bestIndividuals = select_best_N_individuals(
        _archipelago.begin()->get_population().get_problem(),
        allPopulations,
        allFitnesses,
        N
    );

    return bestIndividuals;
}

void distributed_solver::wait_until_workers_connect(const size_t workerCount)
{
    _controller.get_worker_info_repository().wait_until_worker_count(workerCount);
}

void distributed_solver::wait_until_workers_connect()
{
    wait_until_workers_connect(_expectedWorkerCount);
}

bool distributed_solver::wait_until_workers_connect(const size_t workerCount,
                                                   const std::chrono::milliseconds timeout)
{
    return _controller.get_worker_info_repository().wait_until_worker_count(workerCount, timeout);
}

bool distributed_solver::wait_until_workers_connect(const std::chrono::milliseconds timeout)
{
    return wait_until_workers_connect(_expectedWorkerCount, timeout);
}

size_t distributed_solver::get_current_worker_count()
{
    return _controller.get_worker_info_repository().get_worker_count();
}

size_t distributed_solver::get_expected_worker_count() const
{
    return _expectedWorkerCount;
}
