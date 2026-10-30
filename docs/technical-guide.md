# Weapon Cycle

For code structure, ownership/error-handling rules and enforceable formatting
commands, see [Contributing](../CONTRIBUTING.md).

Weapon Cycle is a lightweight native Windows radial action launcher. Its primary
interaction is one gesture: hold `Shift+Space`, move toward an action, then
release. It runs locally as a standard user, uses no telemetry or network
service, and performs no hidden polling or capture.

The Windows GUI displays a cursor-monitor-aware, no-activate DirectComposition
palette: separate rounded action tiles, a quiet central hub, a high-contrast
mint selection, and connected outer-arc submenus. Clockwise order, radial/angular
selection, custom quick keys, and existing configurations are preserved.
Items can launch/focus apps,
run shell commands, copy snippets, open URLs/files, send explicit shortcuts,
invoke Windows tools, or open local utility popovers.

The native dark Preferences window separates **Your actions**, **Look & behavior**,
and **Installed apps**, with an embedded live preview. It supports same-level drag reordering,
file/folder drops, add/remove/disable, installed desktop and packaged-app
discovery with check-to-add, a curated starter pack, typed action editing,
hotkey/placement/appearance customization, color selection, and an embedded
live preview. The developer bench includes JSON, Base64, GUID, JWT, URL,
timestamp, text transform, Base64 image, number base, color, hash, regex, and
text diff tools. The native conversion UI now uses an app-local headless engine
built from Filee.Core and Filee.Engines: images (including HEIC), PDF, Word,
spreadsheets, presentations, media, archives, data, e-books, fonts, CAD and more.
The Filee desktop application is not needed. The larger engines/runtime live
in the portable `conversion` folder and are loaded only for explicit work;
the resident launcher remains native C++.

Opening the executable now opens Preferences instead of appearing to do nothing.
Use `--background` for quiet tray startup. Windows startup entries created by
the app include that flag; existing user configuration is not reset.

Shortcut handling uses release notifications while a gesture is visible, with
the visible timer as a fallback. Icon decoding and Shell icon extraction run on
one short-lived, below-normal-priority worker, never in the shortcut/render path.
Device failures are retryable on the next invocation; lock, sleep, and interrupted
gestures cancel rather than leaving a stuck wheel or committing a stale action.

## Build

Prerequisites:

- Windows 10 or 11, x64
- Visual Studio 2022 or 2026 with Desktop development with C++
- Windows 10 or 11 SDK
- CMake 3.25 or newer for VS 2022; CMake 4.2 or newer for VS 2026

From a Developer PowerShell:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

For the portable optimized build:

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

For Visual Studio 2026, use `windows-2026-debug` or
`windows-2026-release` in all three commands. Their executables are in
`out\build\windows-2026-debug\Debug` and
`out\build\windows-2026-release\Release`.
If the checkout was moved and CMake reports an old source/cache path, configure
with `cmake --fresh --preset <your-preset>` rather than reusing that cache.

### Complete portable converter

The native build alone contains basic Windows imaging, not the complete
conversion engine. With the local Filee source checkout and .NET 10 SDK, build
the app-local engine:

```powershell
dotnet publish conversion\WeaponCycle.Conversion.csproj -c Release -r win-x64 --self-contained true -p:FileeSource=C:\Projects\Filee -o out\portable\conversion
```

`build\Build-Portable.ps1` packages the native Release app, self-contained
engine/runtime, rhwp, 7-Zip, FFmpeg, and their notices. It accepts `-FileeSource`,
`-Preset`, and `-Engines`, and applies the narrow
`conversion\Filee-tiff-depth.patch` to the Filee source if needed. That correction
preserves palette PNG color/alpha when writing TIFF. For a manual publish from
a clean Filee checkout, apply that patch with Git first. Use the script only
where script execution is allowed; do not
change machine security policy just to run the build. Native-only CMake builds
continue to work without .NET.

Distribute the **entire `out\portable` folder**, not just `WeaponCycle.exe`.
The portable package needs no installed .NET runtime or separate Filee app.
Additional large engine packages are downloaded only after the user explicitly
chooses **Add required engine**; download sizes are shown first.

