#include "distributed_solver.h"

#include "logger_init.h"
#include "distributed_island.h"
#include "number_utils.h"
#include "work_plan.h"
#include "population_tools.h"
#include "pagmo/topologies/fully_connected.hpp"
#include "pagmo/utils/multi_objective.hpp"

#include <algorithm>
#include <map>
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
                                            const std::vector<std::string>& algorithmNames)
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
        for (size_t i = 0; i < nodes.size() && !algorithmNames.empty(); ++i)
        {
            const auto info = repo.get_worker_info(nodes.at(i).workerId);
            const auto byAlgo = info->statsByAlgorithm.find(algorithmNames.at(i % algorithmNames.size()));

            if (byAlgo != info->statsByAlgorithm.end() && byAlgo->second.samples > 0)
            {
                nodes.at(i).islandWorkRate = byAlgo->second.islandWorkRate;
                nodes.at(i).measured = true;
            }
        }

        return nodes;
    }
}

std::vector<island_assignment> distributed_solver::_generate_work_plan(
    const size_t populationSize, const size_t generationCount, const std::vector<std::string>& algorithmNames,
    const size_t minIslandPopSize)
{
    std::vector<island_assignment> output{};

    // Helper lambda - "round-robin" algorithm chooser
    size_t nextIndex = 0;
    auto chooseAlgorithm = [&]() -> size_t
    {
        if (nextIndex == algorithmNames.size())
            nextIndex = 0;

        const size_t chosen = nextIndex++;
        dist_logger()->debug("Choosing algorithm: {}", algorithmNames.at(chosen));
        return chosen;
    };

    const auto nodes = rank_nodes(_controller.get_worker_info_repository(), algorithmNames);

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
            output.push_back({
                .populationSize = islandPop, .cycleCount = 1, .generationCount = generationCount,
                .preferredWorkerId = "", .algorithmIndex = chooseAlgorithm()
            });
        }

        return output;
    }

    // OPTION 2: one island per node that the deadline leaves with work to do, sized so that they
    // all finish at about the same time
    auto shares = _heterogeneityPolicy == heterogeneity_policy::BY_GENERATIONS
                      ? equal_shares(nodes, populationSize, minIslandPopSize)
                      : balanced_shares(nodes, populationSize, generationCount, minIslandPopSize);

    // The fastest node carries the cycle even when the population is smaller than one island
    shares.front() = std::max(minIslandPopSize, shares.front());

    // Only a node that is given a share it cannot finish in time may go less deep, so that the
    // other policies keep the full generation count exactly
    std::vector<bool> fitDepth(nodes.size(), _heterogeneityPolicy == heterogeneity_policy::BY_GENERATIONS);

    if (_heterogeneityPolicy == heterogeneity_policy::BY_BOTH)
    {
        // A node the deadline would leave out keeps its smallest island instead, and the leader
        // gives up the same number of individuals so that the shares still add up
        for (size_t i = 1; i < shares.size(); ++i)
        {
            if (shares.at(i) >= minIslandPopSize || shares.front() < 2 * minIslandPopSize)
            {
                continue;
            }

            shares.front() -= minIslandPopSize;
            shares.at(i) = minIslandPopSize;
            fitDepth.at(i) = true;
        }
    }

    // The leader sets the pace by carrying its own share at the full generation count, and the
    // others go as deep as that leaves them
    const size_t leaderPop = ceil_to_multiple(shares.front(), 4);
    const double deadlineMs = predicted_time(nodes.front(), leaderPop, generationCount, minIslandPopSize);

    for (size_t i = 0; i < nodes.size(); ++i)
    {
        const auto& node = nodes.at(i);

        if (shares.at(i) < minIslandPopSize)
        {
            dist_logger()->info("Leaving {} out of this cycle: {} cores at {:.5f} ind*gen/ms per island cannot "
                                "shorten it", node.workerId, node.coreCount, node.islandWorkRate);
            continue;
        }

        // Make sure it's divisible by 4 (some algorithms require this)
        const size_t islandPop = ceil_to_multiple(shares.at(i), 4);

        // The leader runs the full count by definition, since the deadline is its own time for it
        const size_t islandGenerations = i == 0 || !fitDepth.at(i)
                                             ? generationCount
                                             : std::min(generationCount,
                                                        generations_within(node, islandPop, deadlineMs,
                                                                           minIslandPopSize));
        if (islandGenerations == 0)
        {
            dist_logger()->info("Leaving {} out of this cycle: it cannot finish a single generation of {} "
                                "individuals in time", node.workerId, islandPop);
            continue;
        }

        dist_logger()->debug("{} has been assigned {} individuals over {} generations: {} cores at {:.5f} "
                             "ind*gen/ms per island, predicted {:.0f} s", node.workerId, islandPop,
                             islandGenerations, node.coreCount, node.islandWorkRate,
                             predicted_time(node, islandPop, islandGenerations, minIslandPopSize) / 1000);

        output.push_back({
            .populationSize = islandPop, .cycleCount = 1, .generationCount = islandGenerations,
            .preferredWorkerId = node.workerId, .algorithmIndex = chooseAlgorithm()
        });
    }

    return output;
}


