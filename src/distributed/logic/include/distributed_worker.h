#pragma once
#include <filesystem>
#include <numeric>
#include <thread>

#include "dealer_socket.h"
#include "logger_control.h"
#include "logger_init.h"
#include "zmq.hpp"
#include "MsgType.h"
#include "pair_socket.h"
#include "router_socket.h"
#include "settings.h"
#include "worker_settings.h"
#include "zmq_addon.hpp"
#include "pagmo/archipelago.hpp"
#include "pagmo/utils/constrained.hpp"

enum class worker_mode
{
    SINGLE_THREADED,
    ARCHIPELAGO_BASED,
};

class distributed_worker : public logger_control
{
    // Worker's settings (saved to FS)
    settings<worker_settings> _settings;

    // Interval between which the socket sends ping messages
    int _heartbeat_interval;
    // Peer is considered dead if no ping is received after this interval
    int _heartbeat_timeout;
    // Maximal SYN packet interval (for when controller is not reachable) exponentially increases to this value
    int _reconnect_ivl_max;

    zmq::context_t _ctx;
    distributed::dealer_socket _workerSocket;

    std::string _workerId;

    // With this we can check multiple sockets for message recv events in a callback-style syntax
    zmq::active_poller_t _poller;

    // Work-related code should run on this thread, so we don't block the client message loop
    std::thread _workerThread;
    // Cannot use pair_socket here, because we also need udp_registry's handler to send messages to socket too
    distributed::router_socket _threadSocket;

    // Allows this network client to run in the background (function run_client)
    std::thread _clientThread;

    void _start_worker_thread(const std::vector<std::byte>& workData);

    void _handle_Worker_Socket_Msg();
    void _handle_Thread_Socket_Msg();

    /**
     * Spawns a new thread and then simply calls algo.evolve(pop), this causes
     * the evolution algorithm to use only a single CPU core
     */
    void _single_threaded_worker(const pagmo::algorithm& algo, const pagmo::population& pop);

    /**
     * Archipelago based worker, causes multiple CPU cores to be used
     */
    void _archipelago_based_worker(pagmo::algorithm& algo, pagmo::population& pop, size_t archiCycleCount);

    // Minimal pop size for the archipelago worker's islands
    size_t _minIslandPopSize;

    /**
     * Helper function to figure out the optimal island count for this archipelago worker based on hardware core count
     * @return Optimal number of islands
     */
    static unsigned _compute_optimal_island_count();

    /**
     * Helper function to split the input population among multiple islands
     */
    std::vector<pagmo::population>
    _split_population_for_islands(const pagmo::population& pop, size_t islandCount) const;

    worker_mode _workerMode;

public:
    /**
     * Constructs a distributed worker, which will connect and accept work from a specified controller
     * @param controllerAddress Controller's address
     * @param workerMode Can be either SINGLE_THREADED or ARCHIPELAGO_BASED (default)
     * @param heartbeatInterval Interval between which the socket sends ping messages
     * @param heartbeatTimeout Peer is considered dead if no ping is received after this interval
     * @param reconnectIvlMax Maximal SYN packet interval (exponentially increases to this value)
     * @param settingsFilePath Location of the worker settings file (including the filename)
     * @param minIslandPopSize Minimal population size of an island when using archipelago based worker
     */
    explicit distributed_worker(const std::string& controllerAddress,
                                worker_mode workerMode = worker_mode::ARCHIPELAGO_BASED,
                                size_t minIslandPopSize = 80,
                                int heartbeatInterval = 2000, // 2 seconds
                                int heartbeatTimeout = 300000, // 5 minutes
                                int reconnectIvlMax = 10000, // 10 seconds
                                const std::filesystem::path& settingsFilePath = "./worker_settings.xml"
    );

    void client_loop();
    void run_client();

    ~distributed_worker();

    /**
    * This function exists so udp_registry can call it and get a DLL file from the controller
    * @param lib_name Name of the DLL
    * @return DLL file as vector of bytes, or empty optional if not found
    */
    std::optional<std::vector<std::byte>> get_dll_from_controller(const std::string& lib_name);

    /**
     * Blocking request for a named blob held by the controller; the counterpart of
     * get_dll_from_controller for data that is shared by every work unit.
     */
    std::optional<std::vector<std::byte>> get_blob_from_controller(const std::string& blob_key);

protected:
    std::string get_default_log_name() const override
    {
        return "./logs/worker";
    }
};
