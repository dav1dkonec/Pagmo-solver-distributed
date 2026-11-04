#include "udp_registry.h"
#include "logger_init.h"

#include <algorithm>
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
            // Either the peer does not have the file, or its artifact does not load here
            throw std::runtime_error("Error: cannot initialize udp: " + name
                                     + ", no artifact loadable on this platform is available.");
        }

        if (hash != libFileHash.value())
        {
            // The peer's copy is authoritative, but a binary built for another platform
            // cannot be loaded here. Only discard a working library once its replacement works.
            if (!_install_lib_from_provider(name, libFileHash.value()))
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

    // Load a local copy if there is one, be it cached or this process' own build
    if (const auto localLib = _find_local_lib(name))
    {
        _load_lib(name, localLib.value());
        return;
    }

    // Nothing local to fall back on, so the peer's artifact has to be loadable here
    if (_install_lib_from_provider(name))
    {
        _load_lib(name, _get_lib_path(name));
        return;
    }

    throw std::runtime_error("Error: cannot initialize udp: " + name
                             + ", no artifact loadable on this platform is available.");
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

    // Load a local copy if there is one, be it cached or this process' own build
    if (const auto localLib = _find_local_lib(name))
    {
        _load_lib(name, localLib.value());
        return _lib_loaders.at(name).get_instance(params);
    }

    // Nothing local to fall back on, so the peer's artifact has to be loadable here
    if (_install_lib_from_provider(name))
    {
        _load_lib(name, _get_lib_path(name));
        return _lib_loaders.at(name).get_instance(params);
    }

    throw std::runtime_error("Error: cannot construct udp: " + name
                             + ", no artifact loadable on this platform is available.");
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

void udp_registry::add_lib_search_dir(const std::filesystem::path& directory)
{
    std::scoped_lock lock(_registryMutex);

    // Callers can run more than once per process, so keep the list free of duplicates
    if (std::find(_lib_search_dirs.begin(), _lib_search_dirs.end(), directory) != _lib_search_dirs.end())
        return;

    dist_logger()->debug("library search dir has been added: {}", directory.string());

    _lib_search_dirs.push_back(directory);
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

std::optional<std::vector<std::byte>> udp_registry::get_lib_as_file(const std::string& libName,
                                                                    const std::string& platformTag)
{
    std::scoped_lock lock(_registryMutex);

    dist_logger()->trace("get_lib_as_file: {} for platform {}", libName, platformTag);

    auto foundLib = _find_lib_for_platform(libName, platformTag);

    // Only our own build can be fetched; a peer cannot produce one for a platform we lack
    if (!foundLib.has_value() && _is_own_platform(platformTag))
    {
        if (!_install_lib_from_provider(libName))
        {
            return std::nullopt;
        }
        foundLib = _get_lib_path(libName);
    }

    if (!foundLib.has_value())
    {
        return std::nullopt;
    }

    const std::filesystem::path libPath = foundLib.value();

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

bool udp_registry::_install_lib_from_provider(const std::string& libName,
                                              const std::optional<std::string>& remoteHash)
{
    // initialize_udp runs on every deserialization, so without this check we would re-fetch the
    // same unusable artifact for every message this worker receives
    if (remoteHash.has_value() && _rejectedRemoteHashes.contains(remoteHash.value()))
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
        if (remoteHash.has_value())
        {
            _rejectedRemoteHashes.insert(remoteHash.value());
        }
        return false;
    }

    // The fetched artifact works, so it's now safe to drop whatever was here before
    _unload_lib(libName);
    _delete_lib_file(libName);
    std::filesystem::rename(candidate, _get_lib_path(libName));

    dist_logger()->debug("Installed the peer's copy of '{}'", libName);
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

void udp_registry::_load_lib(const std::string& libName, const std::filesystem::path& libPath)
{
    dist_logger()->trace("udp_registry loading lib file: {}", libPath.string());

    _lib_loaders.insert({libName, lib_loader<udp_base>{libPath.string()}});
    _lib_loaders.at(libName).open_lib();
    _lib_loaders.at(libName).install_log_sink(&dist_log_write);
}

bool udp_registry::_is_lib_in_cache(const std::string& libName) const
{
    return std::filesystem::exists(_get_lib_path(libName));
}

bool udp_registry::_is_own_platform(const std::string& platformTag) const
{
    return platformTag.empty() || platformTag == platform_tag();
}

std::optional<std::filesystem::path> udp_registry::_find_lib_for_platform(const std::string& libName,
                                                                          const std::string& platformTag) const
{
    if (_is_own_platform(platformTag))
    {
        return _find_local_lib(libName);
    }

    const auto extension = dll_extension_for_tag(platformTag);
    if (extension.empty())
    {
        return std::nullopt;
    }

    // Artifacts built elsewhere live in a per-platform directory of the cache
    const auto entry = _local_cache / platformTag / std::string{libName + extension};
    if (!std::filesystem::exists(entry))
    {
        return std::nullopt;
    }

    return entry;
}

std::optional<std::filesystem::path> udp_registry::_find_local_lib(const std::string& libName) const
{
    // Cache first: it only ever holds an artifact that already proved loadable here
    if (_is_lib_in_cache(libName))
    {
        return _get_lib_path(libName);
    }

    for (const auto& dir : _lib_search_dirs)
    {
        const auto candidate = dir / std::string{libName + portable_dll_extension()};
        if (std::filesystem::exists(candidate))
        {
            return candidate;
        }
    }

    return std::nullopt;
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
