#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <any>

#include "hash_cache.h"
#include "lib_loader.h"
#include "lru_cache.h"
#include "udp_base.h"

/**
 * A "service discovery" object implemented as singleton.
 * Allows any part of the application to request and load a UDP (User Defined Problems) from a dynamically loaded library.
 * UDP can be defined in separate DLLs, which can be transferred over the network and loaded at run time.
 */
class udp_registry
{
public:
    /**
     * Returns a static instance of this object
     */
    static udp_registry& get()
    {
        static udp_registry instance;
        return instance;
    }

    /**
     * Loads the dynamic library without constructing any object
     * @param name Name of the UDP's library
     * @param libFileHashes Hash of the peer's artifact per platform tag, used to spot a new version
     */
    void initialize_udp(const std::string& name, const std::map<std::string, std::string>& libFileHashes);

    /**
     * Constructs a UDP, which is contained within some dynamic library
     * @param name Name of the UDP's library
     * @param params std::any containing a value passed to the UDP's constructor
     * @return Shared ptr pointing to a newly constructed instance of the UDP
     */
    std::shared_ptr<udp_base> construct_udp(const std::string& name, const std::any& params = std::any());

    /**
     * Clones a UDP which was previously created using construct_udp
     * @param other pointer to the object that's meant to be cloned
     * @return Shared ptr pointing to the cloned object
     */
    std::shared_ptr<udp_base> clone_udp(const std::shared_ptr<udp_base>& other);


    using udp_provider = std::function<std::optional<std::vector<std::byte>>(const std::string&)>;

    /**
     * Registers a function which will be called if the udp_registry can't find a DLL locally
     * @param providerFunc Function that returns the DLL file as byte vector, or an empty std::optional if it doesn't exist
     */
    void register_udp_provider(const udp_provider& providerFunc);

    using udp_publisher = std::function<void(const std::string& libName, const std::string& platformTag,
                                             const std::vector<std::byte>& libFile)>;

    /**
     * Registers a function which offers our build of a library to a peer that has none for us
     */
    void register_udp_publisher(const udp_publisher& publisherFunc);

    /**
     * Stores a peer's build as the catalogue entry for its platform. An existing entry is never
     * replaced, so a peer cannot substitute an artifact this node already serves.
     * @return true if the entry is now stored
     */
    bool store_lib_for_platform(const std::string& libName, const std::string& platformTag,
                                const std::vector<std::byte>& libFile);

    void set_local_cache_dir(const std::filesystem::path& directory);

    /**
     * Adds a read-only directory searched for libraries before any is fetched from a peer.
     * Meant for the directory holding this process' own build.
     */
    void add_lib_search_dir(const std::filesystem::path& directory);

    void use_in_memory_cache(bool value);

    /**
     * Reads the library as bytes, ready to be sent to a peer
     * @param platformTag Build the caller can load; empty or our own tag means the local build
     * @return File contents, or nullopt if no artifact is catalogued for that platform
     */
    std::optional<std::vector<std::byte>> get_lib_as_file(const std::string& libName,
                                                          const std::string& platformTag = {});

    std::optional<std::string> get_lib_file_hash(const std::string& libName,
                                                 const std::string& platformTag = {});

    /**
     * Hashes every platform this node can serve the library for, so that a peer of any platform
     * can tell whether its own copy is still the current one
     * @return Map of platform tag to hash
     */
    std::map<std::string, std::string> get_lib_file_hashes(const std::string& libName);
private:
    // Recursive so public functions can call each other (one thread can lock it multiple times)
    std::recursive_mutex  _registryMutex{};

    // Where to store or look for DLL files. Not the working directory, so that a library
    // fetched from a peer cannot overwrite build output.
    std::filesystem::path _local_cache = "udp_cache";

    // An in-memory cache which prevents repeated reading of the DLL file from FS
    bool _use_in_memory_cache = true;
    lru_cache<std::vector<std::byte>> _libFilesBuffer{5};

    // Map which stores lib_loader objects of libraries which we have already loaded into memory
    std::unordered_map<std::string, lib_loader<udp_base>> _lib_loaders{};

    // Lambda which returns the DLL / shared library file as an array of bytes
    udp_provider _udp_provider{};

    // Lambda which sends our build to a peer, and the libraries already offered to it
    udp_publisher _udp_publisher{};
    std::unordered_set<std::string> _publishedLibs{};

    // Cache for the get_lib_file_hash() function, this prevents repeated re-computing of hashes
    hash_cache _hash_cache{};

    // Peer artifacts that cannot be loaded here. Keyed by hash, not by name, so that a
    // new build on the peer is still retried.
    std::unordered_set<std::string> _rejectedRemoteHashes{};

    // Searched after the cache. Never written to, so a peer cannot overwrite a local build.
    std::vector<std::filesystem::path> _lib_search_dirs{};

    void _save_lib_into_fs(const std::string& libName, const std::vector<std::byte>& libFile);

    void _write_lib_file(const std::filesystem::path& libPath, const std::vector<std::byte>& libFile);

    /**
     * Fetches the library from the provider and installs it as the local copy, but only if the
     * fetched file can actually be loaded by this process. An existing copy is kept on failure.
     * @param remoteHash Hash the peer reported, if known; used to not re-fetch a rejected artifact
     * @return true if the local copy is now the fetched one
     */
    bool _install_lib_from_provider(const std::string& libName,
                                    const std::optional<std::string>& remoteHash = std::nullopt);

    /**
     * Returns true if the file at this path can be loaded as a dynamic library here
     */
    bool _is_loadable(const std::filesystem::path& libPath) const;

    void _load_lib(const std::string& libName, const std::filesystem::path& libPath);

    bool _is_lib_in_cache(const std::string& libName) const;

    /**
     * Looks for the library in the cache and then in the search directories
     * @return Path to the first existing copy, or nullopt if there is none
     */
    std::optional<std::filesystem::path> _find_local_lib(const std::string& libName) const;

    /**
     * Looks for the build of the library that the given platform can load. Our own build for our
     * own tag, otherwise the catalogue entry in the <cache>/<tag> directory.
     */
    std::optional<std::filesystem::path> _find_lib_for_platform(const std::string& libName,
                                                                const std::string& platformTag) const;

    bool _is_own_platform(const std::string& platformTag) const;

    /**
     * Offers our build of the library to the peer, at most once per library
     */
    void _publish_lib_to_peer(const std::string& libName);

    /**
     * Returns a full path to a library file located in local cache (including extension)
     */
    std::filesystem::path _get_lib_path(const std::string& libName) const;

    /**
     * Unloads this library from the address space of this process
     */
    void _unload_lib(const std::string& libName);

    /**
     * Deletes the library from local fs, in-memory cache and from hash cache
     */
    void _delete_lib_file(const std::string& libName);

    udp_registry() = default;

public:
    udp_registry(udp_registry const&) = delete;
    void operator=(udp_registry const&) = delete;
};
