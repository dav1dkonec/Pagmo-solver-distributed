#pragma once

#include <string>

#if defined(WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <system_error>

using HANDLE_TYPE = HMODULE;

inline HANDLE_TYPE portable_dlopen(const char* path)
{
    return LoadLibraryA(path);
}

inline FARPROC portable_dlsym(HANDLE_TYPE handle, const char* symbol)
{
    return GetProcAddress(handle, symbol);
}

inline int portable_dlclose(HANDLE_TYPE handle)
{
    return FreeLibrary(handle);
}

inline bool has_dlclose_failed(int return_value)
{
    return return_value == 0;
}

inline std::string error_msg()
{
    return std::string("LoadLibrary / CloseLibrary error code: ") + std::to_string(GetLastError());
}

#elif defined(__linux__) || defined(__APPLE__)
#include <dlfcn.h>

using HANDLE_TYPE = void*;

inline HANDLE_TYPE portable_dlopen(const char* path)
{
    return dlopen(path, RTLD_NOW);
}

inline void* portable_dlsym(HANDLE_TYPE handle, const char* symbol)
{
    return dlsym(handle, symbol);
}

inline int portable_dlclose(HANDLE_TYPE handle)
{
    return dlclose(handle);
}

inline bool has_dlclose_failed(int return_value)
{
    return return_value != 0;
}

inline std::string error_msg()
{
    const char* err = dlerror();
    return err ? std::string(err) : std::string{"Unknown Error"};
}

#else
#error "Dynamic library loader is not supported on this platform (none of the macros: __linux__, __APPLE__ or WIN32 are defined)"
#endif

/*
 * Helper inline function to get the correct dynamic library file extension based on OS
 */
inline constexpr std::string portable_dll_extension()
{
#if defined(WIN32)
    return ".dll";

#elif defined(__APPLE__)
    return ".dylib";

#elif defined(__linux__)
    return ".so";

#else
#error "Dynamic library extension is not defined for this platform"
#endif
}

/*
 * Identifies the build target of this process as "<os>-<arch>". Library artifacts are catalogued
 * under it, so that a node is served a binary it can actually load.
 */
inline std::string platform_tag()
{
#if defined(WIN32)
    const std::string os = "windows";
#elif defined(__APPLE__)
    const std::string os = "darwin";
#elif defined(__linux__)
    const std::string os = "linux";
#else
    const std::string os = "unknown";
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
    const std::string arch = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    const std::string arch = "x86_64";
#elif defined(__arm__) || defined(_M_ARM)
    const std::string arch = "arm";
#elif defined(__i386__) || defined(_M_IX86)
    const std::string arch = "x86";
#else
    const std::string arch = "unknown";
#endif

    return os + "-" + arch;
}

/*
 * Dynamic library extension used by the given platform tag, which is not necessarily ours.
 * Empty if the tag names an OS we do not know.
 */
inline std::string dll_extension_for_tag(const std::string& platformTag)
{
    if (platformTag.starts_with("windows-"))
        return ".dll";
    if (platformTag.starts_with("darwin-"))
        return ".dylib";
    if (platformTag.starts_with("linux-"))
        return ".so";

    return {};
}
