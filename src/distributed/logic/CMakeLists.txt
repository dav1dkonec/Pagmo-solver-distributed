add_library(distributed_logic
        distributed_island.cpp
        distributed_controller.cpp
        distributed_worker.cpp
        distributed_solver.cpp
        work_plan.cpp
        worker_info_repository.cpp
        worker_settings.cpp
        controller_settings.cpp
)

target_include_directories(distributed_logic PUBLIC include)

target_link_libraries(distributed_logic distributed_messages distributed_sockets distributed_utils distributed_dynamic distributed_discovery)
target_link_libraries(distributed_logic cppzmq libzmq)
target_link_libraries(distributed_logic pagmo)