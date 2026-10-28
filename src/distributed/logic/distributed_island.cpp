#include "distributed_island.h"
#include "logger_init.h"

#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <pagmo/algorithm.hpp>
#include <pagmo/detail/gte_getter.hpp>
#include <pagmo/exceptions.hpp>
#include <pagmo/island.hpp>
#include <pagmo/population.hpp>
#include <pagmo/s11n.hpp>
#include <pagmo/threading.hpp>

#include "UUID.h"
#include "vector_deserialize.h"
#include "vector_istreambuf.h"
#include "pagmo/utils/multi_objective.hpp"


std::tuple<pagmo::algorithm, pagmo::population> distributed_island::_load_pagmo_pop_and_algo(const pagmo::island& isl)
{
    // Load algorithm and population from the island object in exactly the same way as pagmo::thread_island
    const auto gte = pagmo::detail::gte_getter();
    (void)gte;

    const auto algo(isl.get_algorithm());
    auto pop(isl.get_population());

    if (algo.get_thread_safety() < pagmo::thread_safety::basic)
    {
        pagmo_throw(std::invalid_argument,
                    "the 'distributed_island' UDI requires an algorithm providing at least the 'basic' "
                    "thread safety guarantee, but an algorithm of type '"
                    + algo.get_name() + "' does not");
    }

    if (pop.get_problem().get_thread_safety() < pagmo::thread_safety::basic)
    {
        pagmo_throw(std::invalid_argument,
                    "the 'distributed_island' UDI requires a problem providing at least the 'basic' "
                    "thread safety guarantee, but a problem of type '"
                    + pop.get_problem().get_name() + "' does not");
    }

    return {algo, pop};
}

void distributed_island::set_preferred_worker(const std::string& preferredWorkerId)
{
    _preferredWorkerId = preferredWorkerId;
}

void distributed_island::set_cycle_count(const size_t cycleCount)
{
    _cycleCount = cycleCount;
}

void distributed_island::clear_preferred_worker()
{
    _preferredWorkerId.clear();
}

void distributed_island::clear_cycle_count()
{
    _cycleCount = 1;
}

distributed_island::distributed_island() : _ctx{new zmq::context_t{}},
                                           _dealerSocket{new distributed::dealer_socket(*_ctx)}
{
    _islandId = "island_" + uuid::v4::UUID::New().String();
}

distributed_island::distributed_island(const std::string& preferred_worker_id, const size_t cycle_count) :
    _ctx{new zmq::context_t{}},
    _dealerSocket{new distributed::dealer_socket(*_ctx)},
    _preferredWorkerId(preferred_worker_id),
    _cycleCount(cycle_count)
{
    _islandId = "island_" + uuid::v4::UUID::New().String();
}

// Island's name.
std::string distributed_island::get_name() const
{
    return "Distributed island";
}

// Island's extra info.
std::string distributed_island::get_extra_info() const
{
    return std::string("ID: ") + _islandId;
}

// Run evolve.
void distributed_island::run_evolve(pagmo::island& isl) const
{
    /*
    * 1) Define the work thread
    */

    auto impl = [&isl, this]()
    {
        // 1) Load pop and algo
        auto [initial_algo, initial_pop] = _load_pagmo_pop_and_algo(isl);

        // 2) Allocate our algorithm and population to some worker node via controller
        _dealerSocket->set_routing_id(_islandId);
        dist_logger()->debug("Running distributed island");
        _dealerSocket->connect("ipc://distributed_controller_islands_socket");
        dist_logger()->debug("Distributed island connected");
        _dealerSocket->send(MsgType::ALLOCATE_WORK,
                            work_container{initial_algo, initial_pop, _preferredWorkerId, _cycleCount}
        );
        dist_logger()->debug("Distributed island allocate work sent");

        // 3) Wait until controller returns results from worker
        auto [type, binary] = _dealerSocket->receive();
        dist_logger()->debug("island received [{}] from controller", static_cast<int>(type));

        if (type != MsgType::WORK_RESULTS)
        {
            throw std::runtime_error(
                "Distributed island expected WORK_RESULTS from Controller, got enum value: " + std::to_string(
                    static_cast<int>(type)));
        }

        // 4) Pass results back to the pagmo::island object
        // Deserialize received data
        const auto work_results = vector_deserialize<work_container>(binary);

        // Replace the pagmo::island's population with the evolved population
        isl.set_population(work_results.pop);
        // Replace the pagmo::island's algorithm with the algorithm used for the evolution
        isl.set_algorithm(work_results.algo);
    };

    /*
    * 2) Execute the island thread
    */

    std::exception_ptr eptr;
    std::thread worker([&impl, &eptr]()
    {
        try
        {
            impl();
        }
        catch (...)
        {
            eptr = std::current_exception();
        }
    });

    worker.join();
    std::string errMsg = "Aborting distributed island, the thread has thrown an exception: ";
    try
    {
        if (eptr)
            std::rethrow_exception(eptr);
    }
    catch (const std::exception& e)
    {
        errMsg += e.what();
        dist_logger()->critical("{}", errMsg);
        throw std::runtime_error(errMsg);
    }
    catch (const std::string& e)
    {
        errMsg += e;
        dist_logger()->critical("{}", errMsg);
        throw std::runtime_error(errMsg);
    }
    catch (const char* e)
    {
        errMsg += e;
        dist_logger()->critical("{}", errMsg);
        throw std::runtime_error(errMsg);
    }
    catch (...)
    {
        errMsg += "unknown exception type";
        dist_logger()->critical("{}", errMsg);
        throw std::runtime_error(errMsg);
    }
}
