#include "distributed_worker.h"

#include "logger_init.h"

#include <chrono>
#include <cstdint>
#include <iostream>

#include "population_tools.h"
#include "vector_deserialize.h"
#include "pagmo/archipelago.hpp"
#include "pagmo/islands/thread_island.hpp"
#include "pagmo/topologies/fully_connected.hpp"
#include "pagmo/utils/multi_objective.hpp"

namespace
{
    uint64_t elapsed_ms(const std::chrono::steady_clock::time_point start)
    {
        const auto delta = std::chrono::steady_clock::now() - start;
        return std::chrono::duration_cast<std::chrono::milliseconds>(delta).count();
    }
}

//#####################################################################################
//# Handling of socket messages
//#####################################################################################

void distributed_worker::_handle_Worker_Socket_Msg()
{
    auto [type, binary] = _workerSocket.receive();

    dist_logger()->trace("[{}] from controller", static_cast<int>(type));

    switch (type)
    {
    case MsgType::ALLOCATE_WORK:
        {
            // Do not deserialize here, because it can possibly create an instance of udp_dll_wrapper,
            // which blocks the thread by calling socket.receive in get_dll_from_controller().
            _start_worker_thread(binary);
        }
        break;
    case MsgType::DLL_BINARY:
        // Controller has responded to Worker thread's GET_DLL message.
        // This means we need to forward this to the worker thread via the thread socket
        _threadSocket.send("worker_dll_handler", MsgType::DLL_BINARY, binary);
        break;

    default:
        dist_logger()->warn("WARNING: controller sent unhandled message type: {}", static_cast<int>(type));
    }
}

void distributed_worker::_handle_Thread_Socket_Msg()
{
    auto [senderId, type, binary] = _threadSocket.receive();

    switch (type)
    {
    case MsgType::WORK_RESULTS:
        // The standard execution path - worker thread completed work and returned results
        _workerThread.join();
        _workerSocket.send(MsgType::WORK_RESULTS, binary);
        break;

    case MsgType::GET_DLL:
        // Worker thread requests a DLL, which needs to be obtained from the controller.
        // This happens if the DLL is not available locally via udp_registry.
        // We thus forward it via workerSocket to controller, so it can process this request and respond with DLL_BINARY.
        _workerSocket.send(MsgType::GET_DLL, binary);
        break;

    default:
        dist_logger()->warn("WARNING: worker thread socket sent unhandled message type: {}", static_cast<int>(type));
    }
}

//#####################################################################################
//# Worker logic
//#####################################################################################

void distributed_worker::_single_threaded_worker(const pagmo::algorithm& algo, const pagmo::population& pop)
{
    dist_logger()->debug("Single-threaded worker started... ");

    distributed::dealer_socket output{this->_ctx};
    output.set_routing_id("worker_main");
    output.connect("inproc://thread_socket");

    dist_logger()->debug("Running algorithm: {}", algo.get_name());
    dist_logger()->debug("Population size: {}", pop.size());

    const auto evolveStart = std::chrono::steady_clock::now();
    const pagmo::population new_pop = algo.evolve(pop);

    work_container results{algo, new_pop};
    results.evolveTimeMs = elapsed_ms(evolveStart);
    results.evolvedPopulation = pop.size();
    output.send(MsgType::WORK_RESULTS, results);
}


unsigned distributed_worker::_compute_optimal_island_count()
{
    unsigned islandCount = std::thread::hardware_concurrency();
    if (islandCount == 0)
    {
        dist_logger()->debug("Defaulting to 8 islands (Cannot detect core count)");
        islandCount = 8;
    }
    else
    {
        dist_logger()->debug("Using {} islands", islandCount);
    }
    return islandCount;
}


