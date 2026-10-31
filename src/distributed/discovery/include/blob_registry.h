#pragma once
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * Process-wide store of named read-only blobs.
 *
 * Exists so that a payload shared by every work unit, such as an input data set, travels once
 * per worker rather than inside every work_container. The controller puts the blob in; a worker
 * looks it up and, on a miss, asks the controller through the registered provider.
 */
class blob_registry
{
public:
    static blob_registry& get()
    {
        static blob_registry instance;
        return instance;
    }

    using blob_provider = std::function<std::optional<std::vector<std::byte>>(const std::string&)>;

    /**
     * Returns the blob, fetching it through the provider if it is not held locally.
     * A fetched blob is kept, so the provider is called at most once per key.
     */
    std::optional<std::vector<std::byte>> fetch(const std::string& key);

    /**
     * Returns the blob only if it is already held locally; never calls the provider.
     */
    std::optional<std::vector<std::byte>> lookup(const std::string& key) const;

    void put(const std::string& key, std::vector<std::byte> data);

    /**
     * Registers the function used on a cache miss. Called by the worker; the controller leaves
     * it unset, as it is the one holding the blobs.
     */
    void register_provider(const blob_provider& providerFunc);

    /**
     * Key derived from the content, so that changed data gets a different key.
     */
    static std::string make_key(const std::vector<std::byte>& data);

private:
    mutable std::mutex _mutex{};
    std::unordered_map<std::string, std::vector<std::byte>> _blobs{};
    blob_provider _provider{};

    blob_registry() = default;

public:
    blob_registry(blob_registry const&) = delete;
    void operator=(blob_registry const&) = delete;
};
