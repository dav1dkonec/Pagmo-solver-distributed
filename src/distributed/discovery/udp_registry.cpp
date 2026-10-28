#include "udp_registry.h"
#include "logger_init.h"

#include <filesystem>
#include <fstream>
#include <iostream>

//#####################################################################################
//# Public member functions
//#####################################################################################

void udp_registry::initialize_udp(const std::string& name, const std::optional<std::string>& libFileHash)
{
    std::scoped_lock lock(_registryMutex);

    dist_logger()->trace("initializing udp library: {}", name);

    // If a file hash was provided, we need to check if we have the same file version
    // (udp_dll_wrapper calls this each time it deserializes itself - i.e. each time worker receives data)
    if (libFileHash.has_value())
    {
        // This function uses get_lib_as_file() to get the file and then compute hash,
        // which means that it'll first try to get it from a local cache, and only then
        // request the file from the controller
        const auto hash = get_lib_file_hash(name);
        if (!hash.has_value())
        {
            // This should not happen - if controller sent us this file name, it should have the file ready
            throw std::runtime_error("Error: cannot initialize udp: " + name + ", this dynamic library was not found.");
        }

        if (hash != libFileHash.value())
        {
            // The peer's copy is authoritative, but a binary built for another platform
            // cannot be loaded here. Only discard a working library once its replacement works.
            if (!_try_replace_lib_from_provider(name, libFileHash.value()))
            {
                // No provider means this node is the controller, which serves libraries
                // instead of fetching them - expected, not a problem.
                const auto level = _udp_provider ? spdlog::level::warn : spdlog::level::debug;
                dist_logger()->log(level,
                            "Keeping the local build of '{}': the peer's artifact (hash {}) is not "
                            "loadable on this platform, local hash is {}",
                            name, libFileHash.value(), hash.value());
            }
        }
    }

    // If this lib is already loaded in this process, do nothing
    if (_lib_loaders.contains(name))
    {
        return;
    }

    // Check if the lib exists in local cache, if yes, load it
    if (_is_lib_in_cache(name))
    {
        _load_lib(name);
        return;
    }

    // If lib is not loaded or present in cache, try to get it via provider
    if (_udp_provider)
    {
        const auto libFile = _udp_provider(name);
        if (libFile.has_value())
        {
            // The lib file was found and was passed to us as vector of bytes
            _save_lib_into_fs(name, libFile.value());
            _load_lib(name);
            return;
        }
    }

    throw std::runtime_error("Error: cannot initialize udp: " + name + ", this dynamic library was not found.");
}

std::shared_ptr<udp_base> udp_registry::construct_udp(const std::string& name, const std::any& params)
{
    // Anything calling this needs to first acquire the mutex, without this there could be a race condition on if (_udp_provider)
    std::scoped_lock lock(_registryMutex);

    dist_logger()->trace("constructing an instance of: {}", name);

    // If this lib is already loaded, simply construct a new object
    if (_lib_loaders.contains(name))
    {
        return _lib_loaders.at(name).get_instance(params);
    }

    // Check if the lib exists in local cache, if yes, load it and return instance
    if (_is_lib_in_cache(name))
    {
        _load_lib(name);
        return _lib_loaders.at(name).get_instance(params);
    }

    // If lib is not loaded or present in cache, try to get it via provider
    if (_udp_provider)
    {
        const auto libFile = _udp_provider(name);
        if (libFile.has_value())
        {
            // The lib file was found and was passed to us as vector of bytes
            _save_lib_into_fs(name, libFile.value());
            _load_lib(name);
            // The loader should now be ready to construct an instance
            return _lib_loaders.at(name).get_instance(params);
        }
    }

    throw std::runtime_error("Error: cannot construct udp: " + name + ", this dynamic library was not found.");
}

std::shared_ptr<udp_base> udp_registry::clone_udp(const std::shared_ptr<udp_base>& other)
{
    std::scoped_lock lock(_registryMutex);

    //glog::get().trace("cloning an instance of: {}", other->get_lib_file_name());

    // We get the lib_loader associated with this UDP and call its clone function
    const std::string libName = other->get_lib_file_name();
    if (_lib_loaders.contains(libName))
    {
        return _lib_loaders.at(libName).clone_instance(other);
    }

    throw std::runtime_error("Error: cannot clone udp: " + libName + ", this dynamic library was not found.");
}