Run from PowerShell with
`.\out\build\windows-debug\Debug\OrbitDeck.exe` (or from `cmd.exe` without the
leading `.\`). The Release target uses the
Windows GUI subsystem and the static MSVC runtime, so no console or separately
installed C++ runtime is required.

On managed PCs, CTest can transiently report
`BAD_COMMAND [operation not permitted]` for a freshly linked executable even
when direct PowerShell execution succeeds. Run the reported test `.exe` from
PowerShell and retry after endpoint scanning settles.

For a development console, configure with
`-DORBITDECK_DIAGNOSTIC_CONSOLE=ON`. It remains off in the supplied presets.

## Current controls

- Hold `Shift+Space`: the neutral wheel fades and scales in without taking focus.
- If `activation.experimentalLeftShiftOnly` is enabled, hold Left Shift alone
  past `activation.leftShiftHoldThresholdMs` (default `240`) to open the wheel.
  Short taps, Right Shift, and Left Shift followed by normal typing before the
  threshold do not open it.
- Move beyond the configured selection radius (default 82 DIPs) from its
  displayed centre: select a direction clockwise from top; moving inside the
  configured neutral radius (default 50 DIPs) clears the selection. The light
  selected wedge cross-fades with readable content and projects only its outer
  edge rather than scaling the entire wheel segment.
- If edge clamping places the starting cursor outside that radius, move into the
  wheel once to arm selection; it will not auto-select under a stationary cursor.
- Release Shift or Space: play the short commit closing animation when a wedge is
  selected, or the cancel animation otherwise, then hide. Committing an item with
  a configured target applies its existing-window policy (`focus`,
  `restoreAndFocus`, `maximizeAndFocus`, `openNewInstance`, `doNothing`, or
  `toggleMinimize`), otherwise tries its launch candidates in order once the
  overlay is gone. Schema-v1 `focus` retains the original restore-if-minimized
  behavior, and `restoreAndFocus` is its explicit alias. Foreground denial
  flashes the taskbar; releasing with nothing selected does nothing.
- `Escape`: play the cancel animation and close without launching or activating
  another window.
- Right-click or move past the configured outer radius: cancel.
- Dwell on a submenu to fan its children into a connected outer arc. Deeper
  submenu levels continue outward without replacing the learned parent wheel.
- Press `1`–`9`, or a segment's configured single letter/number, to activate a
  visible item immediately.
- In tray-opened mode or Preferences' **Try saved wheel**, press `Enter` to
  commit the highlighted item. Escape cancels. The live preview returns when
  the gesture finishes, and the global shortcut works over Preferences too.
- Single-click, double-click, or keyboard-activate the tray icon: open Preferences.
- Right-click the tray icon: open the Weapon Cycle menu.
- `Start with Windows` in the tray menu: add or remove the current executable
  from the current user's standard Windows startup list.
- Exit: unregister the shortcut, remove the tray icon, and stop cleanly.

With either the Weapon Cycle reduced-motion setting or the Windows animation
preference disabled, these transitions become shorter and opacity-only.

The Left-Shift-only mode uses asynchronous Windows Raw Input and is disabled by
default because it can conflict with typing, text selection shortcuts, games,
and accessibility features. It does not log, buffer, suppress, inject, or record
keystrokes, and it unregisters on reload, pause, and shutdown.
The normal shortcut registers Raw Input only while a held gesture is visible,
for release/Escape notifications. It unregisters when the gesture closes.
There is no system-wide keyboard or mouse hook: a slow render cannot hold up
Windows' input-hook chain.

## File conversion

**Select one file in Explorer and press `Ctrl+Alt+C`.** Weapon Cycle opens its
converter with that file and its real available output formats. This is a
registered shortcut, not background file monitoring. A shortcut conflict is
reported; the picker/drop alternatives remain available.

Other entry points are **Convert a File** in the tray, **Convert a file** in
Preferences, a **File Converter** wheel action, dropping a file into the
converter, or dropping it onto `WeaponCycle.exe`. Command-line activation is
`WeaponCycle.exe --convert "C:\path\file.docx"`; it hands the file to the existing
instance. Existing wheel configuration is preserved.

For a right-click entry, choose **Explorer conversion menu** in the tray and
confirm. **Convert with Weapon Cycle** appears under **Show more options** on
Windows 11. This affects only the current user and does not change default
file associations. A normal double-click still opens your normal viewer; it
never converts or overwrites files automatically.

Choose a target format and select **Convert**. Typical routes include:

| Input | Example outputs |
|---|---|
| JPG, PNG, HEIC/HEIF, WebP, TIFF, GIF, AVIF, camera RAW | PNG, JPG, WebP, TIFF, AVIF, PDF and other image formats |
| DOCX, DOCM, text, Markdown, HTML | PDF, DOCX, TXT, HTML, EPUB |
| PDF | DOCX, TXT, PNG/JPG page images, TIFF and other supported routes |
| XLSX, XLS, CSV, TSV, ODS | CSV, XLSX, ODS, JSON, HTML, PDF |
| PPTX and related presentations | PDF, PNG/JPG, DOCX |
| Video and audio | MP4, WebM, MOV, MP3, WAV, FLAC and other media formats |
| ZIP, 7Z, TAR, RAR and other readable archives | Extract to a folder, ZIP, 7Z, TAR; any file can be packed |
| JSON/YAML/XML/TOML, subtitles, e-books, fonts, CAD, 3D and geospatial files | Supported targets discovered from the same conversion graph |

Not every pair is meaningful or available. The picker shows working routes and
labels routes requiring an extra engine. Legacy Word `.doc`, OpenDocument, some
presentation exports, and special text formats may need LibreOffice or Pandoc;
calibre and Ghostscript enable additional e-book/PostScript routes. These can be
added inside the converter without installing Filee. The base package includes
the image codecs, rhwp document renderer, 7-Zip, and FFmpeg.

Conversion runs at below-normal priority in an isolated, on-demand process.
Progress and explicit failure/cancellation messages stay in the native window.
Completed outputs are published together in a new **`name (converted)` folder
beside the source**, adding a number on collision. **Show result** selects the
first result in Explorer. Original and existing files are never overwritten.
Private staging folders end in `.partial`; failure/cancellation removes the
job's staging folder, while an abrupt process kill may leave an identifiable
partial folder, never a completed-looking result.

Files are bounded to 2 GiB in this interactive workflow. ImageMagick's bundled
build uses 8-bit channels; do not use it as a lossless HDR/high-bit-depth
archival workflow. PDF-to-Word reconstructs content and may change complex
layout. Scanned PDFs are not automatically OCR'd, and substituted fonts are
reported. Unsupported/corrupt/password-protected inputs produce explicit errors.
This interface converts one file at a time; it does not copy Filee's editor,
watch-folder, or batch/preset UI.

If the `conversion` folder is absent, a clear warning identifies basic Windows
imaging mode. That fallback still has its original four output formats and
Windows-codec limitations; it is not advertised as full conversion.

### Conversion checks

```powershell
ctest --preset windows-2026-release
dotnet run --project tests\ConversionEngine.Smoke\ConversionEngine.Smoke.csproj -c Release -- out\portable
```

The smoke runner creates its own files, verifies actual JPG/PNG/HEIC, WebP,
DOCX/PDF, spreadsheets, JSON/YAML, archive and audio conversions, reads back
content, checks collision/original safety, and exercises Unicode/read-only/locked
input, transparency, EXIF rotation, animation/multipage output, protected/corrupt
input, archive containment and cancellation. It also checks 1,600
availability/shaping combinations against the full route planner.
It removes only its own temporary fixture directory.

### Responsiveness and performance checks

Pointer hit testing is allocation-free, quick-key mappings are prepared once
per menu, and DirectWrite layouts/glyph strings are reused instead of reshaped
on every selection frame. Progress controls are updated only when values change,
and UI control calls do not hold the worker's progress lock. A conversion probes
only the engines needed by its route, retaining an available complex-script
alternative; format discovery still checks the full capability graph.

The bounded, sequential benchmark includes process startup and checks output
dimensions/original hashes:

```powershell
dotnet run --project tests\ConversionEngine.Smoke\ConversionEngine.Smoke.csproj -c Release -- out\portable --benchmark C:\Temp\conversion-performance.json
```

Create the report's parent directory first. This is not a GPU/fullscreen stress
test and does not measure physical mouse-to-display latency. The 2026-10-26
results and unverified hardware scenarios must be recorded before a release.
After the reported blank-screen incidents, the performance pass deliberately
uses headless/CPU tests and does not auto-start the overlay or change drivers.

## Configuration

On first run Weapon Cycle writes `%LOCALAPPDATA%\OrbitDeck\config.json`.
The compatibility path keeps existing installations intact. The curated
eight-segment root contains Chrome, Spotify, ChatGPT, and nested Work, Windows,
Utilities, Developer, and PowerToys groups.

- Add, remove, or reorder objects in the `items` array to change the wheel
  without rebuilding OrbitDeck. The first object is the top segment and the
  remaining objects continue clockwise. The wheel accepts 1–12 items; the limit
  is a rendering/input safety ceiling rather than a fixed app list.
- Each item's `label`, optional one-character `quickKey`, `icon.path`, `enabled`
  flag, and
  `existingWindowAction` are honored. Supported window actions are `focus`,
  `restoreAndFocus`, `maximizeAndFocus`, `openNewInstance`, `doNothing`, and
  `toggleMinimize`. Entries in `launch.candidates` are tried in order; `type` is
  `aumid` for a packaged app. Typed candidates also support `shellCommand`,
  `clipboard`, `url`, `file`, `shortcut`, and `system`.
  Unknown schema-v1 window-action strings retain their legacy focus behavior and
  are preserved until explicitly changed in Preferences.
- A disabled item remains visible at reduced opacity but cannot be selected or
  launched. Environment variables such as `%LOCALAPPDATA%` are expanded in
  configured paths.
- An item may use `children` instead of `launch` to define a recursive submenu.
  Each level supports 1–12 items, up to four levels and 128 total items.
- Built-ins include File Converter, Keep Awake, Scratchpad, System Glance, Mirror, Mic Check,
  Color Picker, and the developer bench tools. Clock/Timer, Focus, Calculator,
  Magnifier, Notepad, Settings, Snipping Tool, and Screenshots use supported
  Windows targets instead of duplicate implementations.
- `activation.hotkey` (default `Shift+Space`) sets the supported activation
  chord. `activation.experimentalLeftShiftOnly` enables the opt-in Raw Input path,
  and `activation.leftShiftHoldThresholdMs` sets its hold threshold.
  `appearance` configures reduced motion, backdrop opacity, accent/theme, wheel
  diameter, inner/outer radii, and neutral/selection thresholds. These values are
  editable with a live preview in Preferences.
- `placement.mode` accepts `cursor` or `monitorCenter`.
- `interaction` configures right-click cancellation, outer-exit cancellation,
  number quick keys, outer radius, and submenu dwell time.

The file is re-read when the wheel next opens if it changed on disk, and the tray
menu offers `Preferences`, `Reload Configuration`, `Open Configuration Folder`,
and `Start with Windows`. Preferences imports and exports validated JSON through
native Windows dialogs, writes through a per-user lock, preserves unknown JSON
fields and launch candidates, rejects concurrent edits, and offers an explicit
confirmed reset for invalid configuration. If an edit is
invalid or the file is corrupt, OrbitDeck keeps the previous configuration and
shows a tray balloon explaining why. Parsing uses a vendored
`nlohmann/json` single header (compile-time only; the executable has no runtime
dependency).

### Add or reorder a custom application

Each app is data, not a compiled slot. Add an object like this anywhere in the
`items` array, then save the file:

```json
{
  "id": "my-app",
  "label": "My App",
  "enabled": true,
  "icon": {
    "type": "auto",
    "path": "C:\\Tools\\MyApp\\MyApp.exe"
  },
  "launch": {
    "candidates": [
      {
        "type": "path",
        "target": "C:\\Tools\\MyApp\\MyApp.exe"
      }
    ]
  },
  "existingWindowAction": "focus"
}
```

Move that object within the array to change its direction. Delete it to remove
the segment, or set `enabled` to `false` to leave it visible but inactive. Use
the tray menu's `Reload Configuration` command for an immediate explicit reload;
otherwise the next wheel open detects the saved change.

## Current smoke check

After automated tests:

1. Start OrbitDeck twice; confirm one tray icon/process and no console.
2. Open Preferences from the tray. Add, remove, disable, and reorder apps; add a
   submenu; drag to reorder siblings; drop a file; check installed apps; save
   and confirm the next wheel reflects the exact hierarchy/order.
3. Verify cursor and monitor-center placement on primary, negative-coordinate,
   and mixed-DPI monitors.
4. Verify pointer release, quick keys, and tray-preview `Enter` commit only enabled
   leaf actions; disabled items and submenu parents never launch.
5. Verify center return, Escape, right-click, and outer-exit cancellation, plus
   submenu dwell and connected outer-arc navigation.
6. Confirm the wheel never activates itself and hides before app focus/launch.
7. Add each utility through Preferences. Verify Keep Awake releases after
  removal/shutdown, Scratchpad survives restart/recovery, System Glance stops
  polling when hidden, Color Picker samples only while visible, developer
  transforms stay bounded, and Mirror/Mic Check release hardware on close.
8. Pin/unpin and move a tool; verify topmost state and no taskbar entry.
9. Edit `config.json` externally while Preferences is open; confirm Save rejects
   the conflict. Export, import, then corrupt it and recover with Import or the
   confirmed Reset defaults action.
10. Verify Explorer restart restores the tray icon, sleep/wake leaves no stuck
   wheel, and Exit releases hotkeys, timers, camera, microphone, and power state.
11. Toggle `Start with Windows`; confirm the tray checkmark follows the actual
   HKCU state, then disable it after the test.

## Privacy and managed devices

Weapon Cycle converts files entirely on the local computer. Optional engine
downloads contact their pinned distribution hosts only after explicit consent;
selected files are never uploaded. It has no telemetry, database, input
recording, service, driver, or elevation request.
Mirror, Mic Check, and Color Picker access camera/audio/screen pixels only while
their visible tool is open; nothing is recorded or persisted. Shortcut actions
use `SendInput` only after explicit user configuration and wheel activation.
Installation and startup use may still be
restricted by an employer's security policy; obtain approval before running
unapproved software on a managed device.

See [architecture](architecture.md), [security policy](../SECURITY.md), and
[third-party notices](../THIRD_PARTY_NOTICES.md).

## License

Weapon Cycle source is available under the MIT License. See [LICENSE](../LICENSE).
