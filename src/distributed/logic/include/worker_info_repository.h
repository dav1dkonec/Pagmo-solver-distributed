#pragma once
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <pagmo/s11n.hpp>
#include <boost/serialization/unordered_map.hpp>
#include <boost/serialization/version.hpp>
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
    // Individuals the worker really evolved, as reported by the worker
    uint64_t processedPopulation = 0;

    // Individuals per millisecond of computation on one island, aged so that recent units
    // dominate. Per island rather than per node, so that units run at different island counts
    // stay comparable; multiply by the island count to get the throughput of the whole node.
    double islandThroughput = 0;
    uint64_t samples = 0;

    // Folds one finished unit into the cumulative totals and into the throughput average
    void add_sample(uint64_t workTimeMs, uint64_t evolveTimeMs, uint64_t population, uint64_t islandCount);

private:
    //####################################
    //# BOOST SERIALIZE
    //####################################

    friend class boost::serialization::access;

    template <typename Archive>
    void serialize(Archive& ar, const unsigned version)
    {
        ar & BOOST_SERIALIZATION_NVP(workTime);
        ar & BOOST_SERIALIZATION_NVP(evolveTime);
        ar & BOOST_SERIALIZATION_NVP(processedPopulation);

        if (version == 1)
        {
            // Version 1 measured the node as a whole, which is not comparable with a per-island
            // figure. Read it to keep the archive aligned and let the next unit re-learn.
            double legacyThroughput = 0;
            uint64_t legacySamples = 0;
            ar & boost::serialization::make_nvp("throughput", legacyThroughput);
            ar & boost::serialization::make_nvp("samples", legacySamples);
        }
        else if (version >= 2)
        {
            ar & BOOST_SERIALIZATION_NVP(islandThroughput);
            ar & BOOST_SERIALIZATION_NVP(samples);
        }
    }
};

BOOST_CLASS_VERSION(work_stats, 2)

struct worker_info
{
    work_stats totalStats{};
    std::unordered_map<std::string, work_stats> statsByAlgorithm{};

    // Islands the worker can run in parallel, as it reported on connecting. Zero until it does.
    size_t coreCount{};

    // Place to save a timestamp when worker_started_work() is called
    std::chrono::high_resolution_clock::time_point lastWorkStartTime;

private:
    //####################################
    //# BOOST SERIALIZE
    //####################################

    friend class boost::serialization::access;

    template <typename Archive>
    void serialize(Archive& ar, const unsigned version)
    {
        ar & BOOST_SERIALIZATION_NVP(totalStats);
        ar & BOOST_SERIALIZATION_NVP(statsByAlgorithm);

        if (version >= 1)
        {
            ar & BOOST_SERIALIZATION_NVP(coreCount);
        }
    }
};

BOOST_CLASS_VERSION(worker_info, 1)

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
                              uint64_t evolveTimeMs, size_t islandCount);

    /**
     * Records what the worker itself knows about its hardware, see MsgType::WORKER_INFO. This is
     * also what admits it to the connected set, because a plan built without it would misjudge
     * the node; a worker reconnecting with a stored record is admitted by worker_joined().
     */
    void worker_reported_cores(const std::string& workerID, size_t coreCount);

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
