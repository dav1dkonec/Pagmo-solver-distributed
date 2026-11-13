#include "mo_benchmark.h"

#include "benchmark_runner.h"
#include "benchmark_stats.h"

size_t get_mo_algorithm_count()
{
    return construct_mo_algorithms(100).size();
}

std::vector<pagmo::algorithm> construct_mo_algorithms(const size_t genCount)
{
    std::vector<pagmo::algorithm> out;
    out.emplace_back(pagmo::nsga2(genCount));
    out.emplace_back(pagmo::moead(genCount));
    out.emplace_back(pagmo::moead_gen(genCount));
    out.emplace_back(pagmo::maco(genCount));
    out.emplace_back(pagmo::nspso(genCount));
    out.emplace_back(pagmo::ihs(genCount));

    return out;
}

void benchmark_compare_mo(distributed_solver& ds, const pagmo::problem& prob, benchmark_stats& bench,
                          const size_t popSize, const size_t genCount)
{
    std::cout << "MO benchmark waiting for " << ds.get_expected_worker_count() << " workers to connect..." << std::endl;
    ds.wait_until_workers_connect();

    // Run metasolver and then each MO algorithm alone
    benchmark_compare_runner(ds, prob, bench, popSize, genCount, construct_mo_algorithms);

    bench.end_current_measurement();
}

void run_mo_benchmark(distributed_solver& ds, benchmark_stats& bench, const bool outputHtml,
                      const bool outputCsv, const size_t popSize, const size_t genCount)
{
    std::cout << "\nRunning multi-objective benchmark...\n";

    // ZDT problem suite
    for (unsigned id = 1; id <= 4; ++id)
    {
        std::cout << "\nRunning pagmo::zdt(" << id << ")...\n";
        pagmo::problem prob{pagmo::zdt(id, 30)};
        bench.set_problem(prob);
        benchmark_compare_mo(ds, prob, bench, popSize, genCount);
    }

    // Save benchmark results into the fs
    if (outputCsv)
        bench.save_all_results_as_csv();
    if (outputHtml)
        bench.save_all_results_as_html();
}