std::vector<pagmo::population> distributed_worker::_split_population_for_islands(
    const pagmo::population& pop, const size_t islandCount) const
{
    const size_t totalPopSize = pop.size();

    if (islandCount == 0)
    {
        throw std::runtime_error("_split_population_for_islands - Cannot split among 0 islands");
    }

    const pagmo::problem& prob = pop.get_problem();
    const size_t seed = pop.get_seed();

    // Round down to the nearest population count divisible by 4, make sure to not go below min pop
    size_t baseSize = std::max(_minIslandPopSize, totalPopSize / islandCount);
    baseSize -= baseSize % 4;
    // The subtraction above can possibly cause empty population
    if (baseSize == 0)
        baseSize = 4;

    // We're subtracting, so the population among all islands can be smaller than the total pop
    const size_t assignedPop = baseSize * islandCount;
    size_t remainingPop = (assignedPop < totalPopSize) ? totalPopSize - assignedPop : 0;

    std::vector<pagmo::population> islandPops;
    islandPops.reserve(islandCount);

    size_t mainPopIndex = 0;
    for (size_t i = 0; i < islandCount; ++i)
    {
        // We add extra population to islands if any is remaining after the initial subtractions
        size_t currentSize = baseSize;
        if (remainingPop >= 4)
        {
            currentSize += 4;
            remainingPop -= 4;
        }

        // Assign individuals from the original population to this island's population
        // The population is initially filled with random individuals, so breaking out of this loop is valid
        pagmo::population islandPop{prob, currentSize, static_cast<unsigned>(seed + i)};
        for (size_t j = 0; j < currentSize; ++j)
        {
            if (mainPopIndex >= totalPopSize)
                break;
            islandPop.set_x(j, pop.get_x()[mainPopIndex]);
            ++mainPopIndex;
        }

        islandPops.emplace_back(std::move(islandPop));
    }

    return islandPops;
}

void distributed_worker::_archipelago_based_worker(pagmo::algorithm& algo, pagmo::population& pop,
                                                   const size_t archiCycleCount)
{
    dist_logger()->debug("Archipelago-based worker started... ");

    // 1) Set up socket for communicating with the parent thread
    distributed::dealer_socket output{this->_ctx};
    output.set_routing_id("worker_main");
    output.connect("inproc://thread_socket");

    // Everything from here to step 7 is computation done by this node. The controller
    // cannot see where its own round-trip measurement ends and this begins.
    const auto evolveStart = std::chrono::steady_clock::now();

    // 2) Get island count based on hardware core count
    const unsigned islandCount = _compute_optimal_island_count();

    // 3) Construct and initialize islands with our algorithm and population (pop includes problem)
    pagmo::archipelago archi{pagmo::fully_connected{}};
    const auto islandPops = _split_population_for_islands(pop, islandCount);
    size_t evolvedPopulation = 0;
    for (size_t i = 0; i < islandCount; ++i)
    {
        dist_logger()->debug("Archipelago worker island #{} population size: {}", i + 1, islandPops[i].size());
        evolvedPopulation += islandPops[i].size();
        // Each island gets its portion of the population
        archi.push_back(pagmo::island{algo, islandPops[i]});
    }

    // 4) Run evolution on all islands in parallel
    dist_logger()->debug("Using algorithm: {}", algo.get_name());
    archi.evolve(archiCycleCount);
    archi.wait_check();

    // 5) Merge individuals (and their fitness) from all islands into two vectors
    const auto [allPopulations, allFitnesses] = merge_populations(archi);
    dist_logger()->trace("Size of allPopulations: {}", allPopulations.size());

    // 6) Build a new population containing only the best POPULATION_SIZE individuals
    auto firstIslPop = archi[0].get_population();
    const auto newPop = select_best_N_into_new_population(
        firstIslPop.get_problem(),
        allPopulations,
        allFitnesses,
        pop.size(),
        firstIslPop.get_seed()
    );

    // 7) Send the algorithm (taken from the first island) and new population back to controller
    work_container results{archi[0].get_algorithm(), newPop};
    results.evolveTimeMs = elapsed_ms(evolveStart);
    results.evolvedPopulation = evolvedPopulation;
    output.send(MsgType::WORK_RESULTS, results);
}

void distributed_worker::_start_worker_thread(const std::vector<std::byte>& workData)
{
    // We start the new thread right here, any further blocking calls won't affect the main thread
    _workerThread = std::thread(
        [this, workData]()
        {
            auto wct = vector_deserialize<work_container>(workData);

            try
            {
                if (_workerMode == worker_mode::ARCHIPELAGO_BASED)
                {
                    _archipelago_based_worker(wct.algo, wct.pop, wct.cycleCount);
                }
                else
                {
                    _single_threaded_worker(wct.algo, wct.pop);
                }
            }
            catch (const std::exception& e)
            {
                dist_logger()->critical("Worker thread failed with exception: {}", e.what());
                throw e;
            }

            dist_logger()->debug("Worker thread finished. ");
        });
}

