# Weapon Cycle architecture

## Process model

Weapon Cycle is one standard-user, Unicode Win32 GUI process. The main thread owns
all HWND, tray, input, state-machine, and graphics work. It blocks in
`GetMessageW` while hidden; no hidden-state timer, cursor polling, process scan,
screen capture, or network activity exists. Finite icon preparation is dispatched
once per changed source set on a short-lived worker; it never blocks invocation.
Explicit utility work is the other exception: an active conversion, countdown,
Keep Awake request, pinned System Glance, camera
preview, or microphone meter owns only the resources its visible/running state
requires.

Phase 7 window activation also runs synchronously on the main thread: on commit,
after the overlay hides, `EnumWindows` finds the best existing window for the
target and restores/maximizes/focuses it before any launch, so it needs no
worker. A short-lived worker for bounded *post-launch* window discovery (waiting
for a just-launched app to create its window) remains a deferred option, not yet
required. Launching likewise runs synchronously through ordered
`ShellExecuteEx` candidates posted after the overlay hides, with `SEE_MASK_ASYNCOK`
instead of forcing synchronous Shell handoff. Results return to the main
thread through custom window messages. There is no service, injected module,
permanent worker pool, or always-running second runtime. File conversion starts
a packaged headless helper only for explicit work and never opens Filee's UI.
Phase 8 configuration loading
is also synchronous: the file is read and validated at startup, and re-read only
when the wheel opens if its last-write time changed — there is no watcher or
polling while hidden. Activation now uses asynchronous keyboard Raw Input:
normal activation registers `RIDEV_INPUTSINK` only while a held wheel is visible;
the experimental Left-Shift-only mode registers it while explicitly enabled
and owns one timer during a pending hold. There is no `WH_KEYBOARD_LL` or mouse
hook and no synchronous input-chain dependency on rendering. Messages carry
a gesture token so a late release cannot close the next gesture. The visible
16 ms timer remains a fallback, and every normal-gesture hide unregisters input.
Lock/disconnect/suspend/resume and a one-second tracking interruption cancel
the overlay and clear held state.

```text
wWinMain
  └─ OrbitDeckApp
      ├─ SingleInstanceCoordinator
      ├─ MainMessageWindow
      ├─ ActivationController
      ├─ TrayIconController
      ├─ OverlayController
      ├─ WheelLayout + SelectionEngine
      ├─ SettingsWindow
      ├─ MiniAppWindow
      ├─ DeveloperToolWindow
      └─ FileConversionWindow
```

The app owns the runtime `Configuration` and a separate preview snapshot.
Deferred icon redraws never borrow items from the mutable editor draft.
`ParseConfiguration` validates
bounded depth/count/size, `SettingsWindow` edits the same model, and persistence
preserves unknown JSON fields and ordered fallback candidates while using a per-user
cross-session lock, content revision checks, durable files, and recoverable
generations. Invalid reloads retain the last-known-good runtime state; Reset
requires explicit confirmation. Validated import commits the exact imported JSON;
export merges edited supported fields into the current valid document so
extension fields survive.

Concrete classes are owned directly. Interfaces are introduced only if a
second implementation or a test seam proves necessary.
`platform\UniqueHandle.h` provides move-only ownership for `CloseHandle`
resources; COM and HWND/GDI resources retain their appropriate lifecycle APIs.
The settings action and existing-window choices share one catalog in
`settings\ActionChoices.h`, including legacy mappings, so dropdown labels and
model editing cannot drift independently.

Coding conventions, formatting and warning policies are maintained in
[`CONTRIBUTING.md`](../CONTRIBUTING.md). The native app and test targets share
one `OrbitDeckBuildOptions` contract instead of repeating compiler settings.

## Target event and state flow

The hidden message window receives `WM_HOTKEY`, tray callbacks, Explorer's
registered `TaskbarCreated` message, the single-instance activation broadcast,
and internal app messages. The thin static window procedure retrieves its
owner from `GWLP_USERDATA` and routes messages to `OrbitDeckApp`.

