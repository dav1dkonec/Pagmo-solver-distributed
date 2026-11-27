#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_set>
#include <vector>

// Outcome of one work unit: the last evaluation that still paid off.
struct stop_point
{
    size_t evaluations{};
    // Last e whose window improved by at least epsilon; 0 if none did
    size_t stopEvaluation{};
    uint64_t stopTimeMs{};
    // A full window without such improvement followed; the unit still ran all its generations
    bool converged{};
    // Measurement only: best fitness at e = 0, k, 2k, ...; empty unless a stride was given
    std::vector<double> bestCurve{};
};

/**
 * Finds the evaluation after which a unit no longer pays off: the last e at which the best fitness
 * improved by at least epsilon (relative) over the last W evaluations, f(e - W) - f(e) >= eps*f(e - W).
 * W is windowGenerations times the population size, f(0) the champion the unit started from and
 * f(e) = f(0) for e < 0.
 * Re-evaluating an individual the unit received is not search, so it is not counted at all.
 * Evaluations run in parallel, so record() is thread-safe.
 */
class stop_tracker
{
public:
    stop_tracker(double baseline, size_t window, double epsilon, const std::vector<std::vector<double>>& received = {},
                 size_t curveStride = 0);

    void record(const std::vector<double>& x, double fitness);
    stop_point result() const;

private:
    struct vector_hash
    {
        size_t operator()(const std::vector<double>& x) const noexcept;
    };

    // Read-only after construction, so lookups need no lock
    const std::unordered_set<std::vector<double>, vector_hash> _received;
    mutable std::mutex _guard{};
    const double _epsilon;
    const std::chrono::steady_clock::time_point _start{std::chrono::steady_clock::now()};
    // Best fitness after e evaluations, kept for the last W of them; slot e % W
    std::vector<double> _bestRing;
    double _best;
    size_t _evaluations{};
    size_t _lastSignificant{};
    uint64_t _lastSignificantMs{};
    const size_t _curveStride;
    std::vector<double> _bestCurve{};
};
