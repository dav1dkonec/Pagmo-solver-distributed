#include "distributed_controller.h"
#include "logger_init.h"

#include <iostream>

#include "vector_deserialize.h"
#include "vector_istreambuf.h"
#include "udp_registry.h"

//#####################################################################################
//# Handling of socket messages
//#####################################################################################

void distributed_controller::_handle_Workers_Socket_Msg()
{
    auto [workerId, type, binary] = _workersSocket.receive();

    dist_logger()->trace("[{}] from worker", static_cast<int>(type));

    switch (type)
    {
    case MsgType::WORKER_JOIN:
        // Do nothing if we already have the worker saved as "joined"
        if (_freeWorkersPool.contains(workerId))
            break;

        dist_logger()->info("Worker {} joined", workerId);

        _settings().workerInfo.worker_joined(workerId);
        _add_free_worker(workerId);

        dist_logger()->debug("Free workers: {}", _freeWorkersPool.size());
        break;

    case MsgType::WORKER_LEAVE:
        {
            dist_logger()->info("Worker {} disconnected", workerId);

            _freeWorkersPool.erase(workerId);
            _settings().workerInfo.worker_left(workerId);

            // Handle busy worker leave (if worker is allocated work)
            if (_workAllocationMap.contains(workerId))
            {
                const auto [islandId, workData] = _workAllocationMap.at(workerId);
                _workAllocationMap.erase(workerId);
                // Call the same function which is called when island first requests work allocation
                _allocate_island_work(islandId, workData);
            }
        }
        break;
    case MsgType::WORK_RESULTS:
        {
            // Write stats into the worker info repository
            const auto workResults = vector_deserialize<work_container>(binary);
            _settings().workerInfo.worker_finished_work(workerId, workResults.evolvedPopulation,
                                                       workResults.algo.get_name(), workResults.evolveTimeMs);

            // Pass results from worker to island
            const auto myIslandId = _workAllocationMap.at(workerId).islandId;
            _islandsSocket.send(myIslandId, MsgType::WORK_RESULTS, binary);

            // Remove this {workerId, record} work allocation
            _workAllocationMap.erase(workerId);
            // Add worker back into the free pool
            _add_free_worker(workerId);
        }
        break;

    case MsgType::GET_DLL:
        {
            // Deserialize the DLL request so we can get the name of this DLL
            const auto dll_request = vector_deserialize<get_dll_request>(binary);

            // Get the DLL file via udp_registry (returns std::optional, Worker's udp_registry will throw if it's nullopt)
            const auto file = udp_registry::get().get_lib_as_file(dll_request.dll_name);
            _workersSocket.send(workerId,
                                MsgType::DLL_BINARY,
                                dll_binary_container{dll_request.dll_name, file}
            );
        }
        break;

    default:
        dist_logger()->warn("WARNING: {} sent unhandled message type: {}", workerId, static_cast<int>(type));
    }
}

void distributed_controller::_handle_Islands_Socket_Msg()
{
    auto [islandId, type, binary] = _islandsSocket.receive();
    dist_logger()->trace("[{}] from island", static_cast<int>(type));

    switch (type)
    {
    case MsgType::ALLOCATE_WORK:
        /*
         * Island Requested Work Allocation
         *
         * Allocate worker to this island if there are free workers,
         * otherwise put island into the _islandsWaitingForAlloc set
        */
        _allocate_island_work(islandId, binary);
        break;
    default:
        dist_logger()->warn("WARNING: {} sent unhandled message type: {}", islandId, static_cast<int>(type));
    }
}

//#####################################################################################
//# Controller data logic
//#####################################################################################

void distributed_controller::_allocate_island_work(const std::string& islandId, const std::vector<std::byte>& workData)
{
    if (_freeWorkersPool.empty())
    {
        _islandsWaitingForAlloc.emplace(islandId, workData);
        dist_logger()->info("Island {}is waiting for allocation", islandId);
    }
    else
    {
        _allocate_worker_to_island(islandId, workData);
    }
}