void udp_registry::register_udp_provider(const udp_provider& providerFunc)
{
    // This will prevent provider from changing if any thread is executing inside construct_udp()
    std::scoped_lock lock(_registryMutex);

    dist_logger()->debug("udp_provider has been registered in udp_registry");

    _udp_provider = providerFunc;
}

void udp_registry::set_local_cache_dir(const std::filesystem::path& directory)
{
    std::scoped_lock lock(_registryMutex);

    dist_logger()->debug("cache has been set to: {}", directory.string());

    _local_cache = directory;
}

void udp_registry::use_in_memory_cache(bool value)
{
    std::scoped_lock lock(_registryMutex);

    if (!value)
    {
        _libFilesBuffer.clear();
    }
    _use_in_memory_cache = value;
}

std::optional<std::vector<std::byte>> udp_registry::get_lib_as_file(const std::string& libName)
{
    std::scoped_lock lock(_registryMutex);

    dist_logger()->trace("get_lib_as_file: {}", libName);

    if (!_is_lib_in_cache(libName))
    {
        // If the file doesn't exist locally, we can try using udp provider
        if (_udp_provider)
        {
            const auto libFile = _udp_provider(libName);

            // Save it into the fs too to prevent repeated fetching from controller
            if (libFile.has_value())
            {
                _save_lib_into_fs(libName, libFile.value());
            }

            return libFile;
        }
        return std::nullopt;
    }

    const std::filesystem::path libPath = _get_lib_path(libName);

    // Check our LRU buffer if this file isn't already cached in-memory
    if (_use_in_memory_cache && _libFilesBuffer.contains(libPath.string()))
    {
        return _libFilesBuffer.get(libPath.string());
    }

    // Load file from FS
    try
    {
        std::ifstream fStream{libPath, std::ios::binary};
        if (!fStream)
        {
            return std::nullopt;
        }

        fStream.seekg(0, std::ios::end);
        const auto size = fStream.tellg();
        if (size < 0)
        {
            throw std::runtime_error("Error: cannot read lib file at: " + libPath.string());
        }
        fStream.seekg(0, std::ios::beg);

        std::vector<std::byte> fileContent(size);

        fStream.read(reinterpret_cast<char*>(fileContent.data()), size);

        if (_use_in_memory_cache)
        {
            _libFilesBuffer.put(libPath.string(), fileContent);
        }

        return fileContent;
    }
    catch (const std::exception& e)
    {
        dist_logger()->warn("Warning: error when attempting to load file: {}Err msg:{} Proceeding as if it doesn't exist.", libPath.string(), e.what());
        return std::nullopt;
    }
    catch (...)
    {
        dist_logger()->warn("Warning: unknown error when attempting to load file: {} Proceeding as if it doesn't exist.", libPath.string());
        return std::nullopt;
    }
}

std::optional<std::string> udp_registry::get_lib_file_hash(const std::string& libName)
{
    const auto file = get_lib_as_file(libName);

    // Return nullopt if lib file doesn't exist
    if (!file.has_value())
    {
        return std::nullopt;
    }

    // Use cached value if possible
    if (_hash_cache.has_file(libName))
    {
        return _hash_cache.get_file_hash(libName);
    }

    // Compute hash and return it
    return _hash_cache.hash_file(libName, file.value());
}

//#####################################################################################
//# Private member functions
//#####################################################################################

void udp_registry::_save_lib_into_fs(const std::string& libName, const std::vector<std::byte>& libFile)
{
    dist_logger()->trace("udp_registry saving lib file: {}", libName);

    _write_lib_file(_get_lib_path(libName), libFile);
}

void udp_registry::_write_lib_file(const std::filesystem::path& libPath, const std::vector<std::byte>& libFile)
{
    // We're assuming the higher level directories exist
    if (!std::filesystem::exists(_local_cache))
        std::filesystem::create_directory(_local_cache);

    std::ofstream file(libPath, std::ios::binary);
    if (!file)
        throw std::runtime_error("Error: Failed to open file " + libPath.string());

    file.write(reinterpret_cast<const char*>(libFile.data()), libFile.size());

    if (!file)
        throw std::runtime_error("Error: Failed write to file " + libPath.string());
}

