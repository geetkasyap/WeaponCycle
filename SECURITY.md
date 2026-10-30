# Security policy

OrbitDeck is designed for standard-user execution and never requests
administrator privileges.

The launcher contains no background networking, telemetry, crash upload, desktop
duplication, browser-profile access, process injection, input injection,
service, driver, or arbitrary keystroke recording. Diagnostic logging is
disabled by default and, when implemented, will remain local and omit typed
keys, window contents, full titles, sensitive URLs, and user documents.

Mirror connects to the selected camera only while its local preview is visible;
it does not record or persist frames. Mic Check opens the default microphone
only while its level meter is visible; it computes a peak level from short PCM
buffers and does not record or persist audio. Both release their device handles
on switch, close, configuration removal, and shutdown, subject to bounded driver
cleanup retries.

Preferences preserves unknown configuration fields while validating and writing
supported changes under a per-user lock. Scratchpad content and recovery copies
stay in `%LOCALAPPDATA%\OrbitDeck` (with HKCU emergency recovery only if normal
storage fails); no configuration or note data is transmitted.

File conversion reuses an app-local, headless Filee engine process. The native
parent owns its process tree in a kill-on-close Windows job, communicates through
private inherited pipes, and keeps the UI thread out of decoding/engine work.
Engine downloads require an explicit user action and use pinned SHA-256 hashes;
files being converted are never uploaded. The helper does not read Filee's user
settings, run its UI, or automatically discover/use system-installed programs.

Explorer selection is read only on explicit conversion activation. The optional
right-click action writes only Weapon Cycle's HKCU Shell verb, after confirmation;
default file associations are unchanged. File activation is bounded and validated
before it reaches the same native converter.

Outputs are staged in a job-owned folder and published under a new name only
after conversion completes. Originals are never replaced. Cooperative
cancellation cleans up; a forced process shutdown may leave a clearly named
partial folder. Conversion libraries and document parsers are not a security
sandbox: keep the packaged dependencies current and do not bypass endpoint or
Windows security warnings to run untrusted files.

Activation uses `RegisterHotKey` and asynchronous Windows Raw Input notifications,
not system-wide keyboard/mouse hooks. Normal activation registers keyboard Raw
Input only during a visible held gesture. The optional Left-Shift-only mode
registers it while explicitly enabled, with typing cancellation and a hold
threshold. It observes only activation/cancellation state, never decodes or logs
typed text, suppresses no input, and unregisters on reload, pause and shutdown.
An unresponsive rendering thread cannot block the Windows input-hook chain.

Higher-integrity application windows may reject ordinary focus requests.
OrbitDeck reports that boundary and does not bypass UIPI or endpoint controls.

To report a vulnerability, open a private security advisory in the repository
hosting OrbitDeck. Do not include company-sensitive data, authentication
tokens, or private configuration files in a report.