void distributed_controller::_allocate_worker_to_island(const std::string& islandId,
                                                        const std::vector<std::byte>& workData)
{
    const auto serializedData = vector_deserialize<work_container>(workData);
    const std::string preferredWorker = serializedData.preferredWorkerId;

    // By default we use the first free worker
    std::string workerId = *_freeWorkersPool.begin();
    // If the preferred worker is available in the free workers pool, use it instead
    if (_freeWorkersPool.contains(preferredWorker))
    {
        workerId = preferredWorker;
        dist_logger()->debug("Succesfully selected preferred worker");
    }

    // Erase this id, worker is no longer free, make a record for this allocation
    _freeWorkersPool.erase(workerId);
    _workAllocationMap.emplace(workerId, work_allocation_record{islandId, workData});

    dist_logger()->debug("{} has been allocated {}", islandId, workerId);

    // Write stats into the worker info repository
    _settings().workerInfo.worker_started_work(workerId);

    // Send data to the selected worker
    _workersSocket.send(workerId, MsgType::ALLOCATE_WORK, workData);
}

std::tuple<std::string, std::vector<std::byte>> distributed_controller::_pop_waiting_island()
{
    const auto islandRecord = _islandsWaitingForAlloc.begin();
    const std::string islandId = islandRecord->first;
    const std::vector<std::byte> workData = islandRecord->second;

    _islandsWaitingForAlloc.erase(islandRecord);
    return {islandId, workData};
}

void distributed_controller::_add_free_worker(const std::string& workerId)
{
    /*
     * A worker is free (joined or finished work)
     *
     * If there are islands waiting for allocation, allocate worker to one of them,
     * otherwise save worker ID into the free workers pool
    */
    _freeWorkersPool.emplace(workerId);

    if (!_islandsWaitingForAlloc.empty())
    {
        const auto [islandId, workData] = _pop_waiting_island();
        _allocate_worker_to_island(islandId, workData);
    }
}

//#####################################################################################
//# Sockets setup & initialization
//#####################################################################################

distributed_controller::distributed_controller(const std::string& controllerAddress,
                                               const int heartbeatInterval,
                                               const int heartbeatTimeout,
                                               const int reconnectIvlMax,
                                               const std::filesystem::path& settingsFilePath) :
    _heartbeat_interval(heartbeatInterval),
    _heartbeat_timeout(heartbeatTimeout),
    _reconnect_ivl_max(reconnectIvlMax),
    _workersSocket{_ctx},
    _islandsSocket{_ctx},
    _settings{settingsFilePath}
{
    // Set ping interval and ping timeout for the controller->workers socket
    _workersSocket.get_socket().set(zmq::sockopt::heartbeat_ivl, _heartbeat_interval);
    _workersSocket.get_socket().set(zmq::sockopt::heartbeat_timeout, _heartbeat_timeout);

    // TCP SYN max interval
    _workersSocket.get_socket().set(zmq::sockopt::reconnect_ivl_max, _reconnect_ivl_max);

    // Set disconnect message for workers (will be sent to us if worker is disconnected)
    int disconnectMsg = static_cast<int>(MsgType::WORKER_LEAVE);
    _workersSocket.get_socket().set(zmq::sockopt::disconnect_msg, zmq::buffer(&disconnectMsg, sizeof(disconnectMsg)));

    _poller.add(_workersSocket.get_socket(), zmq::event_flags::pollin,
                [this](zmq::event_flags e)
                {
                    _handle_Workers_Socket_Msg();
                });

    _poller.add(_islandsSocket.get_socket(), zmq::event_flags::pollin,
                [this](zmq::event_flags e)
                {
                    _handle_Islands_Socket_Msg();
                });

    try
    {
        _workersSocket.bind(controllerAddress);
    }
    catch (const std::exception& e)
    {
        dist_logger()->error("Socket failed to bind to address {} due to the following error: {}", controllerAddress,
                           e.what());
        dist_logger()->error("Check if this port isn't already in use");
        throw e;
    }

    // Possibly define the ipc:// address as some global constant? Make the address unique per process so we can run multiple controllers?
    _islandsSocket.bind("ipc://distributed_controller_islands_socket");
}

void distributed_controller::run_server()
{
    _serverThread = std::thread([this]
    {
        try
        {
            for (;;)
            {
                _poller.wait(std::chrono::milliseconds{-1});
            }
        }
        catch (...)
        {
            // poller throws an exception if it's interrupted, see destructor, this way we can shut down the thread
            return;
        }
    });
}

distributed_controller::~distributed_controller()
{
    _settings.save();

    if (_serverThread.joinable())
    {
        // This should shut down the poller and its thread
        _workersSocket.get_socket().close();
        _islandsSocket.get_socket().close();
        _ctx.shutdown();

        _serverThread.join();
    }
}

//####################################################################################
//# Other public member functions
//#####################################################################################

worker_info_repository& distributed_controller::get_worker_info_repository()
{
    return _settings().workerInfo;
}