```text
Dormant
  ├─ registered shortcut ──────────────┐
  └─ optional threshold → HotkeyArmed ┤
                                      v
                                  Opening
                                      v
               ┌────────────── OpenNeutral
               │                      ⇅
               │                OpenSelected
               │                 ⇅ submenu dwell
               │              connected outer arc
       Escape/release neutral   release selected
               v                      v
       ClosingCancelled       ClosingCommitted
               │                      │
               v                 overlay hidden
            Dormant                   v
                         Activate, Launch, or Popover
                                      │
                          ┌───────────┴───────────┐
                          v                       v
                       Dormant              Popover/Pinned
```

Any recoverable graphics, configuration, or launch failure enters
`Recovering`, restores a usable fallback or last-known-good state, then returns
to `Dormant`. Shutdown unregisters input and removes the tray icon before the
message window is destroyed.

## Rendering strategy

Weapon Cycle uses a no-activate, topmost popup on only the cursor's monitor. Its
DirectComposition tree is a root visual with three `IDCompositionVisual3`
children: a stretched 8×8 scrim, a best-effort 256×256 wheel-centred radial
vignette, and a wheel-sized premultiplied surface. The wheel surface contains
rounded root tiles and cached geometry for every visible nested arc. Pure
`WheelLayout` maps recursive paths to angular/radial targets; dwell adds the next
layer without replacing its parents. Geometry and hit testing share that same
layout, including work-area-aware clamping after expansion. Selection cross-fades
between outgoing and incoming tiles with a small selected-tile expansion.
Content switches between contrast-safe palettes rather than passing through an
unreadable mid-gray. The scrim/vignette default to zero opacity; the selected
tile uses a light, non-colour-only treatment with dark content and quiet
quick-key badges. Geometry is rebuilt only when the wheel surface is
recreated; the surface is redrawn only during an opening/closing frame or an
active selection cross-fade. D3D11 hardware is preferred and WARP is the
fallback both at startup and after centralized device/composition recovery.

Each wedge also carries a DirectWrite label and an icon, and the hub carries
centre text that tracks the selection. Text formats use a font size scaled into
the same 96-DPI pixel contract as the geometry so text stays sharp across DPI,
and are recreated only when the surface is rebuilt. Icons come from a
device-independent `IconCache`. One below-normal-priority COM worker decodes
source changes (WIC image, executable icon, AppsFolder shell image), downsizes
them to at most 256 pixels, and posts detached pixel buffers. No COM object or
UI pointer crosses threads. Generation changes discard obsolete results, and
an unresponsive Shell call cannot block the UI or spawn an unbounded worker pool.
Drawing uses only cached sources or semantic fallback glyphs. URL/protocol
sources use semantic glyphs without binding the Shell to a remote image.
Device-dependent
Direct2D bitmaps and DirectWrite text layouts are cached as well, not recreated
or reshaped on every selection frame. Text caches invalidate on text, font and
bounding-box changes; glyph strings are prepared on layout changes.
A missing or
unreadable source falls back instead of crashing,
and selected versus unselected is a draw-time opacity emphasis on the one cached
icon rather than two stored bitmaps.

Animation is a pure, time-based lifecycle (`OverlayAnimation`) driven by
`QueryPerformanceCounter`: opening ramps opacity, scale, and a slight settling
rotation; commit and cancel closings use distinct end scales and capture the
current frame when interrupted. Selection uses symmetric smoothstep blending so
a rapid reversal continues from the visible mix. A reduced-motion path keeps
opacity-only transitions and is selected by either configuration or the Windows
animation preference. The controller only applies frames and commits composition; it
never measures time itself.

The visible-only cadence is the 16 ms animation clock. Each tick advances the
animation, checks pointer selection, submenu dwell, quick keys, activation-key
release, Escape, right-click, and outer-exit cancellation while not already
closing, applies opacity/transform, redraws any active cross-fade, and commits at
most once per frame. A steady, fully open wheel with no selection change draws
and commits nothing, and the cadence stops as soon as the overlay hides.
Selection hysteresis borrows a span of its existing segment rather than
allocating/copying a temporary layout. Quick virtual keys are translated once
per menu, not on each tick; submenu activation returns before a changed layout
can invalidate the rest of the key scan.

Preferences reuses this renderer in an embedded owned preview. It has no
input timer, follows/minimizes with Preferences, and redraws only when edited
content, appearance, DPI, or placement changes. Edits are debounced by a
visible-only 75 ms one-shot timer. Invoking the saved wheel temporarily replaces
the preview; it returns after the gesture without discarding the editing draft.

## File conversion

