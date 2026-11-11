#include "benchmark_runner.h"

void benchmark_compare_runner(distributed_solver& ds, const pagmo::problem& prob, benchmark_stats& bench,
    const size_t popSize, const size_t genCount, const std::vector<pagmo::algorithm>& algs)
{
    // First run the metasolver
    bench.start_timer();
    ds.evolve(prob, algs, popSize, genCount, 10);
    ds.wait_until_completion();
    bench.add_data_point(
        ds.get_best_individual(),
        "META_ALL",
        bench.stop_timer()
    );

    // Run the benchmark for each algorithm alone
    for (int i = 0; i < algs.size(); ++i)
    {
        const auto& currentAlg = algs[i];

        std::cout << i + 1 << "/" << algs.size() << " (" << currentAlg.get_name() + ")" << std::endl;

        // Using only ONE algorithm
        bench.start_timer();
        ds.evolve(prob, {currentAlg}, popSize, genCount, 10);
        ds.wait_until_completion();
        bench.add_data_point(
            ds.get_best_individual(),
            "SINGLE_" + currentAlg.get_name(),
            bench.stop_timer()
        );
    }
}
