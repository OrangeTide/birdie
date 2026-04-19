# Wishlist

Features we want eventually but which are **not** targeted for v1.0. Kept
here so we don't forget and so design decisions today don't accidentally
foreclose them.

## Screen reader / accessibility mode

Inspiration: **VIP-Mud** (https://www.gmagames.com/vipmud.shtml), the
reference MUD client for blind and visually impaired users. Its feature
set is worth studying in full; the headline ideas:

- Complete keyboard-only operation; no mouse assumed.
- Integration with screen readers (JAWS, NVDA on Windows; Orca on Linux).
- Separate "review" buffer navigable line-by-line, word-by-word, char-by-char
  without disturbing live output.
- Speech-friendly filtering: mute map/ASCII-art blocks, collapse repeated
  prompts, announce only what the user asked for.
- Named sound cues tied to triggers (a trigger says "combat start" instead
  of flashing the screen).
- Fine-grained control over what gets spoken vs what gets logged vs what
  gets shown.

### Implication for Birdie's architecture

A screen-reader mode may want **no OpenGL context at all** — the terminal
widget becomes a logical line buffer exposed over the platform
accessibility API (AT-SPI on Linux, UIA on Windows), with stdout/stdin as
a fallback for headless/SSH use.

This is an argument for keeping the core (network → parser → line buffer →
trigger engine → log sink) fully independent of `ludica` and the GUI. The
GUI becomes one front-end among several:

    core ──┬── ludica GUI (default)
           ├── stdout/stdin / curses front-end (accessible, headless)
           └── accessibility-API front-end (screen reader)

If v1.0 is built with this separation in mind — even without the alternate
front-ends existing yet — adding them later is tractable. If v1.0 entangles
core logic with the rendering pipeline, accessibility becomes a rewrite.

Not landing in v1.0. Keep the seam clean.

## Other ideas to revisit later

- **Auto-mapping** — deferred as agreed. The scripting API should expose
  enough room/exit/GMCP-room events that a user script could prototype
  auto-mapping before we build anything native.
- **SSH transport** — libssh2 is a large dependency for a small set of
  MUDs; defer until requested.
- **SOCKS5 proxying** — deferred unless asked. Some users run MUDs
  through Tor or corporate proxies; straightforward to add on top of
  `libiox` when needed.
- **MCCP3** (client → server compression) — MTH supports it; Birdie
  does not enable it in v1.0 because server-side adoption is rare and
  the compile-time flag exists if a fork wants it.
- **Kitty graphics protocol** — superior to SIXEL technically but zero MUD
  adoption. Revisit if a MUD ever ships support.
- **NDJSON log compression** (`.ndjson.zst`) with transparent read in the
  replay path.
- **macOS port** — stretch goal per `concept.md`. `ludica` + ANGLE makes
  it plausible.
- **WebAssembly build** — `ludica` can already target wasm, and our GLES2
  subset maps to WebGL 1.0. The blocker is telnet from the browser (no
  raw TCP); would require a WebSocket-to-telnet bridge service, which is
  out of scope for the client itself.
