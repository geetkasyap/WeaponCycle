# Third-party notices

The native launcher uses the Windows SDK and the vendored JSON parser below.
The optional, app-local conversion helper redistributes Filee.Core
and Filee.Engines, their managed/native dependencies, and isolated engine
programs. It starts only for format discovery, explicit conversion or an
explicit engine download; it is not the Filee desktop application.

## File conversion

- Source: https://github.com/KnifeLemon/Filee
- Source revision used for this integration: `b921fd622350c496ad76bde6deaf0deee22a143c`
- Filee copyright: Copyright (c) 2026 Myeonggu Kim (KnifeLemon) and Filee contributors
- Filee license: MIT, preserved in `conversion/LICENSE-Filee.txt` in the portable build.
- Complete upstream dependency/engine notices:
  `conversion/THIRD-PARTY-NOTICES-Filee.md` in the portable build.
- Package licenses and source/license declarations are collected under
  `conversion/licenses`. The app-local .NET runtime carries its own notices.
- The default engine set includes rhwp (MIT), 7-Zip (LGPL-2.1-or-later with
  the unRAR restriction), and FFmpeg (GPL-3.0-or-later build). Their original
  notices remain in `conversion/engines`; they run as separate processes.
- Other optional engines retain their own licenses, as recorded in the
  upstream notices. No optional engine is silently downloaded.

The source for Filee and the engine/dependency projects is linked in those
notices. The local Filee engine build contains a small TIFF sample-depth
correction in `Magick/ImageEncoder.cs` that preserves palette PNG colors/alpha,
with a regression in `ImageFormatTests.cs`.
The copyright/license notices are preserved; the base revision above identifies
upstream source rather than claiming the local engine build is unmodified.
Keep the complete portable folder, its licenses and notices together when
redistributing; do not distribute the launcher alone as the full converter.

The OrbitDeck icon is an original project asset; its vector source is included
in `assets/orbitdeck.svg`.

## JSON for Modern C++ (nlohmann/json)

- Version: 3.11.3
- File: `third_party/nlohmann/json.hpp` (single-header amalgamation)
- Homepage: https://github.com/nlohmann/json
- License: MIT
- Usage: compile-time only. Parses the user configuration file at
  `%LOCALAPPDATA%\OrbitDeck\config.json`. It is a header compiled into the
  executable and adds no runtime dependency, service, or network access.

```text
MIT License

Copyright (c) 2013-2023 Niels Lohmann <https://nlohmann.me>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
