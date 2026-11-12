#include "worker_info_repository.h"

#include <algorithm>

#include "number_utils.h"

namespace
{
    // Weight of the newest sample; the previous average halves after about two more units.
    constexpr double Throughput_Smoothing = 0.3;
}

void work_stats::add_sample(const uint64_t workTimeMs, const uint64_t evolveTimeMs, const uint64_t population,
                            const uint64_t islandCount, const uint64_t generationCount)
{
    workTime += workTimeMs;
    evolveTime += evolveTimeMs;
    processedPopulation += population;

    // An unstated generation count counts as one, which degrades the rate to individuals per
    // millisecond. Multiplied in last, so that a reported population cannot overflow the product.
    const double sample = safe_divide(population, evolveTimeMs * std::max<uint64_t>(1, islandCount))
        * static_cast<double>(std::max<uint64_t>(1, generationCount));

    // The first sample has nothing to age against, so it seeds the average.
    islandWorkRate = samples == 0
                         ? sample
                         : Throughput_Smoothing * sample + (1 - Throughput_Smoothing) * islandWorkRate;
    ++samples;
}

void worker_info_repository::worker_joined(const std::string& workerID, worker_info info)
{
    {
        std::scoped_lock lock(_mtx);
        dist_logger()->info("Worker joined - info collected");

        if (!_workerRecords.contains(workerID))
        {
            _workerRecords.insert({workerID, info});
        }

        // A node counts as connected once we know what it can do, so that a plan is never built
        // on a capability report still in flight. A record kept from an earlier session has it.
        if (_workerRecords.at(workerID).coreCount > 0)
        {
            _connectedWorkers.insert(workerID);
        }
    }
    _cv.notify_all();
}

void worker_info_repository::worker_left(const std::string& workerID)
{
    {
        std::scoped_lock lock(_mtx);
        dist_logger()->info("Worker left - info collected");

        _connectedWorkers.erase(workerID);
    }
    _cv.notify_all();
}

void worker_info_repository::worker_started_work(const std::string& workerID)
{
    std::scoped_lock lock(_mtx);
    dist_logger()->info("Worker started work - info collected");

    if (!_workerRecords.contains(workerID))
    {
        throw std::runtime_error(
            "Cannot perform worker_started_work(), worker " + workerID + " is not in _workerRecords.");
    }

    _workerRecords.at(workerID).lastWorkStartTime = std::chrono::high_resolution_clock::now();
}

void worker_info_repository::worker_reported_cores(const std::string& workerID, const size_t coreCount)
{
    {
        std::scoped_lock lock(_mtx);

        // The report can beat the join that creates the record, so make one if there is none yet
        _workerRecords[workerID].coreCount = coreCount;
        _connectedWorkers.insert(workerID);

        dist_logger()->info("Worker {} reports {} cores", workerID, coreCount);
    }
    _cv.notify_all();
}

void worker_info_repository::worker_finished_work(const std::string& workerID, const size_t processedPopulation,
                                                  const std::string& algoName, const uint64_t evolveTimeMs,
                                                  const size_t islandCount, const size_t generationCount)
{
    std::scoped_lock lock(_mtx);
    dist_logger()->info("Worker finished work - info collected");

    if (!_workerRecords.contains(workerID))
    {
        throw std::runtime_error(
            "Cannot perform worker_finished_work(), worker " + workerID + " is not in _workerRecords.");
    }

    auto& wInfo = _workerRecords.at(workerID);
    const auto startTime = wInfo.lastWorkStartTime;
    const auto endTime = std::chrono::high_resolution_clock::now();
    const auto workTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    wInfo.totalStats.add_sample(workTime, evolveTimeMs, processedPopulation, islandCount, generationCount);
    wInfo.statsByAlgorithm[algoName].add_sample(workTime, evolveTimeMs, processedPopulation, islandCount,
                                                generationCount);

    dist_logger()->debug("{} round-trip {} ms, of which {} ms computing {} individuals over {} generations on {} "
                         "islands, work rate {:.5f} ind*gen/ms per island",
                         workerID, workTime, evolveTimeMs, processedPopulation, generationCount, islandCount,
                         wInfo.totalStats.islandWorkRate);
}

void worker_info_repository::clear()
{
    {
        std::scoped_lock lock(_mtx);
        _connectedWorkers.clear();
        _workerRecords.clear();
    }
    _cv.notify_all();
}

std::optional<worker_info> worker_info_repository::get_worker_info(
    const std::string& workerID)
{
    std::scoped_lock lock(_mtx);
    dist_logger()->trace("Worker info requested");

    const auto record = _workerRecords.find(workerID);
    if (record != _workerRecords.end())
    {
        return record->second;
    }

    // Record for this id was not found
    return std::nullopt;
}

const std::unordered_set<std::string>& worker_info_repository::get_connected_workers()
{
    std::scoped_lock lock(_mtx);

    return _connectedWorkers;
}

size_t worker_info_repository::get_worker_count()
{
    std::scoped_lock lock(_mtx);

    return _connectedWorkers.size();
}

void worker_info_repository::wait_until_worker_count(const size_t target)
{
    std::unique_lock lock(_mtx);
    dist_logger()->info("Waiting until {} workers connect...", target);

    _cv.wait(lock, [&]
    {
        return _connectedWorkers.size() >= target;
    });
}

bool worker_info_repository::wait_until_worker_count(const size_t target, const std::chrono::milliseconds timeout)
{
    std::unique_lock lock(_mtx);
    dist_logger()->info("Waiting up to {} ms until {} workers connect...", timeout.count(), target);

    return _cv.wait_for(lock, timeout, [&]
    {
        return _connectedWorkers.size() >= target;
    });
}
