#include "udp_dll_wrapper.h"
#include "blob_registry.h"
#include "logger_init.h"

#include <pagmo/problem.hpp>

//#####################################################################################
//# PAGMO UDP public functions
//#####################################################################################

void udp_dll_wrapper::_ensure_required_blob() const
{
    // Pagmo copies the problem per island and per thread, so several copies may arrive here;
    // the registry caches, so only the first one reaches the controller.
    if (!_udpPtr || _udpPtr->has_required_blob())
    {
        return;
    }

    const std::string key = _udpPtr->required_blob_key();
    if (key.empty())
    {
        return;
    }

    const auto blob = blob_registry::get().fetch(key);
    if (!blob)
    {
        throw std::runtime_error("Cannot obtain blob " + key + " required by " + _libFileName);
    }

    _udpPtr->accept_blob(*blob);
}

pagmo::vector_double udp_dll_wrapper::fitness(const pagmo::vector_double& dv) const
{
    _ensure_required_blob();
    return _udpPtr->fitness(dv);
}

std::pair<pagmo::vector_double, pagmo::vector_double> udp_dll_wrapper::get_bounds() const
{
    return _udpPtr->get_bounds();
}

pagmo::vector_double::size_type udp_dll_wrapper::get_nec() const
{
    return _udpPtr->get_nec();
}

pagmo::vector_double::size_type udp_dll_wrapper::get_nic() const
{
    return _udpPtr->get_nic();
}

pagmo::vector_double::size_type udp_dll_wrapper::get_nobj() const
{
    return _udpPtr->get_nobj();
}

pagmo::vector_double::size_type udp_dll_wrapper::get_nix() const
{
    return _udpPtr->get_nix();
}

pagmo::vector_double udp_dll_wrapper::batch_fitness(const pagmo::vector_double& v) const
{
    return _udpPtr->batch_fitness(v);
}

bool udp_dll_wrapper::has_batch_fitness() const
{
    return _udpPtr->has_capability(udp_base::capability::has_batch_fitness);

}

bool udp_dll_wrapper::has_gradient() const
{
    return _udpPtr->has_capability(udp_base::capability::has_gradient);

}

pagmo::vector_double udp_dll_wrapper::gradient(const pagmo::vector_double& v) const
{
    return _udpPtr->gradient(v);
}

bool udp_dll_wrapper::has_gradient_sparsity() const
{
    return _udpPtr->has_capability(udp_base::capability::has_gradient_sparsity);

}

pagmo::sparsity_pattern udp_dll_wrapper::gradient_sparsity() const
{
    return _udpPtr->gradient_sparsity();
}

bool udp_dll_wrapper::has_hessians() const
{
    return _udpPtr->has_capability(udp_base::capability::has_hessians);
}

std::vector<pagmo::vector_double> udp_dll_wrapper::hessians(const pagmo::vector_double& v) const
{
    return _udpPtr->hessians(v);
}

bool udp_dll_wrapper::has_hessians_sparsity() const
{
    return _udpPtr->has_capability(udp_base::capability::has_hessians_sparsity);
}

std::vector<pagmo::sparsity_pattern> udp_dll_wrapper::hessians_sparsity() const
{
    return _udpPtr->hessians_sparsity();
}

bool udp_dll_wrapper::has_set_seed() const
{
    return _udpPtr->has_capability(udp_base::capability::has_set_seed);
}

void udp_dll_wrapper::set_seed(unsigned s)
{
    return _udpPtr->set_seed(s);
}

std::string udp_dll_wrapper::get_name() const
{
    return _udpPtr->get_name();
}

std::string udp_dll_wrapper::get_extra_info() const
{
    return _udpPtr->get_extra_info();
}

pagmo::thread_safety udp_dll_wrapper::get_thread_safety() const
{
    return _udpPtr->get_thread_safety();
}

//#####################################################################################
//# OTHER DLL WRAPPER public functions
//#####################################################################################

udp_dll_wrapper::udp_dll_wrapper(const std::string& lib_file_name, const std::any& lib_object_params) : _libFileName(lib_file_name)
{
    // This is called when the UDP is constructed manually.

    // construct_udp throws std::runtime_error if not found
    _udpPtr = udp_registry::get().construct_udp(_libFileName, lib_object_params);
}

std::string udp_dll_wrapper::get_lib_file_name() const
{
    return _libFileName;
}

udp_dll_wrapper::udp_dll_wrapper(const udp_dll_wrapper& other) : _libFileName(other._libFileName)
{
    // Possibly allow an empty object to exist (we expose the default constructor)
    if (!_libFileName.empty())
        _udpPtr = udp_registry::get().clone_udp(other._udpPtr);
}

udp_dll_wrapper& udp_dll_wrapper::operator=(const udp_dll_wrapper& other)
{
    if (this == &other)
        return *this;

    _libFileName = other._libFileName;

    if (!_libFileName.empty())
        _udpPtr = udp_registry::get().clone_udp(other._udpPtr);

    return *this;
}

BOOST_CLASS_EXPORT(udp_dll_wrapper)

// Without this it's impossible to serialize this class when used inside pagmo objects
BOOST_CLASS_EXPORT(pagmo::detail::prob_inner<udp_dll_wrapper>)
