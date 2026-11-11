#pragma once
#include <iostream>
#include <vector>

#include "distributed_solver.h"
#include "benchmark_stats.h"

void benchmark_compare_runner(distributed_solver& ds, const pagmo::problem& prob, benchmark_stats& bench,
                              size_t popSize, size_t genCount, const std::vector<pagmo::algorithm>& algs);
