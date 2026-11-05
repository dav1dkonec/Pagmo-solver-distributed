#include <catch2/catch_test_macros.hpp>
#include "vector_serialize.h"
#include "vector_deserialize.h"
#include "MsgType.h"
#include "pagmo/algorithms/de.hpp"
#include "pagmo/algorithms/pso.hpp"
#include "pagmo/problems/schwefel.hpp"


TEST_CASE("Serialization - work_container", "[serialize]")
{
    pagmo::algorithm algo(pagmo::de(100));
    pagmo::problem prob(pagmo::schwefel(5));
    pagmo::population pop(prob, 20);
    
    SECTION("round-trip serialization") {
        work_container original(algo, pop, "worker_1", 3);
        
        auto serialized = vector_serialize<work_container>(original);
        REQUIRE(!serialized.empty());
        
        auto deserialized = vector_deserialize<work_container>(serialized);
        REQUIRE(deserialized.preferredWorkerId == "worker_1");
        REQUIRE(deserialized.cycleCount == 3);
        REQUIRE(deserialized.pop.size() == pop.size());
    }
    
    SECTION("default constructor fields") {
        work_container container;
        auto serialized = vector_serialize<work_container>(container);
        auto deserialized = vector_deserialize<work_container>(serialized);
        
        REQUIRE(deserialized.preferredWorkerId == "");
        REQUIRE(deserialized.cycleCount == 0);
    }
    
    SECTION("different algorithms") {
        pagmo::algorithm de(pagmo::de(50));
        pagmo::algorithm pso(pagmo::pso(50));
        
        work_container c1(de, pop, "", 1);
        work_container c2(pso, pop, "", 1);
        
        auto s1 = vector_serialize<work_container>(c1);
        auto s2 = vector_serialize<work_container>(c2);
        
        REQUIRE(!s1.empty());
        REQUIRE(!s2.empty());
    }
}

TEST_CASE("Serialization - get_dll_request", "[serialize]") {
    SECTION("round-trip") {
        get_dll_request original("myudp.dll", "darwin-arm64");
        
        auto serialized = vector_serialize<get_dll_request>(original);
        REQUIRE(!serialized.empty());
        
        auto deserialized = vector_deserialize<get_dll_request>(serialized);
        REQUIRE(deserialized.dll_name == "myudp.dll");
        REQUIRE(deserialized.platform == "darwin-arm64");
    }
    
    SECTION("various filenames") {
        std::vector<std::string> names = {
            "simple.dll",
            "path/to/file.dll",
            "special_chars_123.dll",
            "a.b.c.dll"
        };
        
        for(const auto& name : names) {
            get_dll_request req(name, "linux-arm64");
            auto s = vector_serialize<get_dll_request>(req);
            auto d = vector_deserialize<get_dll_request>(s);
            REQUIRE(d.dll_name == name);
        }
    }
}

TEST_CASE("Serialization - dll_binary_container", "[serialize]") {
    SECTION("with file data") {
        std::vector<std::byte> fileData(100);
        for(size_t i = 0; i < fileData.size(); ++i) {
            fileData[i] = std::byte(i % 256);
        }
        
        dll_binary_container original("lib.dll", fileData);
        
        auto serialized = vector_serialize<dll_binary_container>(original);
        REQUIRE(!serialized.empty());
        
        auto deserialized = vector_deserialize<dll_binary_container>(serialized);
        REQUIRE(deserialized.dll_name == "lib.dll");
        REQUIRE(deserialized.dll_file.has_value());
        REQUIRE(deserialized.dll_file.value() == fileData);
    }
    
    SECTION("without file data (nullopt)") {
        dll_binary_container original("missing.dll", std::nullopt);
        
        auto serialized = vector_serialize<dll_binary_container>(original);
        auto deserialized = vector_deserialize<dll_binary_container>(serialized);
        
        REQUIRE(deserialized.dll_name == "missing.dll");
        REQUIRE(!deserialized.dll_file.has_value());
    }
    
    SECTION("large file data") {
        std::vector<std::byte> largeFile(10000);
        for(size_t i = 0; i < largeFile.size(); ++i) {
            largeFile[i] = std::byte((i * 7) % 256);
        }
        
        dll_binary_container original("large.dll", largeFile);
        auto serialized = vector_serialize<dll_binary_container>(original);
        auto deserialized = vector_deserialize<dll_binary_container>(serialized);
        
        REQUIRE(deserialized.dll_file.value().size() == largeFile.size());
    }
}

TEST_CASE("Serialization - error cases", "[serialize]") {
    SECTION("empty buffer throws") {
        std::vector<std::byte> empty;
        REQUIRE_THROWS_AS(vector_deserialize<work_container>(empty), std::runtime_error);
    }
    
    SECTION("corrupted data throws") {
        std::vector<std::byte> corrupted{std::byte(0xFF), std::byte(0xFF)};
        REQUIRE_THROWS(vector_deserialize<work_container>(corrupted));
    }
}
