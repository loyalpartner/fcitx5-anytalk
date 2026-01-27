# AnyTalk Progress Log

## Summary
- Built a minimal Fcitx5 addon named `anytalk` and confirmed it loads and can be selected as current IM after fixing addon config naming.
- Added a minimal Rust daemon in `anytalk-daemon/` with audio capture and ASR WebSocket pipeline (custom implementation).
- Current issue: F9 hotkey in addon does not trigger start/stop reliably; daemon logs are not showing activity from the addon.

## Fcitx5 addon
- Source: `fcitx5-anytalk/src/addon.cpp`, `fcitx5-anytalk/src/ipc_client.cpp`
- Build: `fcitx5-anytalk/build/lib/fcitx5/anytalk.so`
- Installed addon file: `/usr/share/fcitx5/addon/anytalk.conf` (note: this filename is required; earlier `anytalk-addon.conf` was wrong)
- Input method file: `/usr/share/fcitx5/inputmethod/anytalk.conf`
- Profile updated to include `anytalk` in `~/.config/fcitx5/profile`

### Key behavior
- Hotkey: `F9` in `AnyTalkEngine::keyEvent` toggles start/stop by sending JSON to daemon socket.
- IPC socket path: `$XDG_RUNTIME_DIR/anytalk.sock` (fallback `/run/user/$UID/anytalk.sock` or `/tmp/anytalk.sock`)

## Rust daemon
- Path: `anytalk-daemon/`
- Build: `cargo build`
- Run: `cargo run`
- Env vars:
  - `ANYTALK_APP_ID` (required)
  - `ANYTALK_ACCESS_TOKEN` (required)
  - `ANYTALK_RESOURCE_ID` (optional, default `volc.seedasr.sauc.duration`)
  - `ANYTALK_MODE` (optional, default `bidi_async`)

### What it does now
- Accepts IPC JSON: `start/stop/cancel` over Unix socket.
- On `start`: opens audio capture (cpal), builds 16k mono PCM chunks, sends to ASR WebSocket (custom protocol).
- Parses ASR responses and sends `partial`/`final` JSON back to addon.

## Known issues
1) F9 in anytalk does not trigger daemon (no logs).
2) Need to verify addon receives key events (InputContext focus) and that IPC connection is established.

## Next steps (suggested)
1) Add debug logging in addon `keyEvent` and IPC connect success/failure.
2) Confirm input context focus is valid when pressing F9.
3) Verify daemon socket exists and is connectable.
4) If needed, change hotkey handling to use `filterAndAccept()` and check event modifiers.

