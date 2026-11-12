#include <catch2/catch_test_macros.hpp>
#include "settings.h"
#include "worker_settings.h"
#include "controller_settings.h"
#include <filesystem>
#include <fstream>

TEST_CASE("Settings - basic operations", "[settings]") {
    std::string tempFile = "test_settings_" + std::to_string(time(nullptr)) + ".xml";
    
    SECTION("save and load") {
        {
            settings<worker_settings> s(tempFile, false);
            s().workerId = "test_worker_id";
            s.save();
        }
        
        {
            settings<worker_settings> s(tempFile, false);
            REQUIRE(s().workerId == "test_worker_id");
        }
    }
    
    SECTION("missing file initializes") {
        std::string newFile = "new_settings_" + std::to_string(time(nullptr)) + ".xml";
        std::filesystem::remove(newFile);
        
        settings<worker_settings> s(newFile, false);
        REQUIRE(!s().workerId.empty());
        
        std::filesystem::remove(newFile);
    }
    
    SECTION("auto-save enabled") {
        std::string autoFile = "auto_settings_" + std::to_string(time(nullptr)) + ".xml";
        std::string savedId;
        
        {
            settings<worker_settings> s(autoFile, true);
            savedId = s().workerId;
            auto& ref = s();
        }
        
        {
            settings<worker_settings> s(autoFile, false);
            REQUIRE(s().workerId == savedId);
        }
        
        std::filesystem::remove(autoFile);
    }
    
    std::filesystem::remove(tempFile);
}

TEST_CASE("Settings - worker settings", "[settings]") {
    std::string tempFile = "worker_test_" + std::to_string(time(nullptr)) + ".xml";
    
    SECTION("worker id persists") {
        std::string originalId;
        {
            settings<worker_settings> s(tempFile, false);
            originalId = s().workerId;
            s.save();
        }
        
        {
            settings<worker_settings> s(tempFile, false);
            REQUIRE(s().workerId == originalId);
        }
    }
    
    std::filesystem::remove(tempFile);
}

TEST_CASE("Settings - controller measurements", "[settings]") {
    const std::string file = "controller_settings_test_" + std::to_string(time(nullptr)) + ".xml";

    const auto store_one_measurement = [&file]() {
        settings<controller_settings> s(file, false);
        s().workerInfo.worker_joined("w1");
        s().workerInfo.worker_reported_cores("w1", 4);
        s().workerInfo.worker_started_work("w1");
        s().workerInfo.worker_finished_work("w1", 320, "DE", 100, 4, 50);
        s.save();
    };

    SECTION("a measurement survives a round trip through the file") {
        store_one_measurement();

        settings<controller_settings> s(file, false);
        const auto info = s().workerInfo.get_worker_info("w1");
        REQUIRE(info.has_value());
        REQUIRE(info->totalStats.samples == 1);
    }

    SECTION("measurements from an older version are discarded, not fatal") {
        store_one_measurement();

        // Only the stored class version decides, so the fields themselves are never reached
        std::string content;
        {
            std::ifstream in{file};
            content.assign(std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{});
        }
        const std::string current = "version=\"3\"";
        const auto at = content.find(current);
        REQUIRE(at != std::string::npos);
        content.replace(at, current.size(), "version=\"2\"");
        {
            std::ofstream out{file, std::ios::trunc};
            out << content;
        }

        settings<controller_settings> s(file, false);
        REQUIRE(!s().workerInfo.get_worker_info("w1").has_value());
    }

    std::filesystem::remove(file);
}
