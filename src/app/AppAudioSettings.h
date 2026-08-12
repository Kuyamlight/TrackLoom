#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace trackloom {

struct AppAudioSettings {
    std::string outputDeviceName;
    double requestedSampleRate = 48000.0;
    int requestedBufferFrames = 256;
    int requestedOutputChannels = 2;
};

enum class AppAudioSettingsLoadKind {
    Loaded,
    Missing,
    Invalid
};

struct AppAudioSettingsLoadResult {
    AppAudioSettingsLoadKind kind = AppAudioSettingsLoadKind::Missing;
    AppAudioSettings settings;
    std::string warning;
};

AppAudioSettingsLoadResult loadAppAudioSettings(const std::filesystem::path& path);
bool saveAppAudioSettings(
    const AppAudioSettings& settings,
    const std::filesystem::path& path);

using AppAudioSettingsLoadOperation = std::function<
    AppAudioSettingsLoadResult(const std::filesystem::path&)>;
using AppAudioSettingsSaveOperation = std::function<
    bool(const AppAudioSettings&, const std::filesystem::path&)>;

}
