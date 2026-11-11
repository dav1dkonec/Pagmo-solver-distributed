#include "so_benchmark.h"

#include "benchmark_runner.h"
#include "benchmark_stats.h"


size_t get_so_algorithm_count()
{
    return construct_so_algorithms(100).size();
}

std::vector<pagmo::algorithm> construct_so_algorithms(const size_t genCount)
{
    std::vector<pagmo::algorithm> out;
    out.emplace_back(pagmo::gaco(genCount));
    out.emplace_back(pagmo::de(genCount));
    out.emplace_back(pagmo::sade(genCount));
    out.emplace_back(pagmo::de1220(genCount));
    out.emplace_back(pagmo::gwo(genCount));
    out.emplace_back(pagmo::pso(genCount));
    out.emplace_back(pagmo::bee_colony(genCount));
    out.emplace_back(pagmo::simulated_annealing()); // Not population based

    return out;
}

void benchmark_compare_so(distributed_solver& ds, const pagmo::problem& prob, benchmark_stats& bench,
                          const size_t popSize, const size_t genCount)
{
    const std::vector<pagmo::algorithm> soAlgs = construct_so_algorithms(genCount);

    std::cout << "SO benchmark waiting for " << ds.get_expected_worker_count() << " workers to connect..." << std::endl;
    ds.wait_until_workers_connect();

    // Run metasolver and then run each SO algorithm alone
    benchmark_compare_runner(ds, prob, bench, popSize, genCount, soAlgs);

    // End the current measurement
    bench.end_current_measurement();
}

void run_so_benchmark(distributed_solver& ds, benchmark_stats& bench, const bool outputHtml, const bool outputCsv,
                      const size_t popSize, const size_t genCount)
{
    std::cout << std::endl << "Running single objective benchmark..." << std::endl;

    std::cout << std::endl << "Running pagmo::schwefel..." << std::endl;
    {
        const pagmo::problem prob{pagmo::schwefel(3)};
        bench.set_problem(prob);
        benchmark_compare_so(ds, prob, bench, popSize, genCount);
    }

    std::cout << std::endl << "Running pagmo::rosenbrock..." << std::endl;
    {
        pagmo::problem prob{pagmo::rosenbrock(40)};
        bench.set_problem(prob);
        benchmark_compare_so(ds, prob, bench, popSize, genCount);
    }

    std::cout << std::endl << "Running pagmo::rastrigin..." << std::endl;
    {
        pagmo::problem prob{pagmo::rastrigin(40)};
        bench.set_problem(prob);
        benchmark_compare_so(ds, prob, bench, popSize, genCount);
    }

    std::cout << std::endl << "Running pagmo::ackley..." << std::endl;
    {
        pagmo::problem prob{pagmo::ackley(40)};
        bench.set_problem(prob);
        benchmark_compare_so(ds, prob, bench, popSize, genCount);
    }

    std::cout << std::endl << "Running pagmo::griewank..." << std::endl;
    {
        pagmo::problem prob{pagmo::griewank(40)};
        bench.set_problem(prob);
        benchmark_compare_so(ds, prob, bench, popSize, genCount);
    }

    // Save benchmark results into the fs
    if (outputCsv)
        bench.save_all_results_as_csv();
    if (outputHtml)
        bench.save_all_results_as_html();
}
