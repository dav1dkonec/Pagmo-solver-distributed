#pragma once
#include <boost/archive/archive_exception.hpp>
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

    // Individuals times generations per millisecond of computation on one island, aged so that
    // recent units dominate. Multiply by the island count for the rate of the whole node.
    double islandWorkRate = 0;
    uint64_t samples = 0;

    // Folds one finished unit into the cumulative totals and into the work rate average
    void add_sample(uint64_t workTimeMs, uint64_t evolveTimeMs, uint64_t population, uint64_t islandCount,
                    uint64_t generationCount);

private:
    //####################################
    //# BOOST SERIALIZE
    //####################################

    friend class boost::serialization::access;

    template <typename Archive>
    void serialize(Archive& ar, const unsigned version)
    {
        if (version < 3)
        {
            // Earlier versions counted individuals without the generations, which is not a work
            // rate and cannot be converted into one.
            throw boost::archive::archive_exception(boost::archive::archive_exception::unsupported_class_version);
        }

        ar & BOOST_SERIALIZATION_NVP(workTime);
        ar & BOOST_SERIALIZATION_NVP(evolveTime);
        ar & BOOST_SERIALIZATION_NVP(processedPopulation);
        ar & BOOST_SERIALIZATION_NVP(islandWorkRate);
        ar & BOOST_SERIALIZATION_NVP(samples);
    }
};

BOOST_CLASS_VERSION(work_stats, 3)

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
                              uint64_t evolveTimeMs, size_t islandCount, size_t generationCount);

    /**
     * Records what the worker itself knows about its hardware, see MsgType::WORKER_INFO. This is
     * also what admits it to the connected set, because a plan built without it would misjudge
     * the node; a worker reconnecting with a stored record is admitted by worker_joined().
     */
    void worker_reported_cores(const std::string& workerID, size_t coreCount);

    // Drops every record, so that settings which could not be read are started over
    void clear();

    std::optional<worker_info> get_worker_info(const std::string& workerID);

    std::unordered_set<std::string> get_connected_workers();

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
