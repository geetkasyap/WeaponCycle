# Contributing to Weapon Cycle

Keep the application native, local, event-driven and easy to trace. Prefer a
focused change to the existing concrete controllers over a new framework,
service layer or dependency. Preserve configuration compatibility and the
hold/point/release interaction unless a task explicitly changes them.

## Source boundaries

| Location | Responsibility |
|---|---|
| `src\app` | App coordination, configuration application and lifecycle |
| `src\input` | Registered hotkeys and asynchronous Raw Input; no global hooks |
| `src\overlay` | Rendering, cached resources, pure layout/selection/animation |
| `src\settings` | Native Preferences; `ActionChoices.h` is its action catalog |
| `src\platform` | Windows lifecycle and move-only native resource ownership |
| `src\tools\ConversionEngine.cpp` | Isolated helper process, pipes, timeout and cancellation |
| `src\tools\ConversionProtocol.*` | JSON-lines framing, schema/UTF-8 and result validation |
| `conversion\Program.cs` | Process startup and the final exception-reporting boundary |
| `conversion\ConversionHost.cs` | Format discovery, conversion and explicit engine setup |
| `conversion\OutputWorkspace.cs` | Private output staging and non-replacing publication |
| `tests` | Headless regressions and bounded real-file conversion checks |

The separate Filee checkout is a dependency, not a second UI implementation.
Keep its narrow TIFF-depth correction reproducible with
`conversion\Filee-tiff-depth.patch`; preserve its notices and record source
changes rather than describing a modified library as unmodified.

## Coding conventions

- Use four spaces, UTF-8 and final newlines. `.editorconfig` and
  `.clang-format` are the source of truth. Format first-party files only, never
  vendored headers, generated files or build artifacts.
- C++ follows the existing `PascalCase` types/functions, `camelCase` locals,
  `member_` private fields, `kConstant` constants and attached braces.
  C# uses file-scoped namespaces, `PascalCase` members/types, `_camelCase`
  instance fields, `s_camelCase` static fields and Allman braces.
- Prefer a short named function for one responsibility. Use a small catalog
  for repeated UI/model mappings; do not duplicate enum order, labels and
  serialization names in unrelated switches or nested conditional chains.
- Keep ownership explicit. `UniqueHandle` is move-only and closes only
  `CloseHandle` resources. It is not for HWND, registry keys, COM pointers or
  GDI objects, which have different cleanup APIs. Use WRL `ComPtr` for COM.
  A raw pointer/handle parameter is borrowed unless its API explicitly transfers
  ownership.
- Do not mark allocating or fallible helpers `noexcept` to hide errors.
  Windows callbacks must not leak exceptions; report recoverable failures at
  the appropriate worker/UI boundary. Catch only errors the layer can handle.
  The conversion process entry point is the deliberate final exception boundary:
  it writes diagnostics to stderr and a typed failure to the private pipe.
- Validate external input and helper responses before using them. A malformed
  response, invalid UTF-8, duplicate result or nonzero successful exit is an
  explicit failure, never an empty success. Preserve the JSON field names used
  by the native/managed protocol.
- Only the UI thread touches controls. Snapshot shared worker state under its
  lock, release the lock, then update controls. A worker must not capture an
  unowned window/controller pointer; retain shared job state and clear its target
  during destruction. Preserve cancellation and generation checks.
- Hidden means no recurring work. Never introduce global input hooks, hidden
  polling, a permanent engine process, synchronous Shell/decoder work in the
  pointer path, or avoidable allocations in selection updates.
- Preserve originals. Write only job-owned staging paths, publish a complete
  result set by non-replacing move, and delete only the job's own unfinished
  files. Keep companion files together.
- Comments explain non-obvious constraints or tradeoffs, not what the next line
  already says. Public behavior belongs in the README; ownership/state boundaries
  belong in `docs\architecture.md`.

These conventions adapt the [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines)
(especially RAII and single-responsibility functions), the
[.NET runtime style](https://github.com/dotnet/runtime/blob/main/docs/coding-guidelines/coding-style.md)
and [Microsoft's C# conventions](https://learn.microsoft.com/en-us/dotnet/csharp/fundamentals/coding-style/coding-conventions).
They are concrete project rules, not a claim of corporate certification.

## Tooling

The native application and tests share `OrbitDeckBuildOptions`: C++20, Unicode,
MSVC `/W4 /permissive- /utf-8` and the static runtime. First-party compiler
warnings are errors by default. `ORBITDECK_WARNINGS_AS_ERRORS=OFF` is an explicit
temporary compatibility escape hatch, not the normal validation mode.
`Directory.Build.props` centralizes the first-party .NET target, nullable
analysis and warnings-as-errors. Upstream projects keep their own settings.

Install the pinned development-only formatter in a repository-local environment:

```powershell
python -m venv out\tools\format
.\out\tools\format\Scripts\python.exe -m pip install -r build\requirements-format.txt
cmake --preset windows-2026-release
cmake --build --preset windows-2026-release --target format
cmake --build --preset windows-2026-release --target format-check
```

Python and clang-format are not shipped with the app. The format targets appear
when CMake finds clang-format, including the local path above. There is no need
to change PowerShell execution policy or activate a script.

After restoring/building the managed projects, format and check each owned
project. `dotnet format` skips referenced upstream projects by default:

```powershell
dotnet format style conversion\WeaponCycle.Conversion.csproj --no-restore --diagnostics IDE0011
dotnet format whitespace conversion\WeaponCycle.Conversion.csproj --no-restore
dotnet format whitespace conversion\WeaponCycle.Conversion.csproj --no-restore --verify-no-changes
dotnet format style tests\ConversionEngine.Smoke\ConversionEngine.Smoke.csproj --no-restore --diagnostics IDE0011
dotnet format whitespace tests\ConversionEngine.Smoke\ConversionEngine.Smoke.csproj --no-restore
dotnet format whitespace tests\ConversionEngine.Smoke\ConversionEngine.Smoke.csproj --no-restore --verify-no-changes
```

## Verification

Use the smallest relevant tests first. For a cross-boundary change, run:

```powershell
cmake --build --preset windows-2026-release --parallel 1
ctest --preset windows-2026-release --output-on-failure -j 1
dotnet build tests\ConversionEngine.Smoke\ConversionEngine.Smoke.csproj -c Release -m:1
dotnet publish conversion\WeaponCycle.Conversion.csproj -c Release -r win-x64 --self-contained true -o out\portable\conversion -m:1
dotnet tests\ConversionEngine.Smoke\bin\Release\net10.0\ConversionEngine.Smoke.dll out\portable
git diff --check
```

The native tests do not launch the overlay. Protocol, action-catalog and resource
tests are headless. The smoke runner creates and removes only its own fixtures,
validates output content, and exercises cancellation and original-file safety.
New nontrivial behavior needs a focused regression, not just a successful build.

The app previously experienced unresolved blank-screen/system hangs. Do not
auto-launch the overlay, inject input, run GPU/fullscreen stress or change
Windows/driver settings to validate a maintenance refactor. State those
unverified hardware scenarios plainly. Keep refactors behavior-preserving;
use a separate, explicitly scoped change for new functionality.
