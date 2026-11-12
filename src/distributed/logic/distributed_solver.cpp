#include "distributed_solver.h"

#include "logger_init.h"
#include "distributed_island.h"
#include "number_utils.h"
#include "work_plan.h"
#include "population_tools.h"
#include "pagmo/topologies/fully_connected.hpp"
#include "pagmo/utils/multi_objective.hpp"

#include <algorithm>
#include <ranges>

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

namespace
{
    // Connected nodes by descending throughput, each already credited with the algorithm its
    // island will run.
    std::vector<node_capability> rank_nodes(worker_info_repository& repo,
                                            const std::vector<pagmo::algorithm>& algorithms)
    {
        const auto connectedWorkers = repo.get_connected_workers();

        // A node that has not run anything yet is credited with what the measured ones average per
        // island, so its cores still count. With nothing measured at all the figure cancels out of
        // the ratios and the split is decided by core count alone.
        double measuredSum = 0;
        size_t measuredCount = 0;
        for (const auto& workerId : connectedWorkers)
        {
            const auto info = repo.get_worker_info(workerId);
            if (info && info->totalStats.samples > 0)
            {
                measuredSum += info->totalStats.islandWorkRate;
                ++measuredCount;
            }
        }
        const double assumedWorkRate = measuredCount > 0 ? measuredSum / static_cast<double>(measuredCount) : 1.0;

        std::vector<node_capability> nodes;
        nodes.reserve(connectedWorkers.size());
        for (const auto& workerId : connectedWorkers)
        {
            const auto info = repo.get_worker_info(workerId);
            if (!info)
            {
                continue;
            }

            const size_t cores = std::max<size_t>(1, info->coreCount);
            const bool measured = info->totalStats.samples > 0;
            const double perIsland = measured ? info->totalStats.islandWorkRate : assumedWorkRate;

            nodes.push_back({workerId, cores, perIsland, measured});
        }

        std::ranges::sort(nodes, std::greater{}, [](const node_capability& node)
        {
            return work_rate_with_all_cores(node);
        });

        // Islands take their algorithm in order, so the fastest node runs the first one. Where a node
        // has already been measured on the algorithm it is about to run, that figure is the better one.
        for (size_t i = 0; i < nodes.size() && !algorithms.empty(); ++i)
        {
            const auto info = repo.get_worker_info(nodes.at(i).workerId);
            const auto byAlgo = info->statsByAlgorithm.find(algorithms.at(i % algorithms.size()).get_name());

            if (byAlgo != info->statsByAlgorithm.end() && byAlgo->second.samples > 0)
            {
                nodes.at(i).islandWorkRate = byAlgo->second.islandWorkRate;
                nodes.at(i).measured = true;
            }
        }

        return nodes;
    }
}

std::vector<std::tuple<size_t, size_t, std::string, const pagmo::algorithm&>> distributed_solver::_generate_work_plan(
    const size_t populationSize, const size_t generationCount, const std::vector<pagmo::algorithm>& algorithms,
    const size_t minIslandPopSize)
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

    const auto nodes = rank_nodes(_controller.get_worker_info_repository(), algorithms);

    // OPTION 1: no node to divide by, or the caller asked for an even split regardless of capability
    if (nodes.empty() || _loadBalancingStrategy == load_balancing_strategy::ALL_ISLANDS_EQUAL)
    {
        const size_t islandCount = std::max<size_t>(1, nodes.size());
        size_t islandPop = std::max(minIslandPopSize, populationSize / islandCount);
        // Make sure it's divisible by 4 (some algorithms require this)
        islandPop = ceil_to_multiple(islandPop, 4);

        for (size_t i = 0; i < islandCount; ++i)
        {
            // Empty worker id, it doesn't matter because all workers get the same amount of work
            output.emplace_back(islandPop, 1, "", getAlgorithm());
        }

        return output;
    }

    // OPTION 2: one island per node that the deadline leaves with work to do, sized so that they
    // all finish at about the same time
    const auto shares = balanced_shares(nodes, populationSize, generationCount, minIslandPopSize);

    for (size_t i = 0; i < nodes.size(); ++i)
    {
        const auto& node = nodes.at(i);

        // The fastest node carries the cycle even when the population is smaller than one island
        const size_t share = i == 0 ? std::max(minIslandPopSize, shares.at(i)) : shares.at(i);
        if (share < minIslandPopSize)
        {
            dist_logger()->info("Leaving {} out of this cycle: {} cores at {:.5f} ind*gen/ms per island cannot "
                                "shorten it", node.workerId, node.coreCount, node.islandWorkRate);
            continue;
        }

        // Make sure it's divisible by 4 (some algorithms require this)
        const size_t islandPop = ceil_to_multiple(share, 4);

        dist_logger()->debug("{} has been assigned {} individuals: {} cores at {:.5f} ind*gen/ms per island, "
                             "predicted {:.0f} s", node.workerId, islandPop, node.coreCount, node.islandWorkRate,
                             predicted_time(node, islandPop, generationCount, minIslandPopSize) / 1000);

        output.emplace_back(islandPop, 1, node.workerId, getAlgorithm());
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
                                const size_t populationSize, const size_t generationCount, const size_t cycleCount,
                                const size_t minIslandPopSize)
{
    // This will prevent interrupting the archipelago if it's already evolving
    if (_archipelago.status() == pagmo::evolve_status::busy ||
        _archipelago.status() == pagmo::evolve_status::busy_error)
    {
        throw std::runtime_error("Cannot start a new evolution while the previous one is still running.");
    }

    // Archipelago using fully connected topology between the islands
    _archipelago = pagmo::archipelago{pagmo::fully_connected{}};

    // One island per node the plan admits, each sized so that they all finish at about the same time
    const auto workPlan = _generate_work_plan(populationSize, generationCount, algorithms, minIslandPopSize);

    for (const auto& [islandPopSize, islandCycleCount, preferredWorker, algorithm] : workPlan)
    {
        distributed_island dist_island{preferredWorker, islandCycleCount, generationCount};
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
