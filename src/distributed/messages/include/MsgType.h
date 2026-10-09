#pragma once

#include "pagmo/algorithm.hpp"
#include "pagmo/population.hpp"
#include <pagmo/s11n.hpp>
#include <pagmo/detail/s11n_wrappers.hpp>
#include <utility>
#include <optional>

#include "pagmo/algorithms/nsga2.hpp"

enum class MsgType
{
    WORKER_JOIN,
    WORKER_LEAVE,
    ALLOCATE_WORK,
    WORK_RESULTS,
    GET_DLL,
    DLL_BINARY
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
        pagmo::detail::to_archive(ar, algo, pop, preferredWorkerId, cycleCount);
    }

    template <typename Archive>
    void load(Archive& ar, unsigned)
    {
        pagmo::detail::from_archive(ar, algo, pop, preferredWorkerId, cycleCount);
    }

    BOOST_SERIALIZATION_SPLIT_MEMBER()
};


class get_dll_request
{
public:
    std::string dll_name;

    get_dll_request(const std::string& dll_name)
        : dll_name(dll_name)
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
        pagmo::detail::to_archive(ar, dll_name);
    }

    template <typename Archive>
    void load(Archive& ar, unsigned)
    {
        pagmo::detail::from_archive(ar, dll_name);
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
        // Boost only gained std::optional support in
        // recent versions; Debian 13 ships 1.83,
        // whose serialization/optional.hpp covers
        // boost::optional only. Write the presence
        // flag and the payload separately so no Boost
        // version is required to know about
        // std::optional.
        const bool has_file = dll_file.has_value();
        pagmo::detail::to_archive(ar, dll_name, has_file);

        if (has_file)
        {
            pagmo::detail::to_archive(ar, *dll_file);
        }
    }

    template <typename Archive>
    void load(Archive& ar, unsigned)
    {
        // Must mirror save() exactly: an archive is a
        // stream, so a mismatched read count
        // corrupts everything deserialized afterwards
        bool has_file = false;
        pagmo::detail::from_archive(ar, dll_name, has_file);

        if (has_file)
        {
            std::vector<std::byte> file;
            pagmo::detail::from_archive(ar, file);
            dll_file = std::move(file);
        }
        else
        {
            dll_file.reset();
        }
    }

    BOOST_SERIALIZATION_SPLIT_MEMBER()
};
