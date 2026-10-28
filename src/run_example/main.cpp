#include "logger_init.h"
#include "distributed_controller.h"
#include "distributed_solver.h"
#include "distributed_worker.h"
#include "udp_dll_wrapper.h"
#include "udp_registry.h"
#include "pagmo/algorithms/de.hpp"
#include "pagmo/algorithms/gaco.hpp"

#include <iostream>
#include <string>
#include <stdexcept>
#include <filesystem>

#include "benchmark_stats.h"
#include "mo_benchmark.h"
#include "so_benchmark.h"

//####################################################
//# DIFFERENT CONTROLLER TASKS
//####################################################

void benchmark_controller_task(const std::string& address, const size_t expectedWorkerCount,
                               const load_balancing_strategy loadBalancingStrategy, const std::string& localCacheDir,
                               bool disableLogging, bool runSO, bool runMO)
{
    udp_registry::get().set_local_cache_dir(localCacheDir);

    if (runSO)
    {
        // Single-objective benchmark
        distributed_solver ds{address, get_so_algorithm_count(), loadBalancingStrategy};
        benchmark_stats bench{};
        run_so_benchmark(ds, bench);
    }

    if (runMO)
    {
        // Multi-objective benchmark
        distributed_solver ds{address, get_mo_algorithm_count(), loadBalancingStrategy};
        benchmark_stats bench{};
        run_mo_benchmark(ds, bench);
    }
}

void default_controller_task(const std::string& address, const size_t expectedWorkerCount,
                             const load_balancing_strategy loadBalancingStrategy, const std::string& localCacheDir,
                             bool disableLogging)
{
    udp_registry::get().set_local_cache_dir(localCacheDir);

    unsigned param = 2;
    udp_dll_wrapper probWrapper{"schwefel_udp", std::any{param}};
    const pagmo::problem prob{probWrapper};
    pagmo::algorithm algo{pagmo::de(1500)};
    algo.set_verbosity(0);

    distributed_solver ds{address, expectedWorkerCount, loadBalancingStrategy};
    if (!disableLogging)
    {
        ds.enable_logging();
    }

    ds.wait_until_workers_connect();

    ds.evolve(prob, {algo}, 1000);
    ds.wait_until_completion();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    ds.evolve(prob, {algo}, 1000);
    ds.wait_until_completion();
    std::this_thread::sleep_for(std::chrono::milliseconds(5000));
}

//####################################################
//# CONTROLLER / WORKER RUN
//####################################################

