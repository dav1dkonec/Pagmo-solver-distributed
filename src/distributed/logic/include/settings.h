#pragma once
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <concepts>
#include <pagmo/s11n.hpp>
#include <boost/archive/xml_iarchive.hpp>
#include <boost/archive/xml_oarchive.hpp>
#include <boost/serialization/nvp.hpp>

// The parameter must have an initialize() function
template <typename T>
concept Initializable = requires(T t)
{
    { t.initialize() } -> std::same_as<void>;
};

/**
 * A class that handles saving or loading settings from the local filesystem
 * @tparam SettingsStruct serializable settings struct
 */
template <Initializable SettingsStruct>
class settings
{
    // Location of the settings file
    std::filesystem::path _settingsPath;

    SettingsStruct _settings{};

    bool _autoSave;

public:
    explicit settings(const std::filesystem::path& settingsPath = "./settings.xml", const bool autoSave = true) :
        _settingsPath(settingsPath), _autoSave(autoSave)
    {
        _initialize_settings();
    }

    /**
     * User should call this function after performing changes.
     * Otherwise changes are automatically saved on each () operator call.
     */
    void save()
    {
        _save_to_file();
    }

    SettingsStruct& operator ()()
    {
        if (_autoSave)
        {
            // With this call we at least make sure that the previous version is always saved
            _save_to_file();
        }
        return _settings;
    }

private:
    void _initialize_settings()
    {
        if (std::filesystem::exists(_settingsPath))
        {
            std::ifstream fStream{_settingsPath, std::ios::binary};
            if (!fStream)
            {
                throw std::runtime_error("Failed to open settings file: " + _settingsPath.string());
            }

            try
            {
                // Deserialize
                boost::archive::xml_iarchive ia(fStream);
                ia >> BOOST_SERIALIZATION_NVP(_settings);

                return;
            }
            catch (const boost::archive::archive_exception& e)
            {
                // The file only caches what the nodes have shown, so a run that cannot read it
                // clears it and measures again. initialize() does the clearing, because a
                // half-read archive leaves records behind.
                std::cerr << "Discarding " << _settingsPath.string() << " and measuring again: "
                    << e.what() << std::endl;
            }
        }

        // Settings file doesn't exist, call the initialize function of the struct to use initial default values
        _settings.initialize();
    }

    void _save_to_file()
    {
        std::ofstream file(_settingsPath, std::ios::binary | std::ios::trunc);
        file.exceptions(std::ios::failbit | std::ios::badbit);

        // Serialize into file
        boost::archive::xml_oarchive oa(file);
        oa << BOOST_SERIALIZATION_NVP(_settings);
    }
};
