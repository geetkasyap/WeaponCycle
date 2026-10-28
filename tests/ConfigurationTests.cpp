// Proves the Phase 8 configuration parser/validator that backs config-driven
// items. Covers a valid document, defaults for missing optional fields, every
// documented validation rule (schema version, duplicate ids, radii, empty
// launch candidates, malformed hotkey, invalid colour, item-count safety),
// dynamic item counts, environment-variable expansion, disabled items,
// item-order preservation, and corrupt JSON. Pure ParseConfiguration only: no
// filesystem, no shell. Plain return-code harness.

#include "config/ConfigLoader.h"

#include <windows.h>

#include <cstdio>
#include <string>

using orbitdeck::ParseConfiguration;

namespace {

// Wraps an items array in a minimal valid document body.
std::string Doc(const std::string& body) {
    return std::string("{\"schemaVersion\":1,") + body + "}";
}

const char* kOneItem =
    "\"items\":[{\"id\":\"a\",\"label\":\"A\","
    "\"launch\":{\"candidates\":[{\"type\":\"shell\",\"target\":\"a.exe\"}]}}]";

}  // namespace

int main() {
    {
        const auto parsed =
            ParseConfiguration(Doc("\"items\":[{\"id\":\"convert\",\"label\":\"Convert a file\","
                                   "\"builtin\":\"fileConverter\"}]"));
        if (!parsed.ok ||
            parsed.config.items[0].builtInTool != orbitdeck::BuiltInTool::FileConverter)
            return 120;
        const auto restored = ParseConfiguration(orbitdeck::SerializeConfiguration(parsed.config));
        if (!restored.ok ||
            restored.config.items[0].builtInTool != orbitdeck::BuiltInTool::FileConverter)
            return 121;
    }
    // A valid document parses and maps its single item.
    {
        auto r = ParseConfiguration(Doc(kOneItem));
        if (!r.ok)
            return 1;
        if (r.config.items[0].label != L"A")
            return 2;
        if (r.config.items[0].target != L"a.exe")
            return 3;
        if (r.config.items.size() != 1)
            return 4;
        if (r.config.items[0].launchCandidates.size() != 1 ||
            r.config.items[0].launchCandidates[0].target != L"a.exe") {
            return 90;
        }
    }

    // Missing optional fields (no activation/appearance/icon) still succeed and
    // fall back to the default hotkey (Shift+Space).
    {
        auto r = ParseConfiguration(Doc(kOneItem));
        if (!r.ok)
            return 5;
        if (r.config.activation.virtualKey != VK_SPACE)
            return 6;
        if ((r.config.activation.modifiers & MOD_SHIFT) == 0)
            return 7;
    }

    // Unsupported schema version is rejected.
    {
        auto r = ParseConfiguration(std::string("{\"schemaVersion\":2,") + kOneItem + "}");
        if (r.ok)
            return 8;
    }
    {
        auto r = ParseConfiguration(std::string("{\"schemaVersion\":1.5,") + kOneItem + "}");
        if (r.ok)
            return 68;
    }

    // Duplicate item ids are rejected.
    {
        auto r = ParseConfiguration(
            Doc("\"items\":["
                "{\"id\":\"dup\",\"launch\":{\"candidates\":[{\"target\":\"a\"}]}},"
                "{\"id\":\"dup\",\"launch\":{\"candidates\":[{\"target\":\"b\"}]}}]"));
        if (r.ok)
            return 9;
    }
    {
        auto r =
            ParseConfiguration(Doc("\"items\":[{\"id\":\"a\",\"label\":42,"
                                   "\"launch\":{\"candidates\":[{\"target\":\"x\"}]}}]"));
        if (r.ok)
            return 69;
    }
    {
        auto r =
            ParseConfiguration(Doc("\"items\":[{\"id\":\"a\",\"icon\":{\"path\":42},"
                                   "\"launch\":{\"candidates\":[{\"target\":\"x\"}]}}]"));
        if (r.ok)
            return 70;
    }
    {
        auto r =
            ParseConfiguration(Doc("\"items\":[{\"id\":\"a\",\"existingWindowAction\":42,"
                                   "\"launch\":{\"candidates\":[{\"target\":\"x\"}]}}]"));
        if (r.ok)
            return 71;
    }
    {
        auto r = ParseConfiguration(
            Doc("\"items\":[{\"id\":\"a\","
                "\"launch\":{\"candidates\":[{\"type\":42,\"target\":\"x\"}]}}]"));
        if (r.ok)
            return 72;
    }

    // An inner radius not smaller than the outer radius is rejected.
    {
        auto r = ParseConfiguration(
            Doc("\"appearance\":{\"innerRadiusDip\":300,\"outerRadiusDip\":200}," +
                std::string(kOneItem)));
        if (r.ok)
            return 10;
    }

    // A negative dimension is rejected.
    {
        auto r = ParseConfiguration(
            Doc("\"appearance\":{\"outerRadiusDip\":-5}," + std::string(kOneItem)));
        if (r.ok)
            return 11;
    }

    // An item with no launch candidates is rejected.
    {
        auto r =
            ParseConfiguration(Doc("\"items\":[{\"id\":\"a\",\"launch\":{\"candidates\":[]}}]"));
        if (r.ok)
            return 12;
    }
    {
        auto r =
            ParseConfiguration(Doc("\"items\":[{\"id\":\"a\",\"launch\":{\"candidates\":["
                                   "{\"type\":\"appPath\",\"target\":\"missing.exe\"},"
                                   "{\"type\":\"uri\",\"target\":\"sample:\"}]}}]"));
        if (!r.ok || r.config.items[0].launchCandidates.size() != 2) {
            return 91;
        }
        if (r.config.items[0].launchCandidates[1].target != L"sample:") {
            return 92;
        }
    }
    {
        auto r =
            ParseConfiguration(Doc("\"items\":[{\"id\":\"a\",\"launch\":{\"candidates\":["
                                   "{\"target\":\"ok\"},{\"target\":\"\"}]}}]"));
        if (r.ok)
            return 93;
    }

    // A malformed hotkey is rejected.
    {
        auto r = ParseConfiguration(
            Doc("\"activation\":{\"hotkey\":\"Sh!ft+Spce\"}," + std::string(kOneItem)));
        if (r.ok)
            return 13;
    }

    // An invalid theme colour (bad hex) is rejected.
    {
        auto r =
            ParseConfiguration(Doc("\"appearance\":{\"theme\":\"#XYZ\"}," + std::string(kOneItem)));
        if (r.ok)
            return 14;
    }

    // Item count is driven by configuration rather than a fixed eight slots.
    {
        std::string items = "\"items\":[";
        for (int i = 0; i < 9; ++i) {
            if (i)
                items += ",";
            items += "{\"id\":\"i" + std::to_string(i) +
                     "\",\"launch\":{\"candidates\":[{\"target\":\"x\"}]}}";
        }
        items += "]";
        auto r = ParseConfiguration(Doc(items));
        if (!r.ok)
            return 15;
        if (r.config.items.size() != 9)
            return 36;
    }

    // A generous safety ceiling prevents an unusable or abusive wheel.
    {
        std::string maximumItems = "\"items\":[";
        for (std::size_t i = 0; i < orbitdeck::kMaximumWheelItems; ++i) {
            if (i)
                maximumItems += ",";
            maximumItems += "{\"id\":\"max" + std::to_string(i) +
                            "\",\"launch\":{\"candidates\":[{\"target\":\"x\"}]}}";
        }
        maximumItems += "]";
        const auto maximum = ParseConfiguration(Doc(maximumItems));
        if (!maximum.ok || maximum.config.items.size() != orbitdeck::kMaximumWheelItems) {
            return 37;
        }

        std::string items = "\"items\":[";
        for (std::size_t i = 0; i <= orbitdeck::kMaximumWheelItems; ++i) {
            if (i)
                items += ",";
            items += "{\"id\":\"i" + std::to_string(i) +
                     "\",\"launch\":{\"candidates\":[{\"target\":\"x\"}]}}";
        }
        items += "]";
        if (ParseConfiguration(Doc(items)).ok)
            return 40;
    }

    // An empty wheel is invalid.
    {
        if (ParseConfiguration(Doc("\"items\":[]")).ok)
            return 38;
    }

    // Environment variables in a target expand.
    {
        SetEnvironmentVariableW(L"ORBITDECK_TEST", L"C:\\Tmp");
        auto r =
            ParseConfiguration(Doc("\"items\":[{\"id\":\"a\",\"launch\":{\"candidates\":"
                                   "[{\"target\":\"%ORBITDECK_TEST%\\\\x.exe\"}]}}]"));
        if (!r.ok)
            return 16;
        if (r.config.items[0].target != L"C:\\Tmp\\x.exe")
            return 17;
        if (r.config.items[0].configuredTarget != L"%ORBITDECK_TEST%\\x.exe")
            return 62;
        if (orbitdeck::ExpandConfigurationValue(L"%ORBITDECK_TEST%\\icon.ico") !=
            L"C:\\Tmp\\icon.ico") {
            return 104;
        }
        {
            SetEnvironmentVariableW(L"ORBITDECK_LITERAL", L"expanded");
            auto literalResult = ParseConfiguration(
                Doc("\"items\":[{\"id\":\"clip\",\"launch\":{\"candidates\":"
                    "[{\"type\":\"clipboard\",\"target\":\"%ORBITDECK_LITERAL%\"}]}}]"));
            if (!literalResult.ok ||
                literalResult.config.items[0].target != L"%ORBITDECK_LITERAL%") {
                return 142;
            }
        }
    }

    // A disabled item maps enabled=false.
    {
        auto r =
            ParseConfiguration(Doc("\"items\":[{\"id\":\"a\",\"enabled\":false,"
                                   "\"launch\":{\"candidates\":[{\"target\":\"a\"}]}}]"));
        if (!r.ok)
            return 18;
        if (r.config.items[0].enabled)
            return 19;
    }

    // Item order is preserved into wheel order.
    {
        auto r =
            ParseConfiguration(Doc("\"items\":["
                                   "{\"id\":\"a\",\"label\":\"First\","
                                   "\"launch\":{\"candidates\":[{\"target\":\"a\"}]}},"
                                   "{\"id\":\"b\",\"label\":\"Second\","
                                   "\"launch\":{\"candidates\":[{\"target\":\"b\"}]}},"
                                   "{\"id\":\"c\",\"label\":\"Third\","
                                   "\"launch\":{\"candidates\":[{\"target\":\"c\"}]}}]"));
        if (!r.ok)
            return 20;
        if (r.config.items[0].label != L"First")
            return 21;
        if (r.config.items[1].label != L"Second")
            return 22;
        if (r.config.items[2].label != L"Third")
            return 23;
    }

    {
        auto r =
            ParseConfiguration(Doc("\"items\":[{\"id\":\"a\",\"quickKey\":\"k\","
                                   "\"launch\":{\"candidates\":[{\"target\":\"a\"}]}}]"));
        if (!r.ok || r.config.items[0].quickKey != L'K')
            return 140;
        if (ParseConfiguration(Doc("\"items\":[{\"id\":\"a\",\"quickKey\":\"AB\","
                                   "\"launch\":{\"candidates\":[{\"target\":\"a\"}]}}]"))
                .ok) {
            return 141;
        }
    }

    // Submenus map recursively and preserve child order.
    {
        auto r = ParseConfiguration(
            Doc("\"interaction\":{\"submenuDwellMs\":375},"
                "\"items\":[{\"id\":\"tools\",\"label\":\"Tools\","
                "\"children\":["
                "{\"id\":\"calc\",\"label\":\"Calculator\","
                "\"launch\":{\"candidates\":[{\"target\":\"calc.exe\"}]}},"
                "{\"id\":\"notes\",\"label\":\"Notes\","
                "\"launch\":{\"candidates\":[{\"target\":\"notepad.exe\"}]}}]}]"));
        if (!r.ok)
            return 51;
        if (!r.config.items[0].IsSubmenu())
            return 52;
        if (r.config.items[0].children.size() != 2)
            return 53;
        if (r.config.items[0].children[1].label != L"Notes")
            return 54;
        if (r.config.interaction.submenuDwellMs != 375)
            return 55;
    }

    // Built-in tools are typed actions and round-trip without launch targets.
    {
        auto r = ParseConfiguration(
            Doc("\"items\":[{\"id\":\"timer\",\"label\":\"Timer\","
                "\"builtin\":\"timer\"},{\"id\":\"system\",\"label\":\"System\","
                "\"builtin\":\"systemGlance\"},{\"id\":\"mirror\","
                "\"builtin\":\"mirror\"},{\"id\":\"mic\",\"builtin\":\"micCheck\"}]"));
        if (!r.ok)
            return 73;
        if (r.config.items[0].builtInTool != orbitdeck::BuiltInTool::Timer ||
            r.config.items[1].builtInTool != orbitdeck::BuiltInTool::SystemGlance ||
            r.config.items[2].builtInTool != orbitdeck::BuiltInTool::Mirror ||
            r.config.items[3].builtInTool != orbitdeck::BuiltInTool::MicCheck) {
            return 74;
        }
        const auto roundTrip = ParseConfiguration(orbitdeck::SerializeConfiguration(r.config));
        if (!roundTrip.ok ||
            roundTrip.config.items[0].builtInTool != orbitdeck::BuiltInTool::Timer) {
            return 75;
        }
    }
    {
        auto r = ParseConfiguration(Doc("\"items\":[{\"id\":\"bad\",\"builtin\":\"unknown\"}]"));
        if (r.ok)
            return 76;
    }

    // A submenu cannot also launch, and ids are unique across the tree.
    {
        auto r =
            ParseConfiguration(Doc("\"items\":[{\"id\":\"tools\","
                                   "\"launch\":{\"candidates\":[{\"target\":\"x\"}]},"
                                   "\"children\":[{\"id\":\"child\","
                                   "\"launch\":{\"candidates\":[{\"target\":\"y\"}]}}]}]"));
        if (r.ok)
            return 56;
    }
    {
        auto r =
            ParseConfiguration(Doc("\"items\":[{\"id\":\"dup\",\"children\":[{\"id\":\"dup\","
                                   "\"launch\":{\"candidates\":[{\"target\":\"x\"}]}}]}]"));
        if (r.ok)
            return 57;
    }

    // Existing-window actions map to explicit runtime behavior.
    {
        auto r =
            ParseConfiguration(Doc("\"items\":[{\"id\":\"a\",\"existingWindowAction\":"
                                   "\"maximizeAndFocus\","
                                   "\"launch\":{\"candidates\":[{\"target\":\"a\"}]}}]"));
        if (!r.ok)
            return 24;
        if (!r.config.items[0].maximizeOnActivate)
            return 25;
    }
    {
        auto r =
            ParseConfiguration(Doc("\"items\":[{\"id\":\"a\",\"existingWindowAction\":"
                                   "\"openNewInstance\","
                                   "\"launch\":{\"candidates\":[{\"target\":\"a\"}]}}]"));
        if (!r.ok ||
            r.config.items[0].windowAction != orbitdeck::ExistingWindowAction::OpenNewInstance) {
            return 94;
        }
    }
    {
        auto r =
            ParseConfiguration(Doc("\"items\":[{\"id\":\"a\",\"existingWindowAction\":\"invalid\","
                                   "\"launch\":{\"candidates\":[{\"target\":\"a\"}]}}]"));
        if (!r.ok || r.config.items[0].windowAction != orbitdeck::ExistingWindowAction::Focus ||
            r.config.items[0].existingWindowAction != L"invalid") {
            return 95;
        }
    }
    {
        auto r = ParseConfiguration(
            Doc("\"items\":["
                "{\"id\":\"restore\",\"existingWindowAction\":\"restoreAndFocus\","
                "\"launch\":{\"candidates\":[{\"target\":\"a.exe\"}]}},"
                "{\"id\":\"toggle\",\"existingWindowAction\":\"toggleMinimize\","
                "\"launch\":{\"candidates\":[{\"target\":\"b.exe\"}]}}]"));
        if (!r.ok ||
            r.config.items[0].windowAction != orbitdeck::ExistingWindowAction::RestoreAndFocus ||
            r.config.items[1].windowAction != orbitdeck::ExistingWindowAction::ToggleMinimize) {
            return 100;
        }
    }

    // Corrupt JSON is rejected without throwing.
    {
        auto r = ParseConfiguration("{ this is not json ");
        if (r.ok)
            return 26;
        if (r.error.empty())
            return 27;
    }
    {
        const std::string oversized(4 * 1024 * 1024 + 1, ' ');
        auto r = ParseConfiguration(oversized);
        if (r.ok || r.error.empty())
            return 77;
    }

    // The embedded default configuration is itself valid and full.
    {
        auto r = ParseConfiguration(orbitdeck::DefaultConfigJson());
        if (!r.ok)
            return 28;
        if (r.config.items[0].label != L"Chrome")
            return 29;
        if (!r.config.items[0].maximizeOnActivate)
            return 30;
        if (r.config.items.size() != 8)
            return 39;
        if (!r.config.items[3].IsSubmenu() || r.config.items[3].children.size() != 5 ||
            !r.config.items[5].IsSubmenu() || !r.config.items[6].IsSubmenu() ||
            r.config.items[6].children.size() != 4 || !r.config.items[6].children[0].IsSubmenu() ||
            !r.config.items[7].IsSubmenu() || r.config.items[7].children.size() != 8) {
            return 139;
        }

        const auto roundTrip = ParseConfiguration(orbitdeck::SerializeConfiguration(r.config));
        if (!roundTrip.ok)
            return 58;
        if (roundTrip.config.items.size() != r.config.items.size() ||
            roundTrip.config.items[0].id != L"chrome" ||
            roundTrip.config.items[0].target != r.config.items[0].target) {
            return 59;
        }

        const auto preserved = orbitdeck::SerializeConfigurationPreserving(
            r.config,
            "{\"schemaVersion\":1,\"customRoot\":\"keep\","
            "\"appearance\":{\"theme\":\"custom\"},"
            "\"items\":[{\"id\":\"chrome\",\"customItem\":\"keep\","
            "\"launch\":{\"candidates\":["
            "{\"type\":\"appPath\",\"target\":\"old.exe\"},"
            "{\"type\":\"shell\",\"target\":\"fallback\"}]}}]}");
        if (!preserved.ok || preserved.json.find("\"customRoot\": \"keep\"") == std::string::npos ||
            preserved.json.find("\"customItem\": \"keep\"") == std::string::npos ||
            preserved.json.find("\"target\": \"chrome\"") == std::string::npos ||
            preserved.json.find("\"theme\": \"orbitDark\"") == std::string::npos) {
            return 60;
        }

        const auto oneCandidate = ParseConfiguration(Doc(kOneItem));
        const auto removedFallback = orbitdeck::SerializeConfigurationPreserving(
            oneCandidate.config,
            "{\"schemaVersion\":1,\"items\":[{\"id\":\"a\","
            "\"launch\":{\"candidates\":[{\"target\":\"old\"},"
            "{\"target\":\"stale-fallback\"}]}}]}");
        if (!removedFallback.ok ||
            removedFallback.json.find("stale-fallback") != std::string::npos) {
            return 101;
        }

        const auto invalidPreserved =
            orbitdeck::SerializeConfigurationPreserving(r.config,
                                                        "{\"schemaVersion\":1,"
                                                        "\"appearance\":{\"theme\":\"#XYZ\"},"
                                                        "\"items\":[{\"id\":42}]}");
        if (invalidPreserved.ok || invalidPreserved.error.empty())
            return 61;

        const auto newerSchema = orbitdeck::SerializeConfigurationPreserving(
            r.config, "{\"schemaVersion\":2,\"items\":[]}");
        if (newerSchema.ok || newerSchema.error.empty())
            return 63;
        const auto arrayRoot = orbitdeck::SerializeConfigurationPreserving(r.config, "[]");
        if (arrayRoot.ok || arrayRoot.error.empty())
            return 64;
        const auto missingSchema =
            orbitdeck::SerializeConfigurationPreserving(r.config, "{\"items\":[]}");
        if (missingSchema.ok || missingSchema.error.empty())
            return 65;
    }

    // Experimental Left-Shift-only activation is opt-in and carries a threshold.
    {
        auto r =
            ParseConfiguration(Doc("\"activation\":{\"experimentalLeftShiftOnly\":true,"
                                   "\"leftShiftHoldThresholdMs\":333}," +
                                   std::string(kOneItem)));
        if (!r.ok)
            return 31;
        if (!r.config.activation.experimentalLeftShiftOnly)
            return 32;
        if (r.config.activation.leftShiftHoldThresholdMs != 333)
            return 33;
    }

    // Bad experimental activation fields are rejected.
    {
        auto r = ParseConfiguration(
            Doc("\"activation\":{\"experimentalLeftShiftOnly\":\"yes\"}," + std::string(kOneItem)));
        if (r.ok)
            return 34;
    }
    {
        auto r = ParseConfiguration(
            Doc("\"activation\":{\"leftShiftHoldThresholdMs\":-1}," + std::string(kOneItem)));
        if (r.ok)
            return 35;
    }

    // Placement, background dimming, and visible interaction controls map.
    {
        auto r =
            ParseConfiguration(Doc("\"placement\":{\"mode\":\"monitorCenter\"},"
                                   "\"appearance\":{\"scrimOpacity\":0.25,"
                                   "\"wheelDiameterDip\":640,\"outerRadiusDip\":280,"
                                   "\"innerRadiusDip\":120,\"selectionThresholdDip\":118,"
                                   "\"neutralThresholdDip\":90,\"theme\":\"#7ab6ff\"},"
                                   "\"interaction\":{\"cancelOnRightClick\":false,"
                                   "\"cancelOnOuterExit\":false,\"numberQuickKeys\":false,"
                                   "\"outerExitRadiusDip\":500,\"submenuDwellMs\":425}," +
                                   std::string(kOneItem)));
        if (!r.ok)
            return 41;
        if (r.config.placement.mode != orbitdeck::PlacementMode::MonitorCentre)
            return 42;
        if (r.config.appearance.scrimOpacity != 0.25F)
            return 43;
        if (r.config.appearance.wheelDiameterDip != 640.0F ||
            r.config.appearance.outerRadiusDip != 280.0F ||
            r.config.appearance.innerRadiusDip != 120.0F ||
            r.config.appearance.selectionThresholdDip != 118.0F ||
            r.config.appearance.neutralThresholdDip != 90.0F ||
            r.config.appearance.theme != L"#7ab6ff") {
            return 96;
        }
        if (r.config.interaction.cancelOnRightClick || r.config.interaction.cancelOnOuterExit ||
            r.config.interaction.numberQuickKeys ||
            r.config.interaction.outerExitRadiusDip != 500.0F ||
            r.config.interaction.submenuDwellMs != 425) {
            return 44;
        }
    }

    // Invalid placement and interaction values are rejected.
    {
        auto r = ParseConfiguration(
            Doc("\"placement\":{\"mode\":\"somewhere\"}," + std::string(kOneItem)));
        if (r.ok)
            return 45;
    }
    {
        auto r = ParseConfiguration(Doc("\"placement\":{\"mode\":42}," + std::string(kOneItem)));
        if (r.ok)
            return 48;
    }
    {
        auto r = ParseConfiguration(Doc("\"placement\":{\"mode\":\"\"}," + std::string(kOneItem)));
        if (r.ok)
            return 49;
    }
    {
        auto r = ParseConfiguration(
            Doc("\"interaction\":{\"outerExitRadiusDip\":100}," + std::string(kOneItem)));
        if (r.ok)
            return 46;
    }
    {
        auto r = ParseConfiguration(
            Doc("\"interaction\":{\"numberQuickKeys\":\"yes\"}," + std::string(kOneItem)));
        if (r.ok)
            return 47;
    }
    {
        auto r = ParseConfiguration(
            Doc("\"interaction\":{\"submenuDwellMs\":50}," + std::string(kOneItem)));
        if (r.ok)
            return 50;
    }
    {
        auto r =
            ParseConfiguration(Doc("\"appearance\":{\"selectionThresholdDip\":400,"
                                   "\"outerRadiusDip\":450,\"innerRadiusDip\":100,"
                                   "\"wheelDiameterDip\":920},"
                                   "\"interaction\":{\"outerExitRadiusDip\":300}," +
                                   std::string(kOneItem)));
        if (r.ok)
            return 103;
    }
    {
        auto r = ParseConfiguration(Doc("\"activation\":42," + std::string(kOneItem)));
        if (r.ok)
            return 66;
    }
    {
        auto r = ParseConfiguration(Doc("\"appearance\":false," + std::string(kOneItem)));
        if (r.ok)
            return 67;
    }

    // Portable export is atomic and reuses the same bounded validator on import.
    {
        auto parsed = ParseConfiguration(orbitdeck::DefaultConfigJson());
        if (!parsed.ok)
            return 97;
        wchar_t folder[MAX_PATH]{};
        wchar_t path[MAX_PATH]{};
        if (::GetTempPathW(MAX_PATH, folder) == 0 ||
            ::GetTempFileNameW(folder, L"odc", 0, path) == 0) {
            return 98;
        }
        const std::wstring sidecar = std::wstring(path) + L".new";
        const HANDLE sidecarFile = ::CreateFileW(sidecar.c_str(), GENERIC_WRITE, 0, nullptr,
                                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (sidecarFile == INVALID_HANDLE_VALUE)
            return 102;
        (void)::CloseHandle(sidecarFile);
        const auto exported = orbitdeck::ExportConfigurationFile(parsed.config, path);
        const auto imported = orbitdeck::LoadConfigurationFile(path);
        const bool sidecarPreserved =
            ::GetFileAttributesW(sidecar.c_str()) != INVALID_FILE_ATTRIBUTES;
        (void)::DeleteFileW(path);
        (void)::DeleteFileW(sidecar.c_str());
        if (!exported.ok || !imported.ok || !sidecarPreserved ||
            imported.config.items.size() != parsed.config.items.size() ||
            imported.config.appearance.wheelDiameterDip !=
                parsed.config.appearance.wheelDiameterDip) {
            std::fwprintf(stderr,
                          L"export=%d (%ls) import=%d (%ls) sidecar=%d items=%zu/%zu "
                          L"wheel=%.1f/%.1f\n",
                          exported.ok, exported.error.c_str(), imported.ok, imported.error.c_str(),
                          sidecarPreserved, imported.config.items.size(),
                          parsed.config.items.size(), imported.config.appearance.wheelDiameterDip,
                          parsed.config.appearance.wheelDiameterDip);
            return 99;
        }
        const auto reserved = orbitdeck::ExportConfigurationFile(
            parsed.config, orbitdeck::ConfigFilePath() + L".previous");
        if (reserved.ok)
            return 105;
    }

    std::puts("Configuration tests passed");
    return 0;
}