`FileConversionWindow` owns the native picker/drop/format/progress UI and one
cancellable worker. `Ctrl+Alt+C` reads one foreground Explorer/desktop selection
on that worker; an opt-in HKCU context-menu verb and `--convert` activation use
the same source path. `WM_COPYDATA` forwards bounded file paths to the existing
instance, never to an arbitrary shell command.

`ConversionEngine` starts the app-local `conversion\WeaponCycle.Conversion.exe`
with isolated stdin/stdout pipes and an explicit inherited-handle whitelist.
The process and its engine children belong to a kill-on-close Windows job.
JSON-lines messages report formats, progress, results and explicit errors.
`ConversionProtocol` owns byte framing and typed response validation separately
from the process/pipe transport. It accepts one terminal response, strictly
decodes UTF-8, validates field types and checks successful process exit status.
Native controls never receive a success-shaped fallback for malformed output.
Cancellation is cooperative through stdin, with a bounded forced-stop fallback;
only an active request owns a wait loop/timer. When the helper exits no managed
runtime or engine process remains resident.

The small .NET host references Filee.Core/Filee.Engines and their
existing graph planner, codecs and job queue. It does not depend on Filee.App
or its settings, does not index files, and never uploads documents. Native/WIC
Its entry point handles only startup, cancellation-listener lifetime and final
error reporting. `ConversionRequest` validates command shape,
`ConversionHost` coordinates the engine workflow, and `ConversionMessages`
serializes the typed wire contract. `OutputWorkspace` owns private staging,
contained outputs, collision-safe publication and disposal independently of
engine execution. These boundaries have headless contract/ownership tests. Native/WIC
four-format imaging remains an explicitly labeled fallback only if the complete
engine package is absent. The default complete package includes rhwp, 7-Zip and
FFmpeg; additional packages are downloaded only on request through pinned hashes.
The 7-Zip bootstrap handles the bundled 7-Zip MSI separately, since Filee's
optional MSI installer is specific to Office/calibre.
For an actual conversion, `ConversionCatalog` checks candidate routes lazily
instead of launching unrelated engines' availability/version probes. It retains
the available shaped-text alternative before JobQueue replans complex text.
Discovery still checks the full graph. The Filee source build includes a narrow
TIFF sample-depth correction, recorded in the redistribution notices.

All files for an advanced conversion are written beneath a uniquely reserved
`.weaponcycle-<id>.partial` sibling directory. Only after the job succeeds is
the complete result set moved non-replacing to a unique `name (converted)`
directory. Related files remain together; failures/cancellation remove only the
job's own directory. A forced process exit can leave that identifiable partial,
never a completed-looking output. Inputs are bounded to 2 GiB, ImageMagick
memory/disk/thread use is limited, and conversions run at below-normal priority.
A 125 ms progress/completion timer exists only while this window is visible and
work is active.
Progress snapshots are copied under the worker lock; native control updates
happen after releasing it and only when the displayed value changes.

The native launcher remains C++/Win32 without a managed UI. The full converter
necessarily has a larger disk footprint (app-local runtime, codecs and CLI
engines); memory/CPU cost is paid on demand instead of while the launcher is idle.

## Technical decisions

- Per-Monitor V2 is declared in the manifest and requested before HWND creation.
- COM uses a single-threaded apartment on the UI thread.
- `RegisterHotKey` is the default, global, message-driven activation.
- Asynchronous `WM_INPUT` delivers release/Escape events;
  experimental Left-Shift-only activation keeps its registration while enabled. It is
  opt-in, opens only after its hold threshold, cancels pre-threshold typing,
  suppresses nothing, and unregisters on pause/reload/shutdown.
- A session-local named mutex enforces one instance. A registered broadcast
  message asks the existing hidden top-level window to open.
- Start with Windows uses the current user's `Run` key and always derives the
  quoted executable path at runtime.
- Release builds use `/MT`, optimization, and link-time optimization.
- AppsFolder discovery runs only on the visible Installed apps page, on a cancellable
  COM worker that posts generation-tagged results to the UI.
- Typed leaf actions use one tagged candidate model: app/AUMID, URL/file, shell
  command, clipboard snippet, shortcut, system action, or built-in utility.
- Developer transformations run only on explicit `Run`, with size/complexity
  bounds, strict codecs, safe-regex restrictions, and atomic binary output.
