#include "worker_info_repository.h"

void worker_info_repository::worker_joined(const std::string& workerID, worker_info info)
{
    {
        std::scoped_lock lock(_mtx);
        spdlog::info("Worker joined - info collected");

        if (!_workerRecords.contains(workerID))
        {
            _workerRecords.insert({workerID, info});
        }
        _connectedWorkers.insert(workerID);
    }
    _cv.notify_all();
}

void worker_info_repository::worker_left(const std::string& workerID)
{
    {
        std::scoped_lock lock(_mtx);
        spdlog::info("Worker left - info collected");

        _connectedWorkers.erase(workerID);
    }
    _cv.notify_all();
}

void worker_info_repository::worker_started_work(const std::string& workerID)
{
    std::scoped_lock lock(_mtx);
    spdlog::info("Worker started work - info collected");

    if (!_workerRecords.contains(workerID))
    {
        throw std::runtime_error(
            "Cannot perform worker_started_work(), worker " + workerID + " is not in _workerRecords.");
    }

    _workerRecords.at(workerID).lastWorkStartTime = std::chrono::high_resolution_clock::now();
}

void worker_info_repository::worker_finished_work(const std::string& workerID, const size_t processedPopulation,
                                                  const std::string& algoName, const uint64_t evolveTimeMs)
{
    std::scoped_lock lock(_mtx);
    spdlog::info("Worker finished work - info collected");

    if (!_workerRecords.contains(workerID))
    {
        throw std::runtime_error(
            "Cannot perform worker_finished_work(), worker " + workerID + " is not in _workerRecords.");
    }

    auto& wInfo = _workerRecords.at(workerID);
    const auto startTime = wInfo.lastWorkStartTime;
    const auto endTime = std::chrono::high_resolution_clock::now();
    const auto workTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    wInfo.totalStats.workTime += workTime;
    wInfo.totalStats.evolveTime += evolveTimeMs;
    wInfo.totalStats.processedPopulation += processedPopulation;

    wInfo.statsByAlgorithm[algoName].workTime += workTime;
    wInfo.statsByAlgorithm[algoName].evolveTime += evolveTimeMs;
    wInfo.statsByAlgorithm[algoName].processedPopulation += processedPopulation;

    spdlog::debug("{} round-trip {} ms, of which {} ms computing", workerID, workTime, evolveTimeMs);
}

std::optional<worker_info> worker_info_repository::get_worker_info(
    const std::string& workerID)
{
    std::scoped_lock lock(_mtx);
    spdlog::trace("Worker info requested");

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
    spdlog::info("Waiting until {} workers connect...", target);

    _cv.wait(lock, [&]
    {
        return _connectedWorkers.size() >= target;
    });
}