distributed_solver::distributed_solver(const std::string& controllerAddress, const size_t expectedWorkerCount,
    const load_balancing_strategy loadBalancingStrategy, const heterogeneity_policy heterogeneityPolicy):
    _controller(controllerAddress), _expectedWorkerCount(expectedWorkerCount),
    _loadBalancingStrategy(loadBalancingStrategy), _heterogeneityPolicy(heterogeneityPolicy)
{
    _controller.run_server();
}

distributed_solver::~distributed_solver()
{
}

void distributed_solver::evolve(const pagmo::problem& problem, const algorithm_factory& makeAlgorithms,
                                const size_t populationSize, const size_t generationCount, const size_t cycleCount,
                                const size_t minIslandPopSize)
{
    // This will prevent interrupting the archipelago if it's already evolving
    if (_archipelago.status() == pagmo::evolve_status::busy ||
        _archipelago.status() == pagmo::evolve_status::busy_error)
    {
        throw std::runtime_error("Cannot start a new evolution while the previous one is still running.");
    }

    auto algorithms = makeAlgorithms(generationCount);
    if (algorithms.empty())
    {
        throw std::runtime_error("Cannot evolve, the algorithm factory returned an empty portfolio.");
    }

    std::vector<std::string> algorithmNames;
    algorithmNames.reserve(algorithms.size());
    for (const auto& algorithm : algorithms)
    {
        algorithmNames.push_back(algorithm.get_name());
    }

    // A generation count is fixed when an algorithm is constructed, so islands that go to
    // different depths need portfolios of their own
    std::map<size_t, std::vector<pagmo::algorithm>> portfolios;
    portfolios.emplace(generationCount, std::move(algorithms));

    const auto portfolio_for = [&](const size_t generations) -> const std::vector<pagmo::algorithm>&
    {
        const auto known = portfolios.find(generations);
        if (known != portfolios.end())
        {
            return known->second;
        }

        auto built = makeAlgorithms(generations);
        if (built.size() != algorithmNames.size())
        {
            throw std::runtime_error("The algorithm factory returned a portfolio of a different size for "
                "another generation count.");
        }

        for (size_t i = 0; i < built.size(); ++i)
        {
            if (built.at(i).get_name() != algorithmNames.at(i))
            {
                throw std::runtime_error("The algorithm factory reordered the portfolio for another generation"
                    " count: expected " + algorithmNames.at(i) + ", got " + built.at(i).get_name());
            }
        }

        return portfolios.emplace(generations, std::move(built)).first->second;
    };

    // Archipelago using fully connected topology between the islands
    _archipelago = pagmo::archipelago{pagmo::fully_connected{}};

    // One island per node the plan admits, each sized so that they all finish at about the same time
    const auto workPlan = _generate_work_plan(populationSize, generationCount, algorithmNames, minIslandPopSize);

    for (const auto& assignment : workPlan)
    {
        distributed_island dist_island{
            assignment.preferredWorkerId, assignment.cycleCount, assignment.generationCount
        };
        auto isl = pagmo::island{
            dist_island, portfolio_for(assignment.generationCount).at(assignment.algorithmIndex), problem,
            assignment.populationSize
        };
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
