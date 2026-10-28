include(CTest)
enable_testing()
include(Catch)

if(NOT DEFINED DISTRIBUTED_SOLVER_RUNTIME_OUTPUT_DIR)
    set(DISTRIBUTED_SOLVER_RUNTIME_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin")
endif()

add_executable(unit_tests_distributed_solver
    # Phase 1: Foundation Data Structures
    discovery/lru_cache_test.cpp
    discovery/hash_cache_test.cpp
    
    # Phase 2: Serialization & Messages
    messages/serialization_test.cpp
    
    # Phase 3: Utilities & Helpers
    logic/population_tools_test.cpp
    logic/settings_test.cpp
    utils/platform_utils_test.cpp
    utils/logger_init_test.cpp
    
    # Phase 4: Core Domain Logic
    logic/worker_info_repository_test.cpp

    # Phase 5: Discovery Layer
    discovery/udp_registry_test.cpp
    
    # Phase 6: Socket Layer
    sockets/socket_roundtrip_test.cpp

)

target_link_libraries(unit_tests_distributed_solver PRIVATE
    distributed_discovery
    distributed_logic
    distributed_messages
    distributed_sockets
    distributed_utils
    distributed_dynamic
    Catch2::Catch2WithMain
    pagmo
    cppzmq
    libzmq
)

target_include_directories(unit_tests_distributed_solver PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

# So the exe is besides all other required DLLs
set_target_properties(unit_tests_distributed_solver PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${DISTRIBUTED_SOLVER_RUNTIME_OUTPUT_DIR}"
)

catch_discover_tests(unit_tests_distributed_solver
        WORKING_DIRECTORY "${DISTRIBUTED_SOLVER_RUNTIME_OUTPUT_DIR}"
)
