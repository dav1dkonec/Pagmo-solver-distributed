#include "blob_registry.h"

#include <cstdio>

#include "logger_init.h"

std::optional<std::vector<std::byte>> blob_registry::lookup(const std::string& key) const
{
    std::lock_guard<std::mutex> lock{_mutex};

    const auto found = _blobs.find(key);
    return (found != _blobs.end()) ? std::optional{found->second} : std::nullopt;
}

void blob_registry::put(const std::string& key, std::vector<std::byte> data)
{
    std::lock_guard<std::mutex> lock{_mutex};
    _blobs.insert_or_assign(key, std::move(data));
}

void blob_registry::register_provider(const blob_provider& providerFunc)
{
    std::lock_guard<std::mutex> lock{_mutex};
    _provider = providerFunc;
}

std::optional<std::vector<std::byte>> blob_registry::fetch(const std::string& key)
{
    // Held across the provider call, as in udp_registry: its socket identity serves one request.
    std::lock_guard<std::mutex> lock{_mutex};

    const auto found = _blobs.find(key);
    if (found != _blobs.end())
    {
        return found->second;
    }

    if (!_provider)
    {
        dist_logger()->error("Blob {} is not held locally and no provider is registered", key);
        return std::nullopt;
    }

    auto fetched = _provider(key);
    if (!fetched)
    {
        dist_logger()->error("The controller did not provide blob {}", key);
        return std::nullopt;
    }

    dist_logger()->debug("Fetched blob {} from the controller, {} B", key, fetched->size());

    _blobs.insert_or_assign(key, *fetched);

    return fetched;
}

std::string blob_registry::make_key(const std::vector<std::byte>& data)
{
    // FNV-1a; distinguishes data sets within a run, not an attacker's chosen collisions.
    uint64_t hash = 1469598103934665603ull;
    for (const std::byte b : data)
    {
        hash ^= static_cast<uint64_t>(b);
        hash *= 1099511628211ull;
    }

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "blob_%016llx_%zu",
                  static_cast<unsigned long long>(hash), data.size());

    return buffer;
}
