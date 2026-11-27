#include "stop_tracker.h"

#include <cmath>
#include <functional>
#include <stdexcept>

size_t stop_tracker::vector_hash::operator()(const std::vector<double>& x) const noexcept
{
    size_t seed = x.size();
    for (const double value : x)
    {
        seed ^= std::hash<double>{}(value) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    }
    return seed;
}

stop_tracker::stop_tracker(const double baseline, const size_t window, const double epsilon,
                           const std::vector<std::vector<double>>& received) :
    _received(received.begin(), received.end()),
    _epsilon(epsilon),
    _bestRing(window, baseline),
    _best(baseline)
{
    if (window == 0)
    {
        throw std::invalid_argument("stop_tracker needs a window of at least one evaluation");
    }
}

void stop_tracker::record(const std::vector<double>& x, const double fitness)
{
    if (_received.contains(x))
    {
        return;
    }

    std::scoped_lock lock{_guard};

    const size_t e = ++_evaluations;
    // An unevaluable candidate still costs an evaluation, but cannot be the best
    if (std::isfinite(fitness) && !(fitness >= _best))
    {
        _best = fitness;
    }

    // Before overwriting, the slot holds f(e - W); every slot starts with f(0)
    double& slot = _bestRing[e % _bestRing.size()];
    // A strict decrease first, so that a zero fitness does not count as improving forever
    if (_best < slot && slot - _best >= _epsilon * std::abs(slot))
    {
        _lastSignificant = e;
        _lastSignificantMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - _start).count());
    }
    slot = _best;
}

stop_point stop_tracker::result() const
{
    std::scoped_lock lock{_guard};
    return {_evaluations, _lastSignificant, _lastSignificantMs, _evaluations - _lastSignificant >= _bestRing.size()};
}
