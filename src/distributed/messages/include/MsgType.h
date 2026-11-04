#pragma once

#include "pagmo/algorithm.hpp"
#include "pagmo/population.hpp"
#include <pagmo/s11n.hpp>
#include <pagmo/detail/s11n_wrappers.hpp>
#include <utility>
#include <optional>
#include <cstdint>

#include "pagmo/algorithms/nsga2.hpp"

// Boost 1.83, still shipped by distributions, serializes boost::optional but not std::optional.
// save and load must read the same item count: an archive is a stream.
template <typename Archive, typename T>
void save_optional(Archive& ar, const std::optional<T>& value)
{
    const bool has_value = value.has_value();
    pagmo::detail::to_archive(ar, has_value);

    if (has_value)
    {
        pagmo::detail::to_archive(ar, *value);
    }
}

template <typename Archive, typename T>
void load_optional(Archive& ar, std::optional<T>& value)
{
    bool has_value = false;
    pagmo::detail::from_archive(ar, has_value);

    if (has_value)
    {
        T payload;
        pagmo::detail::from_archive(ar, payload);
        value = std::move(payload);
    }
    else
    {
        value.reset();
    }
}

// ZeroMQ itself sends WORKER_JOIN and WORKER_LEAVE as a single frame, so receive() reads no
// payload for them. Never reuse them for our own messages, it would desynchronize the socket.
enum class MsgType
{
    WORKER_JOIN,
    WORKER_LEAVE,
    ALLOCATE_WORK,
    WORK_RESULTS,
    GET_DLL,
    DLL_BINARY,
    GET_BLOB,
    BLOB_BINARY,
    WORK_FAILED
};

// Requests a read-only blob shared by every work unit, so it travels once per worker.
class get_blob_request
{
public:
    std::string blob_key;

    get_blob_request(const std::string& blob_key)
        : blob_key(blob_key)
    {
    }

    get_blob_request() = default;

private:
    friend class boost::serialization::access;

    template <typename Archive>
    void save(Archive& ar, unsigned) const
    {
        pagmo::detail::to_archive(ar, blob_key);
    }

    template <typename Archive>
    void load(Archive& ar, unsigned)
    {
        pagmo::detail::from_archive(ar, blob_key);
    }

    BOOST_SERIALIZATION_SPLIT_MEMBER()
};

// Reply to get_blob_request; an empty optional means the controller does not have the key.
class blob_binary_container
{
public:
    std::string blob_key;
    std::optional<std::vector<std::byte>> blob_data;

    blob_binary_container(const std::string& blob_key, const std::optional<std::vector<std::byte>>& blob_data)
        : blob_key(blob_key),
          blob_data(blob_data)
    {
    }

    blob_binary_container() = default;

private:
    friend class boost::serialization::access;

    template <typename Archive>
    void save(Archive& ar, unsigned) const
    {
        pagmo::detail::to_archive(ar, blob_key);
        save_optional(ar, blob_data);
    }

    template <typename Archive>
    void load(Archive& ar, unsigned)
    {
        pagmo::detail::from_archive(ar, blob_key);
        load_optional(ar, blob_data);
    }

    BOOST_SERIALIZATION_SPLIT_MEMBER()
};

class work_container
{
public:
    pagmo::algorithm algo;
    pagmo::population pop;

    // This allows the controller to assign this to a specific worker
    std::string preferredWorkerId;
    // Archipelago evolution cycle count for the worker
    size_t cycleCount{};
    // Time the worker spent computing, without transfer or serialization.
    // Set on the way back, left at 0 on the way out.
    uint64_t evolveTimeMs{};
    // Individuals the worker really evolved. Can exceed pop.size(), because an archipelago
    // worker pads each island up to its minimum size. Also only set on the way back.
    size_t evolvedPopulation{};

    work_container(pagmo::algorithm algo, pagmo::population pop, std::string preferredWorker = "",
                   const size_t cycleCount = 1) :
        algo(std::move(algo)),
        pop(std::move(pop)),
        preferredWorkerId(std::move(preferredWorker)),
        cycleCount(cycleCount)
    {
    }

    work_container() = default;

private:
    //####################################
    //# BOOST SERIALIZE
    //####################################

    friend class boost::serialization::access;

    template <typename Archive>
    void save(Archive& ar, unsigned) const
    {
        pagmo::detail::to_archive(ar, algo, pop, preferredWorkerId, cycleCount, evolveTimeMs, evolvedPopulation);
    }

    template <typename Archive>
    void load(Archive& ar, unsigned)
    {
        pagmo::detail::from_archive(ar, algo, pop, preferredWorkerId, cycleCount, evolveTimeMs, evolvedPopulation);
    }

    BOOST_SERIALIZATION_SPLIT_MEMBER()
};


// The platform tag says which build of the library the sender can load, see platform_tag()
class get_dll_request
{
public:
    std::string dll_name;
    std::string platform;

    get_dll_request(const std::string& dll_name, const std::string& platform)
        : dll_name(dll_name),
          platform(platform)
    {
    }

    get_dll_request() = default;

private:
    //####################################
    //# BOOST SERIALIZE
    //####################################

    friend class boost::serialization::access;

    template <typename Archive>
    void save(Archive& ar, unsigned) const
    {
        pagmo::detail::to_archive(ar, dll_name, platform);
    }

    template <typename Archive>
    void load(Archive& ar, unsigned)
    {
        pagmo::detail::from_archive(ar, dll_name, platform);
    }

    BOOST_SERIALIZATION_SPLIT_MEMBER()
};

class dll_binary_container
{
public:
    std::string dll_name;
    std::optional<std::vector<std::byte>> dll_file;

    dll_binary_container(const std::string& dll_name, const std::optional<std::vector<std::byte>>& dll_file)
        : dll_name(dll_name),
          dll_file(dll_file)
    {
    }

    dll_binary_container() = default;

private:
    //####################################
    //# BOOST SERIALIZE
    //####################################

    friend class boost::serialization::access;

    template <typename Archive>
    void save(Archive& ar, unsigned) const
    {
        pagmo::detail::to_archive(ar, dll_name);
        save_optional(ar, dll_file);
    }

    template <typename Archive>
    void load(Archive& ar, unsigned)
    {
        pagmo::detail::from_archive(ar, dll_name);
        load_optional(ar, dll_file);
    }

    BOOST_SERIALIZATION_SPLIT_MEMBER()
};