//#####################################################################################
//# Sockets setup & initialization
//#####################################################################################

distributed_worker::distributed_worker(const std::string& controllerAddress,
                                       const worker_mode workerMode,
                                       const size_t minIslandPopSize,
                                       const int heartbeatInterval,
                                       const int heartbeatTimeout,
                                       const int reconnectIvlMax,
                                       const std::filesystem::path& settingsFilePath) :
    _settings(settingsFilePath),
    _heartbeat_interval(heartbeatInterval),
    _heartbeat_timeout(heartbeatTimeout),
    _reconnect_ivl_max(reconnectIvlMax),
    _workerSocket(_ctx),
    _threadSocket(_ctx),
    _minIslandPopSize(minIslandPopSize),
    _workerMode(workerMode)
{
    // Set ping interval and ping timeout for the worker->controller socket
    _workerSocket.get_socket().set(zmq::sockopt::heartbeat_ivl, _heartbeat_interval);
    _workerSocket.get_socket().set(zmq::sockopt::heartbeat_timeout, _heartbeat_timeout);

    // Set maximal SYN packet intervals (for when controller is not reachable) exponentially increases to this value
    _workerSocket.get_socket().set(zmq::sockopt::reconnect_ivl_max, _reconnect_ivl_max);


    // Set hello message for controller (will be sent to controller every time worker connects or reconnects)
    int connectMsg = static_cast<int>(MsgType::WORKER_JOIN);
    _workerSocket.get_socket().set(zmq::sockopt::hello_msg, zmq::buffer(&connectMsg, sizeof(connectMsg)));

    // Poller callback - worker socket has message
    _poller.add(_workerSocket.get_socket(), zmq::event_flags::pollin,
                [this](zmq::event_flags e)
                {
                    _handle_Worker_Socket_Msg();
                });

    // Poller callback - thread socket has message
    _poller.add(_threadSocket.get_socket(), zmq::event_flags::pollin,
                [this](zmq::event_flags e)
                {
                    _handle_Thread_Socket_Msg();
                });

    // Thread socket always listens on this address
    _threadSocket.bind("inproc://thread_socket");

    // Configure the worker socket so it can communicate with the controller
    _workerId = _settings().workerId;
    _workerSocket.set_routing_id(_workerId);
    _workerSocket.connect(controllerAddress);
}

void distributed_worker::client_loop()
{
    // -1 means no poller timeout
    _poller.wait(std::chrono::milliseconds{-1});
}

void distributed_worker::run_client()
{
    _clientThread = std::thread([this]
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

distributed_worker::~distributed_worker()
{
    _settings.save();

    if (_clientThread.joinable())
    {
        _workerSocket.get_socket().close();
        _threadSocket.get_socket().close();
        _ctx.shutdown();

        _clientThread.join();

    }

}

//#####################################################################################
//# Other public member functions
//#####################################################################################

std::optional<std::vector<std::byte>> distributed_worker::get_dll_from_controller(const std::string& lib_name)
{
    const auto myId = "worker_dll_handler";

    distributed::dealer_socket socket{this->_ctx};
    socket.set_routing_id(myId);
    socket.connect("inproc://thread_socket");

    // 1) Send the request
    socket.send(MsgType::GET_DLL, get_dll_request{lib_name});

    // 2) Block until controller eventually replies with DLL_BINARY
    std::tuple<MsgType, std::vector<std::byte>> receivedData{};
    dist_logger()->debug("Worker waiting for DLL from controller... (Name: {})", lib_name);
    receivedData = socket.receive();
    while (std::get<0>(receivedData) != MsgType::DLL_BINARY)
    {
        dist_logger()->warn("Blocking until next message... Worker received incorrect reply to GET_DLL message: enum type:{}", static_cast<int>(std::get<0>(receivedData)));
        receivedData = socket.receive();
    }

    // 3) Return the file itself
    auto dbc = vector_deserialize<dll_binary_container>(std::get<1>(receivedData));

    return dbc.dll_file;
}
