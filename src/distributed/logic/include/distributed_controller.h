#pragma once
#include <thread>
#include <unordered_set>

#include "controller_settings.h"
#include "logger_control.h"
#include "router_socket.h"
#include "settings.h"
#include "worker_info_repository.h"
#include "zmq.hpp"
#include "zmq_addon.hpp"

/**
 * A helper struct, used to keep track of which worker is allocated which island's work,
 * so we can re-allocate in case the worker disconnects or fails.
 */
struct work_allocation_record
{
    std::string islandId;
    std::vector<std::byte> workData;
    // The count the controller sent out. The one the worker sends back is ignored, because this
    // figure feeds the planner and a node must not size its own share.
    size_t generationCount{};
};


class distributed_controller
{
    // Interval between which the socket sends ping messages
    int _heartbeat_interval;
    // Peer is considered dead if no ping is received after this interval
    int _heartbeat_timeout;
    // Maximal SYN packet interval (for when worker is not reachable) exponentially increases to this value
    int _reconnect_ivl_max;

    zmq::context_t _ctx;
    zmq::active_poller_t _poller;

    distributed::router_socket _workersSocket;
    distributed::router_socket _islandsSocket;
    std::thread _serverThread;

    // "Settings" such as worker performance metrics, saved and loaded from the filesystem
    settings<controller_settings> _settings;

    // Islands which cannot be allocated to any worker (_freeWorkersPool is empty) are stored here along with their data
    std::unordered_map<std::string, std::vector<std::byte>> _islandsWaitingForAlloc{};
    // Worker nodes ready to be allocated to an island
    std::unordered_set<std::string> _freeWorkersPool{};
    // Pairs of {workerID, work record} indicate which worker is currently being used by which island
    std::unordered_map<std::string, work_allocation_record> _workAllocationMap{};

    void _add_free_worker(const std::string& workerId);
    void _handle_Workers_Socket_Msg();
    void _handle_Islands_Socket_Msg();

    void _allocate_island_work(const std::string& islandId, const std::vector<std::byte>& workData);

    void _allocate_worker_to_island(const std::string& islandId, const std::vector<std::byte>& workData);
    std::tuple<std::string, std::vector<std::byte>> _pop_waiting_island();

public:
    explicit distributed_controller(const std::string& controllerAddress,
                                    int heartbeatInterval = 1000,
                                    int heartbeatTimeout = 3000,
                                    int reconnectIvlMax = 1000,
                                    const std::filesystem::path& settingsFilePath = "./controller_settings.xml"
                                    );

    void run_server();

    ~distributed_controller();

    worker_info_repository& get_worker_info_repository();
};
