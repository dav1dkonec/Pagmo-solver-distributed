#pragma once
#include <iostream>
#include <memory>
#include <any>

#include "udp_base.h"
#include "pagmo/types.hpp"
#include <pagmo/s11n.hpp>
#include <pagmo/detail/s11n_wrappers.hpp>
#include <boost/serialization/shared_ptr.hpp>

#include "logger_init.h"
#include "udp_registry.h"

class udp_dll_wrapper
{
    // The udp contained within this shared_ptr must be cloned each time we copy construct udp_dll_wrapper.
    // Using shared_ptr appears to be the best option, unique_ptr would needlessly complicate the code.
    std::shared_ptr<udp_base> _udpPtr{};

    std::string _libFileName;

    // This exists so worker can see if it has the same version of this file or no
    // Assigned value only in the "save" (serialization) function, as we don't use it anywhere else
    std::string _libFileHash;
public:
    //#####################################################################################
    //# PAGMO UDP public functions
    //#####################################################################################
    pagmo::vector_double fitness(const pagmo::vector_double& dv) const;
    std::pair<pagmo::vector_double, pagmo::vector_double> get_bounds() const;
    pagmo::vector_double::size_type get_nec() const;
    pagmo::vector_double::size_type get_nic() const;
    pagmo::vector_double::size_type get_nobj() const;
    pagmo::vector_double::size_type get_nix() const;
    pagmo::vector_double batch_fitness(const pagmo::vector_double& v) const;
    bool has_batch_fitness() const;
    bool has_gradient() const;
    pagmo::vector_double gradient(const pagmo::vector_double& v) const;
    bool has_gradient_sparsity() const;
    pagmo::sparsity_pattern gradient_sparsity() const;
    bool has_hessians() const;
    std::vector<pagmo::vector_double> hessians(const pagmo::vector_double& v) const;
    bool has_hessians_sparsity() const;
    std::vector<pagmo::sparsity_pattern> hessians_sparsity() const;
    bool has_set_seed() const;
    void set_seed(unsigned s);
    std::string get_name() const;
    std::string get_extra_info() const;
    pagmo::thread_safety get_thread_safety() const;

    // This constructor needs to exist or this class won't be recognized as UDP by Pagmo
    udp_dll_wrapper() = default;

    //#####################################################################################
    //# OTHER DLL WRAPPER public functions
    //#####################################################################################

    explicit udp_dll_wrapper(const std::string& lib_file_name, const std::any& lib_object_params = std::any());

    [[nodiscard]] virtual std::string get_lib_file_name() const;

    // Custom copy constructor, ensuring that the UDP itself is cloned, not just the pointer being copied
    udp_dll_wrapper(const udp_dll_wrapper& other);

    udp_dll_wrapper& operator=(const udp_dll_wrapper& other);

    virtual ~udp_dll_wrapper() = default;
private:
    //#####################################################################################
    //# BOOST SERIALIZE
    //#####################################################################################

    friend class boost::serialization::access;

    template <typename Archive>
    void save(Archive& ar, unsigned) const
    {
        const auto hash = udp_registry::get().get_lib_file_hash(_libFileName);
        // Put it into a temporary var, we can't modify the field itself because this function is const
        const std::string libFileHash = hash.has_value() ? hash.value() : _libFileHash;

        pagmo::detail::to_archive(ar, libFileHash, _libFileName, _udpPtr);
        dist_logger()->trace("udp_dll_wrapper successfully saved");
    }

    template <typename Archive>
    void load(Archive& ar, unsigned)
    {
        try
            {
            // 1) Deserialize _libFileHash and _libFileName first
            ar >> _libFileHash;
            ar >> _libFileName;

            // 2) Attempt to load the dynamic library of this UDP
            udp_registry::get().initialize_udp(_libFileName, _libFileHash);

            // 3) Deserialize UDP, lib containing the UDP class should now be available in the address space of this process
            ar >> _udpPtr;

            dist_logger()->trace("udp_dll_wrapper successfully loaded");
        }
        catch (const std::exception& e)
        {
            std::cerr << e.what() << std::endl;

            *this = udp_dll_wrapper{};
            throw;
        }
    }

    BOOST_SERIALIZATION_SPLIT_MEMBER()
};
