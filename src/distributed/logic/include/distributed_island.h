#pragma once

#include <string>


#include "dealer_socket.h"


class distributed_island
{
    // We need to wrap there in shared ptr to make this class copy and move constructable
    // Or else it won't be recognized as UDI by Pagmo
    std::shared_ptr<zmq::context_t> _ctx;
    std::shared_ptr<distributed::dealer_socket> _dealerSocket;

    std::string _islandId;

    // Controller will try to assign worker identified by this ID to this island
    std::string _preferredWorkerId;
    // Cycle count for the worker's archipelago evolve
    size_t _cycleCount = 1;
    // Generations the algorithm runs per cycle; the controller needs it to measure the unit
    size_t _generationCount = 0;

    static std::tuple<pagmo::algorithm, pagmo::population> _load_pagmo_pop_and_algo(const pagmo::island& isl);

public:
    void set_preferred_worker(const std::string& preferredWorkerId);

    void set_cycle_count(const size_t cycleCount);

    void clear_preferred_worker();

    void clear_cycle_count();

    // Default ctor.
    distributed_island();

    distributed_island(const std::string& preferred_worker_id, size_t cycle_count, size_t generation_count);

    // Island's name.
    std::string get_name() const;
    // Extra info.
    std::string get_extra_info() const;

    // run_evolve implementation.
    void run_evolve(pagmo::island&) const;
};