bool udp_registry::_try_replace_lib_from_provider(const std::string& libName, const std::string& remoteHash)
{
    // initialize_udp runs on every deserialization, so without this check we would re-fetch the
    // same unusable artifact for every message this worker receives
    if (_rejectedRemoteHashes.contains(remoteHash))
        return false;

    if (!_udp_provider)
        return false;

    const auto libFile = _udp_provider(libName);
    if (!libFile.has_value())
        return false;

    // Write the candidate next to the library and never over it, so that a failure leaves the
    // working copy untouched
    const std::filesystem::path candidate =
        _local_cache / std::string{libName + ".incoming" + portable_dll_extension()};

    _write_lib_file(candidate, libFile.value());

    if (!_is_loadable(candidate))
    {
        std::filesystem::remove(candidate);
        _rejectedRemoteHashes.insert(remoteHash);
        return false;
    }

    // The replacement works, so it's now safe to drop the old one
    _unload_lib(libName);
    _delete_lib_file(libName);
    std::filesystem::rename(candidate, _get_lib_path(libName));

    dist_logger()->debug("Replaced local '{}' with the controller's copy", libName);
    return true;
}

bool udp_registry::_is_loadable(const std::filesystem::path& libPath) const
{
    try
    {
        // open_lib() only dlopens; run_after_load() runs later, on first use.
        // dlopen does still run the library's static initializers.
        lib_loader<udp_base> probe{libPath.string()};
        probe.open_lib();
        return true; // probe's destructor closes the handle again
    }
    catch (const std::exception& e)
    {
        dist_logger()->debug("Candidate library {} is not loadable here: {}", libPath.string(), e.what());
        return false;
    }
}

void udp_registry::_load_lib(const std::string& libName)
{
    dist_logger()->trace("udp_registry loading lib file: {}", libName);

    _lib_loaders.insert({libName, lib_loader<udp_base>{_get_lib_path(libName).string()}});
    _lib_loaders.at(libName).open_lib();
}

bool udp_registry::_is_lib_in_cache(const std::string& libName) const
{
    return std::filesystem::exists(_get_lib_path(libName));
}

std::filesystem::path udp_registry::_get_lib_path(const std::string& libName) const
{
    return _local_cache / std::string{libName + portable_dll_extension()};
}

void udp_registry::_unload_lib(const std::string& libName)
{
    if (!_lib_loaders.contains(libName))
        return;

    dist_logger()->trace("udp_registry unloading lib: {}", libName);

    try
    {
        _lib_loaders.at(libName).close_lib();
    }
    catch (const std::exception& e)
    {
        dist_logger()->error("Error: failed to unload lib: {} Err msg: {}", libName, e.what());
        throw;
    }
    catch (...)
    {
        dist_logger()->error("Error: unknown failure unloading lib: {}", libName);
        throw;
    }

    _lib_loaders.erase(libName);
}

void udp_registry::_delete_lib_file(const std::string& libName)
{
    dist_logger()->trace("udp_registry deleting lib file: {}", libName);

    const std::filesystem::path libPath = _get_lib_path(libName);

    // Remove from in-memory cache
    if (_use_in_memory_cache)
    {
        _libFilesBuffer.erase(libPath.string());
    }

    // Remove from hash cache
    if (_hash_cache.has_file(libName))
    {
        _hash_cache.erase(libName);
    }

    // Remove from fs
    try
    {
        if (std::filesystem::exists(libPath))
        {
            if (!std::filesystem::remove(libPath))
            {
                dist_logger()->error("Error: failed to delete file: {}", libPath.string());
                throw std::runtime_error("Failed to delete file: " + libPath.string());
            }
        }
    }
    catch (const std::exception& e)
    {
        dist_logger()->error("Error: exception deleting file: {} Err msg: {}", libPath.string(), e.what());
        throw;
    }
    catch (...)
    {
        dist_logger()->error("Error: unknown exception deleting file: {}", libPath.string());
        throw;
    }
}
