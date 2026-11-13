#include "benchmark_runner.h"

void benchmark_compare_runner(distributed_solver& ds, const pagmo::problem& prob, benchmark_stats& bench,
    const size_t popSize, const size_t genCount, const algorithm_factory& makeAlgorithms)
{
    // First run the metasolver
    bench.start_timer();
    ds.evolve(prob, makeAlgorithms, popSize, genCount, 10);
    ds.wait_until_completion();
    bench.add_data_point(
        ds.get_best_individual(),
        "META_ALL",
        bench.stop_timer()
    );

    // Run the benchmark for each algorithm alone
    const auto algs = makeAlgorithms(genCount);
    for (size_t i = 0; i < algs.size(); ++i)
    {
        const auto& currentAlg = algs[i];

        std::cout << i + 1 << "/" << algs.size() << " (" << currentAlg.get_name() + ")" << std::endl;

        // Narrowed to one algorithm, still as a factory so the solver can ask for any count
        const algorithm_factory onlyThisOne = [&makeAlgorithms, i](const size_t gen)
        {
            return std::vector{makeAlgorithms(gen).at(i)};
        };

        bench.start_timer();
        ds.evolve(prob, onlyThisOne, popSize, genCount, 10);
        ds.wait_until_completion();
        bench.add_data_point(
            ds.get_best_individual(),
            "SINGLE_" + currentAlg.get_name(),
            bench.stop_timer()
        );
    }
}
