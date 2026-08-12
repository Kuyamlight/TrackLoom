#include "AppAudioSettings.h"

#include "AtomicFileReplace.h"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace {

bool isValid(const trackloom::AppAudioSettings& settings)
{
    return std::isfinite(settings.requestedSampleRate)
        && settings.requestedSampleRate > 0.0
        && settings.requestedBufferFrames > 0
        && (settings.requestedOutputChannels == 1 || settings.requestedOutputChannels == 2);
}

trackloom::AppAudioSettingsLoadResult invalidSettings()
{
    return {
        trackloom::AppAudioSettingsLoadKind::Invalid,
        trackloom::AppAudioSettings {},
        "\xE9\x9F\xB3\xE9\xA2\x91\xE8\xAE\xBE\xE7\xBD\xAE\xE6\x96\x87\xE4\xBB\xB6\xE6\x97\xA0\xE6\x95\x88\xEF\xBC\x8C\xE5\xB7\xB2\xE6\x81\xA2\xE5\xA4\x8D\xE9\xBB\x98\xE8\xAE\xA4\xE8\xAE\xBE\xE7\xBD\xAE\xE3\x80\x82"
    };
}

bool writeSettings(const trackloom::AppAudioSettings& settings, const std::filesystem::path& path)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }

    output << "trackloom_audio_settings 1\n";
    output << "output_device " << std::quoted(settings.outputDeviceName) << '\n';
    output << "sample_rate " << settings.requestedSampleRate << '\n';
    output << "buffer_frames " << settings.requestedBufferFrames << '\n';
    output << "output_channels " << settings.requestedOutputChannels << '\n';
    output.flush();
    return output.good();
}

template <typename Value>
bool parseSettingsLine(const std::string& line, const char* key, Value& value)
{
    std::istringstream input(line);
    std::string parsedKey;
    std::string trailing;
    return (input >> parsedKey >> value)
        && parsedKey == key
        && !(input >> trailing);
}

bool parseQuotedSettingsLine(const std::string& line, const char* key, std::string& value)
{
    std::istringstream input(line);
    std::string parsedKey;
    std::string trailing;
    if (!(input >> parsedKey) || parsedKey != key) {
        return false;
    }
    input >> std::ws;
    if (input.peek() != '"') {
        return false;
    }
    return (input >> std::quoted(value))
        && !(input >> trailing);
}

}

namespace trackloom {

AppAudioSettingsLoadResult loadAppAudioSettings(const std::filesystem::path& path)
{
    if (path.empty()) {
        return { AppAudioSettingsLoadKind::Missing, AppAudioSettings {}, {} };
    }

    std::error_code existsError;
    if (!std::filesystem::exists(path, existsError)) {
        if (existsError) {
            return invalidSettings();
        }
        return { AppAudioSettingsLoadKind::Missing, AppAudioSettings {}, {} };
    }

    std::ifstream input(path, std::ios::binary);
    std::string headerLine;
    std::string outputLine;
    std::string sampleRateLine;
    std::string bufferFramesLine;
    std::string outputChannelsLine;
    std::string trailing;
    AppAudioSettings settings;
    int version = 0;
    if (!input
        || !std::getline(input, headerLine)
        || !std::getline(input, outputLine)
        || !std::getline(input, sampleRateLine)
        || !std::getline(input, bufferFramesLine)
        || !std::getline(input, outputChannelsLine)
        || std::getline(input, trailing)
        || !parseSettingsLine(headerLine, "trackloom_audio_settings", version)
        || version != 1
        || !parseQuotedSettingsLine(outputLine, "output_device", settings.outputDeviceName)
        || !parseSettingsLine(sampleRateLine, "sample_rate", settings.requestedSampleRate)
        || !parseSettingsLine(bufferFramesLine, "buffer_frames", settings.requestedBufferFrames)
        || !parseSettingsLine(outputChannelsLine, "output_channels", settings.requestedOutputChannels)
        || !isValid(settings)) {
        return invalidSettings();
    }

    return { AppAudioSettingsLoadKind::Loaded, std::move(settings), {} };
}

bool saveAppAudioSettings(const AppAudioSettings& settings, const std::filesystem::path& path)
{
    if (path.empty() || !isValid(settings)) {
        return false;
    }

    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code createDirectoryError;
        std::filesystem::create_directories(parent, createDirectoryError);
        if (createDirectoryError) {
            return false;
        }
    }

    auto temporaryPath = path;
    temporaryPath += ".tmp";
    if (!writeSettings(settings, temporaryPath)) {
        std::error_code ignoredError;
        std::filesystem::remove(temporaryPath, ignoredError);
        return false;
    }

    const auto replacement = replaceFileAtomically(temporaryPath, path);
    if (!replacement.success) {
        std::error_code ignoredError;
        std::filesystem::remove(temporaryPath, ignoredError);
    }
    return replacement.success;
}

}
