#pragma once
#include <any>
#include <cstddef>
#include <string>
#include <vector>
#include "pagmo/types.hpp"
#include <pagmo/s11n.hpp>
#include "pagmo/threading.hpp"

class udp_base
{
public:
    //#####################################################################################
    //# Capability check: allows pagmo (via wrapper) to check if any of these functions are implemented
    //#####################################################################################
    enum class capability
    {
        has_batch_fitness,
        has_gradient,
        has_gradient_sparsity,
        has_hessians,
        has_hessians_sparsity,
        has_set_seed
    };

    /**
     * Override this function if you implement any of the functions specified in the capability enum
     * @param c one of the functions specified in the "capability" enum
     * @return true if our class implements this function, false otherwise
     */
    virtual bool has_capability(capability c) const { return false; }

    //#####################################################################################
    //# Mandatory UDP functions
    //#####################################################################################

    virtual pagmo::vector_double fitness(const pagmo::vector_double& dv) const = 0;

    virtual std::pair<pagmo::vector_double, pagmo::vector_double> get_bounds() const = 0;

    //#####################################################################################
    //# Optional UDP functions with default values (no "has_" function check)
    //#####################################################################################

    virtual pagmo::vector_double::size_type get_nobj() const { return 1; }
    virtual pagmo::vector_double::size_type get_nec() const { return 0; }
    virtual pagmo::vector_double::size_type get_nic() const { return 0; }
    virtual pagmo::vector_double::size_type get_nix() const { return 0; }
    virtual std::string get_extra_info() const { return "No extra info."; }
    virtual pagmo::thread_safety get_thread_safety() const { return pagmo::thread_safety::basic; }
    virtual std::string get_name() const { return "Name of this UDP is not specified."; }

    //#####################################################################################
    //# Optional UDP functions with "has_" check (override has_capability accordingly)
    //#####################################################################################

    virtual pagmo::vector_double batch_fitness(const pagmo::vector_double& dv) const
    {
        throw std::runtime_error("batch_fitness() is not implemented by this UDP");
    }

    virtual pagmo::vector_double gradient(const pagmo::vector_double& dv) const
    {
        throw std::runtime_error("gradient() is not implemented by this UDP");
    }

    virtual pagmo::sparsity_pattern gradient_sparsity() const
    {
        throw std::runtime_error("gradient_sparsity() is not implemented by this UDP");
    }

    virtual std::vector<pagmo::vector_double> hessians(const pagmo::vector_double& dv) const
    {
        throw std::runtime_error("hessians() is not implemented by this UDP");
    }

    virtual std::vector<pagmo::sparsity_pattern> hessians_sparsity() const
    {
        throw std::runtime_error("hessians_sparsity() is not implemented by this UDP");
    }

    virtual void set_seed(unsigned seed)
    {
        throw std::runtime_error("set_seed() is not implemented by this UDP");
    }

    //#####################################################################################
    //# Optional large shared payload
    //#####################################################################################

    /**
     * Key of a blob this UDP needs but does not carry in its own serialized form. Returning a
     * non-empty key lets a payload shared by every work unit travel once per worker instead of
     * inside every work_container. An empty key, the default, means the UDP is self-contained.
     */
    virtual std::string required_blob_key() const { return {}; }

    /**
     * True once the UDP holds the blob named by required_blob_key().
     */
    virtual bool has_required_blob() const { return true; }

    /**
     * Hands over the blob. Called before the first fitness() whenever has_required_blob() is
     * false; the UDP decodes it into whatever form it needs.
     */
    virtual void accept_blob(const std::vector<std::byte>&) {}


    //#####################################################################################
    //# Other
    //#####################################################################################

    virtual std::string get_lib_file_name() = 0;

    virtual ~udp_base() = default;
};

BOOST_SERIALIZATION_ASSUME_ABSTRACT(udp_base)
