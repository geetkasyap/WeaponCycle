#pragma once

#include "config/Configuration.h"

#include <cstdint>
#include <string>

namespace orbitdeck {

// Result of parsing a JSON configuration document. On success, config holds the
// validated, mapped configuration; on failure, ok is false and error carries a
// concise, user-facing reason and config is left at its defaults.
struct ConfigParseResult {
    bool ok{false};
    Configuration config{};
    std::wstring error;
};

// Pure: parse, validate, and map a UTF-8 JSON configuration document into a
// Configuration. Performs no filesystem access, so it is directly unit-testable.
// Validates the documented rules (schema version, a bounded non-empty item
// list, unique ids, a launch candidate per item, positive/non-overlapping
// radii, a parseable hotkey, a valid theme colour) and expands environment
// variables in configured paths. Never throws.
[[nodiscard]] ConfigParseResult ParseConfiguration(const std::string& utf8Json);
[[nodiscard]] std::wstring ExpandConfigurationValue(const std::wstring& value);
[[nodiscard]] bool ParseHotkeyText(const std::wstring& text, ActivationConfig& activation);
[[nodiscard]] std::wstring FormatHotkey(const ActivationConfig& activation);
[[nodiscard]] ConfigParseResult LoadConfigurationFile(const std::wstring& path,
                                                      std::string* sourceJson = nullptr);

// The embedded eight-item example configuration as a UTF-8 JSON document. Used
// to seed config.json on first run and as an in-memory fallback when the file
// cannot be read.
[[nodiscard]] std::string DefaultConfigJson();
[[nodiscard]] std::string SerializeConfiguration(const Configuration& configuration);

struct ConfigSerializeResult {
    bool ok{false};
    std::string json;
    std::wstring error;
};

[[nodiscard]] ConfigSerializeResult SerializeConfigurationPreserving(
    const Configuration& configuration, const std::string& existingJson);

struct ConfigExportResult {
    bool ok{false};
    std::wstring error;
};

[[nodiscard]] ConfigExportResult ExportConfigurationFile(const Configuration& configuration,
                                                         const std::wstring& path);

// %LOCALAPPDATA%\OrbitDeck and its config.json, or empty if the known folder
// cannot be resolved.
[[nodiscard]] std::wstring ConfigFolderPath();
[[nodiscard]] std::wstring ConfigFilePath();

// config.json's last-write time as a FILETIME packed into an int64, or 0 when
// the file is missing or unreadable. A cheap probe for reload-on-open.
[[nodiscard]] long long ConfigLastWrite();

struct ConfigLoadResult {
    bool ok{false};
    Configuration config{};
    std::wstring error;
    long long lastWrite{0};
    std::uint64_t contentHash{0};
};

// Reads config.json from %LOCALAPPDATA%\OrbitDeck (writing the embedded default
// first if the file is missing), then parses and validates it. Returns ok=false
// with a reason on any read or validation failure so the caller can retain its
// last-known-good configuration. Never throws.
[[nodiscard]] ConfigLoadResult LoadConfiguration();

struct ConfigSaveResult {
    bool ok{false};
    std::wstring error;
    std::wstring warning;
    long long lastWrite{0};
    std::uint64_t contentHash{0};
};

// Atomically replaces config.json with the supported configuration model.
[[nodiscard]] ConfigSaveResult SaveConfiguration(const Configuration& configuration,
                                                 long long expectedLastWrite,
                                                 std::uint64_t expectedContentHash,
                                                 bool replaceInvalidExisting,
                                                 const std::string* replacementJson = nullptr);

}  // namespace orbitdeck
