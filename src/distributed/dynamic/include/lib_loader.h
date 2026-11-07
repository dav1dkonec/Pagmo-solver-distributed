#pragma once

#include <memory>
#include <utility>
#include <string>
#include <stdexcept>
#include "defines.h"
#include <any>
/**
 * - Dynamic library loader providing a POSIX / Win32 interface for loading a C++ dynamic library
 *
 * - The header should have these functions specified:
 *      - extern "C" DLL_PUBLIC my_udp* allocator(const std::any& params);
 *          - returning a pointer to new instance
 *      - extern "C" DLL_PUBLIC void deleter(my_udp* ptr);
 *          - deleting the object allocated by allocator
 *      - extern "C" DLL_PUBLIC my_udp* cloner(const my_udp* other);
 *          - copy-constructs a new object from the passed pointer
 *
 * - The following functions are optional:
 *      - extern "C" DLL_PUBLIC void run_after_load();
 *          - gets executed during the first call to get_instance or clone_instance
 *      - extern "C" DLL_PUBLIC void set_dist_log_sink(void (*sink)(int, const char*));
 *          - receives the host's log sink right after the library is opened
 *
 * @tparam T The class contained within this library
 */
template <class T>
class lib_loader
{
private:
    HANDLE_TYPE _handle;
    std::string _pathToLib;
    std::string _allocSymbol;
    std::string _deleteSymbol;
    std::string _runAfterLoadSymbol;
    std::string _cloneSymbol;

    bool _afterLoadExecuted = false;

    void _ensure_after_load()
    {
        if (_afterLoadExecuted)
            return;

        _afterLoadExecuted = true;

        using runAfterLoadT = void (*)();

        auto runAfterLoadFunc = reinterpret_cast<runAfterLoadT>(
            portable_dlsym(_handle, _runAfterLoadSymbol.c_str())
        );

        if (runAfterLoadFunc)
        {
            runAfterLoadFunc();
        }
    }

public:
    explicit lib_loader(std::string pathToLib,
                        std::string allocSymbol = "allocator",
                        std::string deleteSymbol = "deleter",
                        std::string runAfterLoadSymbol = "run_after_load",
                        std::string cloneSymbol = "cloner") :
        _handle(nullptr),
        _pathToLib(std::move(pathToLib)),
        _allocSymbol(std::move(allocSymbol)),
        _deleteSymbol(std::move(deleteSymbol)),
        _runAfterLoadSymbol(std::move(runAfterLoadSymbol)),
        _cloneSymbol(std::move(cloneSymbol))
    {
    }

    virtual ~lib_loader()
    {
        close_lib();
    }

    /**
     * Opens the dynamic library (specified by pathToLib in the constructor)
     */
    void open_lib()
    {
        if (!(_handle = portable_dlopen(_pathToLib.c_str())))
        {
            throw std::runtime_error{error_msg()};
        }
    }

    // A loaded library cannot reach the host's logger instance, so it is handed a sink instead.
    void install_log_sink(void (*sink)(int, const char*))
    {
        using installT = void (*)(void (*)(int, const char*));

        if (const auto install = reinterpret_cast<installT>(portable_dlsym(_handle, "set_dist_log_sink")))
        {
            install(sink);
        }
    }

    /**
     * Creates an instance of the class contained within this dynamic library
     * @param params Parameters of the contained object
     * @return Shared pointer to an instance of the class
     */
    std::shared_ptr<T> get_instance(const std::any& params = std::any())
    {
        using allocT = T *(*)(const std::any& params);
        using deleteT = void (*)(T*);

        auto allocFunc = reinterpret_cast<allocT>(
            portable_dlsym(_handle, _allocSymbol.c_str())
        );
        auto deleteFunc = reinterpret_cast<deleteT>(
            portable_dlsym(_handle, _deleteSymbol.c_str())
        );

        _ensure_after_load();

        if (!allocFunc || !deleteFunc)
        {
            close_lib();
            throw std::runtime_error("Can't find allocator or deleter symbol in " + _pathToLib);
        }

        return std::shared_ptr<T>(
            allocFunc(params),
            [deleteFunc](T* p) { deleteFunc(p); }
        );
    }

    /**
     * Clones an existing instance using the library-provided clone function
     */
    std::shared_ptr<T> clone_instance(const std::shared_ptr<T>& other)
    {
        using cloneT = T* (*)(const T*);
        using deleteT = void (*)(T*);

        auto cloneFunc = reinterpret_cast<cloneT>(
            portable_dlsym(_handle, _cloneSymbol.c_str())
        );

        auto deleteFunc = reinterpret_cast<deleteT>(
            portable_dlsym(_handle, _deleteSymbol.c_str())
        );

        _ensure_after_load();

        if (!cloneFunc || !deleteFunc)
        {
            throw std::runtime_error("Can't find clone or deleter symbol in " + _pathToLib);
        }

        return std::shared_ptr<T>(
            cloneFunc(other.get()),
            [deleteFunc](T* p) { deleteFunc(p); }
        );
    }

    /**
     * Closes the dynamically loaded library
     */
    void close_lib()
    {
        if (!_handle)
            return;

        if (has_dlclose_failed(portable_dlclose(_handle)))
        {
            throw std::runtime_error{error_msg()};
        }

        _handle = nullptr;
    }
};