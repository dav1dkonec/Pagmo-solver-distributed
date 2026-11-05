#include <catch2/catch_test_macros.hpp>
#include "worker_info_repository.h"
#include <thread>
#include <chrono>

TEST_CASE("Worker Info Repository - join and leave", "[worker-info]") {
    worker_info_repository repo;
    
    SECTION("worker counts as connected once it reports its hardware") {
        repo.worker_joined("worker1");
        REQUIRE(repo.get_worker_count() == 0);
        
        repo.worker_reported_cores("worker1", 4);
        REQUIRE(repo.get_connected_workers().contains("worker1"));
    }
    
    SECTION("rejoining with a known record needs no new report") {
        repo.worker_joined("worker1");
        repo.worker_reported_cores("worker1", 4);
        repo.worker_left("worker1");
        
        repo.worker_joined("worker1");
        REQUIRE(repo.get_worker_count() == 1);
    }
    
    SECTION("worker leave removes from connected") {
        repo.worker_joined("worker1");
        repo.worker_reported_cores("worker1", 4);
        repo.worker_left("worker1");
        REQUIRE(repo.get_worker_count() == 0);
    }
    
    SECTION("multiple workers") {
        for (const auto* id : {"w1", "w2", "w3"}) {
            repo.worker_joined(id);
            repo.worker_reported_cores(id, 4);
        }
        
        REQUIRE(repo.get_worker_count() == 3);
        
        repo.worker_left("w2");
        REQUIRE(repo.get_worker_count() == 2);
    }
}

TEST_CASE("Worker Info Repository - work tracking", "[worker-info]") {
    worker_info_repository repo;
    
    SECTION("track single job") {
        repo.worker_joined("worker1");
        repo.worker_started_work("worker1");
        
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        repo.worker_finished_work("worker1", 50, "DE", 12, 1);
        
        auto info = repo.get_worker_info("worker1");
        REQUIRE(info.has_value());
        REQUIRE(info->totalStats.processedPopulation == 50);
        REQUIRE(info->totalStats.workTime > 0);
        // The worker reported 12 ms of it as computation; the rest is transfer.
        REQUIRE(info->totalStats.evolveTime == 12);
        REQUIRE(info->totalStats.workTime >= info->totalStats.evolveTime);
    }
    
    SECTION("track multiple jobs") {
        repo.worker_joined("worker1");
        
        repo.worker_started_work("worker1");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        repo.worker_finished_work("worker1", 100, "DE", 5, 1);
        
        repo.worker_started_work("worker1");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        repo.worker_finished_work("worker1", 200, "PSO", 7, 1);
        
        auto info = repo.get_worker_info("worker1");
        REQUIRE(info->totalStats.processedPopulation == 300);
        REQUIRE(info->totalStats.evolveTime == 12);
    }
    
    SECTION("stats by algorithm") {
        repo.worker_joined("worker1");
        
        repo.worker_started_work("worker1");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        repo.worker_finished_work("worker1", 100, "DE", 4, 1);
        
        repo.worker_started_work("worker1");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        repo.worker_finished_work("worker1", 150, "DE", 6, 1);
        
        repo.worker_started_work("worker1");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        repo.worker_finished_work("worker1", 50, "PSO", 3, 1);
        
        auto info = repo.get_worker_info("worker1");
        REQUIRE(info->statsByAlgorithm["DE"].processedPopulation == 250);
        REQUIRE(info->statsByAlgorithm["PSO"].processedPopulation == 50);
        REQUIRE(info->statsByAlgorithm["DE"].evolveTime == 10);
        REQUIRE(info->statsByAlgorithm["PSO"].evolveTime == 3);
    }
}

TEST_CASE("Worker Info Repository - throughput", "[worker-info]") {
    worker_info_repository repo;
    repo.worker_joined("worker1");

    SECTION("throughput is measured per island, not per node") {
        repo.worker_started_work("worker1");
        repo.worker_finished_work("worker1", 800, "DE", 100, 4);

        // 800 individuals in 100 ms on 4 islands is 2 individuals per ms on each of them
        REQUIRE(repo.get_worker_info("worker1")->totalStats.islandThroughput == 2.0);
    }

    SECTION("a later sample ages the average instead of replacing it") {
        repo.worker_started_work("worker1");
        repo.worker_finished_work("worker1", 100, "DE", 100, 1);
        repo.worker_started_work("worker1");
        repo.worker_finished_work("worker1", 300, "DE", 100, 1);

        const auto throughput = repo.get_worker_info("worker1")->totalStats.islandThroughput;
        REQUIRE(throughput > 1.0);
        REQUIRE(throughput < 3.0);
    }

    SECTION("core count is zero until the worker reports it") {
        REQUIRE(repo.get_worker_info("worker1")->coreCount == 0);

        repo.worker_reported_cores("worker1", 4);
        REQUIRE(repo.get_worker_info("worker1")->coreCount == 4);
    }

    SECTION("a plain join leaves the throughput record alone") {
        repo.worker_started_work("worker1");
        repo.worker_finished_work("worker1", 400, "DE", 100, 2);

        repo.worker_joined("worker1");
        REQUIRE(repo.get_worker_info("worker1")->totalStats.islandThroughput == 2.0);
    }
}

TEST_CASE("Worker Info Repository - queries", "[worker-info]") {
    worker_info_repository repo;
    
    SECTION("get info for unknown worker") {
        auto info = repo.get_worker_info("unknown");
        REQUIRE(!info.has_value());
    }
    
    SECTION("get connected workers") {
        repo.worker_joined("w1");
        repo.worker_reported_cores("w1", 4);
        repo.worker_joined("w2");
        repo.worker_reported_cores("w2", 8);
        
        auto workers = repo.get_connected_workers();
        REQUIRE(workers.size() == 2);
        REQUIRE(workers.contains("w1"));
        REQUIRE(workers.contains("w2"));
    }
}

TEST_CASE("Worker Info Repository - error handling", "[worker-info]") {
    worker_info_repository repo;
    
    SECTION("start work on unknown throws") {
        REQUIRE_THROWS_AS(repo.worker_started_work("unknown"), std::runtime_error);
    }
    
    SECTION("finish work on unknown throws") {
        REQUIRE_THROWS_AS(
            repo.worker_finished_work("unknown", 100, "DE", 0, 1),
            std::runtime_error
        );
    }
}

TEST_CASE("Worker Info Repository - wait until", "[worker-info]") {
    worker_info_repository repo;
    
    SECTION("wait_until_worker_count blocks until reached") {
        std::thread t([&repo]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            repo.worker_joined("worker1");
            repo.worker_reported_cores("worker1", 4);
        });
        
        auto start = std::chrono::high_resolution_clock::now();
        repo.wait_until_worker_count(1);
        auto end = std::chrono::high_resolution_clock::now();
        
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        REQUIRE(elapsed.count() >= 100);
        
        t.join();
    }
    
    SECTION("timed wait gives up when nobody connects") {
        auto start = std::chrono::high_resolution_clock::now();
        const bool reached = repo.wait_until_worker_count(1, std::chrono::milliseconds(50));
        auto end = std::chrono::high_resolution_clock::now();
        
        REQUIRE_FALSE(reached);
        REQUIRE(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() >= 50);
    }
    
    SECTION("timed wait returns immediately when target is already met") {
        repo.worker_joined("worker1");
        repo.worker_reported_cores("worker1", 4);
        
        REQUIRE(repo.wait_until_worker_count(1, std::chrono::milliseconds(5000)));
    }
}