void run_controller(const std::string& address, const size_t expectedWorkerCount,
                    const load_balancing_strategy loadBalancingStrategy, const std::string& localCacheDir,
                    const std::function<void(std::string, size_t, load_balancing_strategy, std::string, bool)>& runFunc,
                    bool disableLogging)
{
    std::string errMsg = "Aborting distributed solver, an exception occurred: ";
    try
    {
        runFunc(address, expectedWorkerCount, loadBalancingStrategy, localCacheDir, disableLogging);
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

void run_worker(const std::string& address,
                worker_mode mode,
                size_t minIslandPopSize,
                const std::string& localCacheDir,
                const std::filesystem::path& settingsFilePath,
                bool disableLogging)
{
    std::string errMsg = "Aborting distributed worker, an exception occurred: ";
    try
    {
        distributed_worker worker{
            address,
            mode,
            minIslandPopSize,
            1000,
            3000000,
            10000,
            settingsFilePath
        };

        if (!disableLogging)
        {
            worker.enable_logging();
        }

        udp_registry::get().set_local_cache_dir(localCacheDir);
        udp_registry::get().register_udp_provider(
            [&worker](const std::string& libName) { return worker.get_dll_from_controller(libName); });
        for (;;)
            worker.client_loop();
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

//####################################################
//# PARSING
//####################################################

void print_help(char** argv)
{
    const std::string fName = std::filesystem::path(argv[0]).filename().string();

    std::cout <<
        "Usage:\n"
        "  " << fName << " --controller [options]\n"
        "  " << fName << " --worker     [options]\n"
        "\n"
        "Common options:\n"
        "  --disable-logging\n"
        "  --benchmark-so  Run single-objective benchmarks\n"
        "  --benchmark-mo  Run multi-objective benchmarks\n"
        "\n"
        "Controller options:\n"
        "  --address      <addr>  (default: tcp://0.0.0.0:5000)\n"
        "  --workers      <n>     Expected worker count (default: 2)\n"
        "  --strategy     <name>  (default: BY_PERFORMANCE)\n"
        "  --cache-dir    <dir>   (default: controller_cache)\n"
        "\n"
        "Worker options:\n"
        "  --address      <addr>  (default: tcp://localhost:5000)\n"
        "  --mode         <name>  (default: ARCHIPELAGO_BASED)\n"
        "  --min-pop-size <n>     (default: 80)\n"
        "  --cache-dir    <dir>   (default: worker_cache)\n"
        "  --worker-dir   <name>  Per-worker isolated subdirectory name (default: auto-generated UUID)\n"
        "  --no-worker-isolation  Disable per-worker isolated subdirectory with cache and settings\n"
        << std::endl;
}

struct main_args
{
    bool isController = false;

    std::string address = ""; // Set by user
    std::string defaultAddressWorker = "tcp://localhost:5000";
    std::string defaultAddressController = "tcp://0.0.0.0:5000";
    size_t workers = 2;

    std::string strategy = "BY_PERFORMANCE";
    std::string workerMode = "ARCHIPELAGO_BASED";
    size_t minPopSize = 80;

    std::string cacheDir = ""; // Set by user
    std::string defaultCacheDirWorker = "worker_cache";
    std::string defaultCacheDirController = "controller_cache";

    bool runBenchmark = false;
    bool disableLogging = false;

    bool noWorkerDirectory = false;
    std::string workerDir = "";

    bool runSOBenchmark = false;
    bool runMOBenchmark = false;
};

std::optional<main_args> parse_main_args(const int argc, char** argv)
{
    main_args mainArgs{};
    bool modeSet = false;

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        auto nextArg = [&]() -> std::string
        {
            if (++i >= argc)
                throw std::runtime_error(a + " requires a value");
            return argv[i];
        };

        if (a == "--help" || a == "-h")
        {
            // handled in main
        }
        else if (a == "--controller")
        {
            mainArgs.isController = true;
            modeSet = true;
        }
        else if (a == "--benchmark-so")
        {
            mainArgs.runSOBenchmark = true;
        }
        else if (a == "--benchmark-mo")
        {
            mainArgs.runMOBenchmark = true;
        }
        else if (a == "--benchmark")
        {
            mainArgs.runBenchmark = true;
        }
        else if (a == "--disable-logging")
        {
            mainArgs.disableLogging = true;
        }
        else if (a == "--worker")
        {
            mainArgs.isController = false;
            modeSet = true;
        }
        else if (a == "--address")
        {
            mainArgs.address = nextArg();
        }
        else if (a == "--workers")
        {
            mainArgs.workers = std::stoul(nextArg());
        }
        else if (a == "--strategy")
        {
            mainArgs.strategy = nextArg();
        }
        else if (a == "--mode")
        {
            mainArgs.workerMode = nextArg();
        }
        else if (a == "--min-pop-size")
        {
            mainArgs.minPopSize = std::stoul(nextArg());
        }
        else if (a == "--cache-dir")
        {
            mainArgs.cacheDir = nextArg();
        }
        else if (a == "--worker-dir")
        {
            mainArgs.workerDir = nextArg();
        }
        else if (a == "--no-worker-isolation")
        {
            mainArgs.noWorkerDirectory = true;
        }
        else
        {
            std::cerr << "Unknown argument: " << a << "\n";
            modeSet = false;
            break;
        }
    }

    if (!modeSet)
    {
        return std::nullopt;
    }
    return mainArgs;
}

//####################################################
//# MAIN
//####################################################
int main(int argc, char* argv[])
{
    // Parse cmd line args
    const auto& mainArgsOpt = parse_main_args(argc, argv);
    if (!mainArgsOpt.has_value())
    {
        print_help(argv);
        return 2;
    }
    const auto& ma = mainArgsOpt.value();

    if (ma.disableLogging)
    {
        use_null_logger();
    }

    // Run controller / worker
    try
    {
        if (ma.isController)
        {
            const load_balancing_strategy lbs = (ma.strategy == "ALL_ISLANDS_EQUAL")
                                                    ? load_balancing_strategy::ALL_ISLANDS_EQUAL
                                                    : load_balancing_strategy::BY_PERFORMANCE;

            auto controllerTaskWrapper = [&](const std::string& addr, size_t count, load_balancing_strategy strat,
                                             const std::string& cache, bool log)
            {
                if (ma.runSOBenchmark || ma.runMOBenchmark)
                {
                    benchmark_controller_task(addr, count, strat, cache, log, ma.runSOBenchmark, ma.runMOBenchmark);
                }
                else
                {
                    default_controller_task(addr, count, strat, cache, log);
                }
            };

            run_controller(
                ma.address.empty() ? ma.defaultAddressController : ma.address,
                ma.workers,
                lbs,
                ma.cacheDir.empty() ? ma.defaultCacheDirController : ma.cacheDir,
                controllerTaskWrapper,
                ma.disableLogging
            );
        }
        else
        {
            const worker_mode wm = (ma.workerMode == "ARCHIPELAGO_BASED")
                                       ? worker_mode::ARCHIPELAGO_BASED
                                       : worker_mode::SINGLE_THREADED;

            const std::filesystem::path baseCacheDir = ma.cacheDir.empty() ? ma.defaultCacheDirWorker : ma.cacheDir;
            std::filesystem::path workerCacheDir;
            std::filesystem::path settingsFile;

            if (ma.noWorkerDirectory)
            {
                // Do NOT create a new dir for the worker
                workerCacheDir = baseCacheDir;
                settingsFile = baseCacheDir / "worker_settings.xml";
            }
            else
            {
                // Isolated worker directory
                const std::string workerId = ma.workerDir.empty()
                                                 ? ("worker_" + uuid::v4::UUID::New().String())
                                                 : ma.workerDir;
                const std::filesystem::path workerRoot = baseCacheDir / workerId;
                workerCacheDir = workerRoot / "cache";
                settingsFile = workerRoot / "worker_settings.xml";
                std::filesystem::create_directories(workerCacheDir);
            }

            run_worker(
                ma.address.empty() ? ma.defaultAddressWorker : ma.address,
                wm,
                ma.minPopSize,
                workerCacheDir.string(),
                settingsFile,
                ma.disableLogging
            );
        }
    }
    catch (const std::exception& e)
    {
        // Also on stderr, not just in the log: the log may be disabled, or the failure may
        // have happened before logging was configured.
        std::cerr << "Fatal: " << e.what() << std::endl;
        return 2;
    }

    return 0;
}
