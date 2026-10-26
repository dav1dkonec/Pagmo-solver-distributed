#pragma once
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <pagmo/s11n.hpp>
#include <boost/serialization/unordered_map.hpp>
#include "logger_init.h"

struct work_stats
{
    /**
     * Maximal uint64_t value is: 18446744073709551615.
     * We're using milliseconds, if we convert this value to years (divide by 3,154e+10),
     * the result is: 2149100000000000 years. Therefore it's safe to assume these variables
     * will never overflow.
    */

    // Total round-trip time measured by the controller: transfer + computation
    uint64_t workTime = 0;
    // Of which the worker reported as computation. workTime - evolveTime is the transfer cost.
    uint64_t evolveTime = 0;
    // Total number of population individuals processed by this worker
    uint64_t processedPopulation = 0;

private:
    //####################################
    //# BOOST SERIALIZE
    //####################################

    friend class boost::serialization::access;

    template <typename Archive>
    void serialize(Archive& ar, unsigned)
    {
        ar & BOOST_SERIALIZATION_NVP(workTime);
        ar & BOOST_SERIALIZATION_NVP(evolveTime);
        ar & BOOST_SERIALIZATION_NVP(processedPopulation);
    }
};

struct worker_info
{
    work_stats totalStats{};
    std::unordered_map<std::string, work_stats> statsByAlgorithm{};

    // Place to save a timestamp when worker_started_work() is called
    std::chrono::high_resolution_clock::time_point lastWorkStartTime;

private:
    //####################################
    //# BOOST SERIALIZE
    //####################################

    friend class boost::serialization::access;

    template <typename Archive>
    void serialize(Archive& ar, unsigned)
    {
        ar & BOOST_SERIALIZATION_NVP(totalStats);
        ar & BOOST_SERIALIZATION_NVP(statsByAlgorithm);
    }
};

class worker_info_repository
{
    // Protect members of this class
    std::mutex _mtx{};

    // Notifying when the number of connected workers changes
    std::condition_variable _cv;

    std::unordered_set<std::string> _connectedWorkers;

    std::unordered_map<std::string, worker_info> _workerRecords;

public:
    void worker_joined(const std::string& workerID, worker_info info = worker_info{});

    void worker_left(const std::string& workerID);

    void worker_started_work(const std::string& workerID);

    void worker_finished_work(const std::string& workerID, size_t processedPopulation, const std::string& algoName,
                              uint64_t evolveTimeMs);

    std::optional<worker_info> get_worker_info(const std::string& workerID);

    const std::unordered_set<std::string>& get_connected_workers();

    size_t get_worker_count();

    void wait_until_worker_count(size_t target);

    // Same, but gives up after timeout. Returns whether the target was reached.
    bool wait_until_worker_count(size_t target, std::chrono::milliseconds timeout);

private:
    //####################################
    //# BOOST SERIALIZE
    //####################################

    friend class boost::serialization::access;

    template <typename Archive>
    void serialize(Archive& ar, unsigned)
    {
        ar & BOOST_SERIALIZATION_NVP(_workerRecords);
    }
};
