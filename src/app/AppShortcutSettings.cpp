#include "AppShortcutSettings.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <string>

namespace trackloom {
namespace {

char normalizedSettingsKey(char key)
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(key)));
}

bool parseBinaryFlag(int value, bool& flag)
{
    if (value == 0) {
        flag = false;
        return true;
    }
    if (value == 1) {
        flag = true;
        return true;
    }
    return false;
}

bool parseShortcutSettingsLine(const std::string& line, AppShortcutBinding& binding)
{
    if (line.empty() || line.front() == '#') {
        return false;
    }

    int commandId = 0;
    std::string keyToken;
    int primary = 0;
    int shift = 0;
    int alt = 0;
    std::string trailingToken;

    std::istringstream input(line);
    if (!(input >> commandId >> keyToken >> primary >> shift >> alt) || (input >> trailingToken)) {
        return false;
    }
    if (commandId <= 0 || keyToken.size() != 1) {
        return false;
    }

    AppShortcutChord chord;
    chord.key = normalizedSettingsKey(keyToken[0]);
    if (!parseBinaryFlag(primary, chord.primaryModifier)
        || !parseBinaryFlag(shift, chord.shift)
        || !parseBinaryFlag(alt, chord.alt)
        || !isSupportedAppShortcutChord(chord)) {
        return false;
    }

    binding = { chord, commandId };
    return true;
}

void writeShortcutSettingsLine(std::ofstream& output, const AppShortcutBinding& binding)
{
    output << binding.commandId << ' '
           << normalizedSettingsKey(binding.chord.key) << ' '
           << (binding.chord.primaryModifier ? 1 : 0) << ' '
           << (binding.chord.shift ? 1 : 0) << ' '
           << (binding.chord.alt ? 1 : 0) << '\n';
}

}

bool saveAppShortcutCustomBindings(
    const std::vector<AppShortcutBinding>& customBindings,
    const std::filesystem::path& settingsPath)
{
    if (settingsPath.empty()) {
        return false;
    }

    const auto parent = settingsPath.parent_path();
    if (!parent.empty()) {
        std::error_code createDirectoryError;
        std::filesystem::create_directories(parent, createDirectoryError);
        if (createDirectoryError) {
            return false;
        }
    }

    std::ofstream output(settingsPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }

    output << "# TrackLoom shortcut settings v1\n";
    output << "# commandId key primary shift alt\n";
    for (const auto& binding : customBindings) {
        if (binding.commandId > 0 && isSupportedAppShortcutChord(binding.chord)) {
            writeShortcutSettingsLine(output, binding);
        }
    }

    return output.good();
}

std::vector<AppShortcutBinding> loadAppShortcutCustomBindings(
    const std::filesystem::path& settingsPath)
{
    std::vector<AppShortcutBinding> bindings;
    if (settingsPath.empty()) {
        return bindings;
    }

    std::error_code existsError;
    const auto settingsFileExists = std::filesystem::exists(settingsPath, existsError);
    if (existsError || !settingsFileExists) {
        return bindings;
    }

    std::ifstream input(settingsPath, std::ios::binary);
    if (!input) {
        return bindings;
    }

    std::string line;
    while (std::getline(input, line)) {
        AppShortcutBinding binding;
        if (parseShortcutSettingsLine(line, binding)) {
            bindings.push_back(binding);
        }
    }

    return bindings;
}

AppShortcutCustomizationResult loadAppShortcutCustomization(
    const std::filesystem::path& settingsPath)
{
    return customizeAppShortcutBindings(loadAppShortcutCustomBindings(settingsPath));
}

}