- Application activation honors explicit focus/restore/maximize/new-instance/
  do-nothing/toggle-minimize policies and Windows foreground restrictions. It
  falls back to `FlashWindowEx` when foreground activation is denied and never
  injects input. Schema-v1 `focus` retains restore-if-minimized behavior, with
  `restoreAndFocus` accepted as its explicit alias. The
  chosen window is matched by owning-process image basename and picked by largest
  client area (most-recently-active on ties).

## Dependencies

The resident launcher uses C++20, Win32, and Windows SDK libraries. The
current build links `user32`, `shell32`, `ole32`, `d3d11`, `dxgi`, `d2d1`,
`dcomp`, `dwrite`, `windowscodecs` (WIC), `dwmapi`, `comctl32`, `uxtheme`,
`comdlg32`, `propsys`, `bcrypt`, `vfw32`, `winmm`, `wtsapi32`, and `shlwapi`.
The complete converter additionally ships an isolated .NET 10 helper, Filee's
managed/native conversion libraries and the selected CLI engines. Their source
revision and licenses are recorded in the portable package; no Filee desktop
runtime/UI is required and no engine process is kept resident after a request.
DirectWrite and WIC were added in Phase 5 for labels, centre text, and icons.
Launching reuses the already-linked `shell32`: a committed item is
resolved (trim, environment expansion, and the `shell:AppsFolder\<AUMID>` moniker
for packaged apps) and opened with ordered `ShellExecuteEx` verb-`open` attempts, which
natively covers executables, App Paths names, URLs, URI protocols, folders, and
files. Phase 7 existing-window activation adds `dwmapi` (for the
`DWMWA_CLOAKED` check that skips windows on other virtual desktops or suspended
apps) and uses `user32` `EnumWindows`/`ShowWindow`/`SetForegroundWindow`/
`FlashWindowEx` plus `QueryFullProcessImageNameW` to match a window to the
target's executable basename; it reads no process memory and never elevates.
Phase 8 vendors the `nlohmann/json` single header (`third_party/nlohmann`, MIT)
to parse the configuration; it is compile-time only and creates no runtime
dependency. The configuration is read with already-linked `shell32`/`ole32`
(`SHGetKnownFolderPath` for `%LOCALAPPDATA%`, `SHCreateDirectoryExW`) plus
standard-library file I/O. Raw Input and the opt-in hold timer use the already-linked
`user32`.

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| Hotkey already registered | Keep running, show one concise tray error, allow pause/reload |
| Windows denies foreground activation | Restore normally, then use `FlashWindowEx`; never fake input |
| Device removal or composition failure | Central resource teardown/recreation, then WARP/native fallback |
| Mixed-DPI or disconnected monitor | Monitor-local DPI/geometry; cancel and recover if rebuilding in place is unsafe |
| App installer/package variation | Ordered documented App Paths, Shell, URI, and AUMID candidates |
| Activation interferes with typing/accessibility | Shift-only disabled by default; asynchronous Raw Input, typing cancellation, no suppression |
| Memory pressure | Wheel-sized cached surfaces, bounded icon variants, no hidden cadence |
| Shell discovery stalls | Visible-settings-only cancellable worker; no startup or hidden scan |
| Untrusted utility input | Size/depth/codec/regex bounds and atomic file replacement |
| Overlay blocks target activation | Complete hide and stop cadence before posting launch work |

## Final-product acceptance gates

- Startup: one process, no console or elevation, tray present, invalid optional
  configuration non-fatal.
- Hidden: no overlay, timer, cursor poll, process scan, network connection, or
  measurable recurring work.
- Performance: hidden CPU delta is zero over a settled sample; steady open wheel
  remains below the 1% single-core budget on the target office PC.
- Activation and selection: configured shortcut, correct monitor, neutral
  start, dead-zone and angular hysteresis, disabled items non-committable.
- Commit/cancel: hide before action; restore/focus/maximize or launch selected
  targets; Escape/neutral release launches nothing.
- Visual: evenly distributed configured segments; sharp mixed-DPI output; unambiguous
  non-color-only selection; smooth and reduced-motion paths.
- Recovery: Explorer recreates the tray icon; missing apps/icons and ordinary
  graphics failures do not crash; shutdown leaves no tray icon, hotkey, or
  hook.
