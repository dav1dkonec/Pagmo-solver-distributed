cmake_minimum_required(VERSION 3.19.0)
project(dist_solver CXX C)
message(STATUS "Executing top-level CMakeLists.txt...")

set(CMAKE_CXX_STANDARD 20)

# These static libraries are also linked into shared modules, such as the UDP libraries of
# dependent projects. ELF accepts that only for position-independent objects.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Add this so spdlog can use utf-8
if (MSVC)
    add_compile_options(/utf-8)
endif()

# Allow the user to override where DLLs are placed
if(NOT DEFINED DISTRIBUTED_SOLVER_RUNTIME_OUTPUT_DIR)
    set(DISTRIBUTED_SOLVER_RUNTIME_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin")
endif()

message(STATUS "Distributed solver runtime output directory: ${DISTRIBUTED_SOLVER_RUNTIME_OUTPUT_DIR}")

#########################################################################################
# EIGEN 3
execute_process(
        COMMAND cmake -S . -B ./build/ -G ${CMAKE_GENERATOR}
        -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
        -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
        -DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/external/Eigen3
)
message(STATUS "> Eigen3 has been configured.")
#########################################################################################

#########################################################################################
# PAGMO

message(AUTHOR_WARNING "Make sure that you've installed Boost and Intel TBB before using the Pagmo library.")
message(AUTHOR_WARNING "Both Boost and Intel TBB must be in your env PATH variable (e.g. C:\\Program Files (x86)\\Intel\\oneAPI\\tbb\\2022.2\\bin) or else pagmo.dll won't load.'")

set(PAGMO_WITH_EIGEN3 ON)

# If we statically link to boost (default behavior on Windows), we run into an issue where there are two copies of Boost
# in memory, one copy belongs to main.exe and the second one belongs to pagmo.dll, Boost serialize internally uses singleton
# objects, those objects are in this case duplicated (separate for main.exe and pagmo.dll), which causes serialization to
# fail with runtime exception: "unregistered class exception". Below is an email from 2004 which describes this issue:
# > My suspicion is that the problem is caused by the serialization library
# > using a static library, rather than a DLL. If the BOOST_CLASS_EXPORT
# > commands updated a list contained in the static library then there would
# > be one instance of this list statically linked into my DLL, and another
# > in my EXE. Thus, the registration occurring in the DLL wouldn't affect
# > the list used in the EXE.
# (Source: https://listarchives.boost.org/Archives/boost/2004/03/62704.php)
# More about this: https://stackoverflow.com/questions/58769398/is-linking-two-dlls-to-the-same-static-library-and-then-linking-both-dlls-togeth
#set(PAGMO_BUILD_STATIC_LIBRARY ON)

set(Boost_USE_MULTITHREADED ON)
# ENSURE BOOST IS DYNAMICALLY LINKED (Works on Windows)
# Prevents the problem described above; pagmo.dll, main.exe and any UDP DLLs dynamically link to the same boost serialize DLL
set(Boost_USE_STATIC_LIBS OFF)
set(Boost_USE_STATIC_RUNTIME OFF)

add_subdirectory(./external/pagmo)


# This needs to be added or Pagmo won't compile with newest MSVC (14.44)
if(MSVC)
    target_compile_definitions(pagmo PRIVATE _SILENCE_ALL_MS_EXT_DEPRECATION_WARNINGS)
    target_compile_options(pagmo PRIVATE /W2 /EHsc)
endif()
#########################################################################################

# So the EXE find this dll
set_target_properties(pagmo
        PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${DISTRIBUTED_SOLVER_RUNTIME_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${DISTRIBUTED_SOLVER_RUNTIME_OUTPUT_DIR}"
)

#########################################################################################
# CATCH2 unit testing framework
if(NOT TARGET Catch2::Catch2 AND NOT TARGET Catch2::Catch2WithMain)
    add_subdirectory(external/Catch2)
endif()
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/external/Catch2/extras")
#########################################################################################

#########################################################################################
# libzmq
# IMPORTANT: CMake generation step ends with ninja error if this is not set to OFF
set(ENABLE_PRECOMPILED  OFF CACHE BOOL "" FORCE)
add_subdirectory(./external/libzmq)

# So the EXE find this dll
set_target_properties(libzmq
        PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${DISTRIBUTED_SOLVER_RUNTIME_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${DISTRIBUTED_SOLVER_RUNTIME_OUTPUT_DIR}"
)
#########################################################################################

#########################################################################################
# cppzmq
set(CPPZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(./external/cppzmq)
#########################################################################################

add_subdirectory(src)
# Perhaps add some if statement here to add tests conditionally
add_subdirectory(test)

# Pagmo deps:
# Boost (must be in env path)
# Intel TBB (must be in evn path)
# Eigen3