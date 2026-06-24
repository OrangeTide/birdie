# Networking

Birdie's network layer: sockets, TLS, telnet, and the MUD telnet
extensions. Everything below the trigger engine and above the OS.

## Reusable code

- **`~/DEVEL/lumi/src/libiox`** — event loop (`iox_loop`), fd sources
  (`iox_fd`), timers (`iox_timer`), signal handling (`iox_signal`).
  Portable across Linux/Windows. Birdie vendors this under
  `src/thirdparty/libiox/` per `doc/vendoring.md`.
- **`~/boris/src/thirdparty/mth`** — Mud Telopt Handler (from the
  TinTin++ lineage). Covers CHARSET, ECHO, EOR, GMCP, MCCP2, MCCP3,
  MSDP, MSSP, MTTS, NAWS, NEW_ENVIRON, TTYPE. This is the complete v1.0
  extension set in one library. Vendor under `src/thirdparty/mth/`.

**MTH is not clean code.** It depends on `mud.h` globals, uses
`descriptor_data` structs shaped by the server it was cut from, pulls
in `zlib` directly, and uses `STRALLOC`/`RESTRING`/`STRFREE` macros with
fixed-size buffers. Vendor it, then put a Birdie-owned wrapper in front
of it (`src/net/telopt.[ch]`) that:

- Presents a clean C API to the rest of birdie (no macro leaks).
- Owns its own buffers (no fixed `MAX_STRING_LENGTH` ceiling leaking
  into birdie).
- Translates MTH's output events into `bd_vm` calls and NDJSON log
  records.
- Isolates zlib so MCCP can be compile-time-disabled on targets without
  it.

Do not refactor MTH internally during v1.0 — it works, and rewriting it
is post-v1.0 work. The wrapper is the seam.

## Transport

### TCP / telnet (v1.0)

Plain TCP plus telnet option negotiation. `libiox` handles the fd; the
wrapper above MTH handles IAC parsing.

### TLS telnet (v1.0)

**TLS library: mbedTLS.** Vendored under `src/thirdparty/mbedtls/`.
Reasons:

- Single-source-tree, BSD-style licensed, fits our vendor model.
- Smaller and easier to cross-compile for Windows and Pi than OpenSSL.
- Known-good on aarch64 and armhf.
- No system-crypto dependency on Linux.

A TLS connection is a normal telnet stream run over an mbedTLS BIO.
The telnet/IAC layer does not know whether TLS is underneath. Profile
schema (`doc/profiles.md`) carries a `tls` column that picks plain vs.
TLS at connect time.

Trust store: bundle Mozilla's CA list (via `certdata.txt` conversion)
at install prefix `share/birdie/cacert.pem`, overridable per-profile.
Per-user overrides live at:

- Linux: `$XDG_DATA_HOME/birdie/cacert.pem` (default
  `~/.local/share/birdie/cacert.pem`).
- Windows: `%LOCALAPPDATA%\birdie\cacert.pem`.

Document the update path in `doc/vendoring.md` conventions — it's
effectively a vendored asset.

## Filesystem layout

Birdie follows the XDG Base Directory Specification on Linux and the
standard `%APPDATA%` / `%LOCALAPPDATA%` split on Windows.

| role            | Linux                                    | Windows                          |
|-----------------|------------------------------------------|----------------------------------|
| config (roaming)| `$XDG_CONFIG_HOME/birdie/`               | `%APPDATA%\birdie\`              |
| data / logs     | `$XDG_DATA_HOME/birdie/`                 | `%LOCALAPPDATA%\birdie\`         |
| cache           | `$XDG_CACHE_HOME/birdie/`                | `%LOCALAPPDATA%\birdie\cache\`   |
| bundled assets  | install-prefix `share/birdie/`           | install dir `share\birdie\`      |

What lives where:

- `config/` — `profiles.csv`, `settings.toml`, filter defaults.
- `data/` — `logs/<year>/<mud>/<character>/...`, per-profile
  `profiles/<name>/` scripts, per-user CA override.
- `cache/` — transient state (DNS cache, downloaded gist imports
  awaiting review).
- `share/birdie/` — bundled CA list, default themes, shipped Lua
  stdlib.

`doc/profiles.md` and `doc/logging.md` reference these roots.

### SSH

Out of scope for v1.0. See `doc/wishlist.md`.

## Telnet options in v1.0

All covered by MTH; listing here so the set is explicit.

| option       | purpose                                    | notes                         |
|--------------|--------------------------------------------|-------------------------------|
| `TTYPE`      | terminal type identification               | advertise `birdie/<version>`  |
| `MTTS`       | extended terminal-type bit flags           | 256-color, UTF-8, MNES if set |
| `NAWS`       | window size                                | resend on terminal resize     |
| `NEW_ENVIRON`| environment variables (client → server)    | minimal: `CHARSET`, `CLIENT`  |
| `CHARSET`    | negotiate encoding                         | prefer UTF-8; fall back per profile |
| `ECHO`       | server-controlled local echo               | used for password prompts     |
| `EOR` / `GA` | prompt marking                             | drives prompt-type triggers   |
| `MCCP2`      | server → client compression (zlib)         | on by default when offered    |
| `MSSP`       | MUD status report                          | surfaced in profile dialog    |
| `MSDP`       | structured out-of-band messaging           | routed to `bd_vm` as tables   |
| `GMCP`       | structured out-of-band messaging (JSON)    | routed to `bd_vm` as tables; NDJSON-logged |
| `MXP`        | in-band tag protocol (option 91)           | parsed by `bd_mxp`; tags routed to `on.mxp`/`mxp` triggers, NDJSON-logged |

MSDP and GMCP packages are **routed by name**, not interpreted by
birdie. The scripting layer's `on.gmcp["Char.Vitals"]` hook table is
what consumes them. Birdie only needs to know two structural things:
package-name routing and JSON-vs-MSDP decoding.

### GMCP package allowlist

None. Birdie passes every received GMCP package to scripts. Users may
filter in Lua. Logging has a separate filter knob (`doc/logging.md`).

## Connection lifecycle

States: `idle → resolving → connecting → tls-handshake? → connected →
negotiating → ready → (closing → closed)`. Each transition emits an
NDJSON `kind:"connect"` / `"disconnect"` record and a `bd_vm` event
(`on.connect`, `on.disconnect`).

**DNS:** resolve via `getaddrinfo` on a worker thread (libiox wraps
this); the UI thread is never blocked on resolution. **Happy Eyeballs
(RFC 8305)** is in v1.0 with IPv6 preferred and a 250 ms delay before
falling back to IPv4. Dual-stack MUD hosts are common and IPv4-first
blocking resolution produces multi-second connect stalls.

**Negotiation window:** give the server 2 s to finish `IAC DO/WILL`
chatter before declaring the session `ready`. The window resets on
each received IAC byte so slow servers don't get cut off.

**Reconnect:** on unexpected disconnect, auto-reconnect with exponential
backoff capped at 60 s, unless the profile opts out (`autoreconnect=no`)
or the disconnect was user-initiated. On reconnect, the last N lines of
scrollback are preserved (NDJSON replay — `doc/logging.md`). Triggers
do not fire on the replayed lines; they're marked with a `replay`
attribute.

**Keepalive:** send `IAC NOP` every 300 s if the connection is
otherwise idle. TCP keepalive is enabled as a second line of defense.

## Threading

One **network thread** runs the libiox loop, drives TLS, runs MTH,
and pushes decoded events onto a lock-free single-consumer ring. The
UI thread consumes the ring, appends to terminal scrollback, drives
the trigger engine, and pumps log sinks. This matches the decisions in
`doc/gui.md` and `doc/triggers.md`.

Why one network thread, not one per connection: birdie supports a handful
of concurrent MUDs, not hundreds. Multiplexing them on one `iox_loop` is
simpler, keeps TLS contexts from fighting over scheduler time, and
matches the typical user workload (1–3 active sessions).

### Implementation status

The network thread and the ring handoff are built, in `src/birdie/bd_net.c`
and `src/birdie/bd_ring.c`:

- The net thread runs the libiox poll loop and owns the socket: resolution
  (`getaddrinfo`), connect, `recv`/`send`, and telnet IAC filtering all run
  there, off the UI thread.
- `bd_ring` is the lock-free single-producer/single-consumer ring. Two
  instances carry framed messages: `tx` (UI → net: connect/send/close) and
  `rx` (net → UI: decoded data / state transitions). The UI wakes the net
  thread through a self-pipe the loop watches. `bd_net_poll()` drains `rx`
  on the UI thread and fires the data/state callbacks there.
- Backpressure: when `rx` fills, the net thread caps each `recv` to the
  ring's free space and drops read interest (`iox_fd_mod`), resuming once
  the UI has drained. No bytes are dropped.

Built today: plain TCP and TLS, the core telnet options (bd_telopt:
TTYPE/MTTS, NAWS, NEW_ENVIRON, CHARSET, SGA, ECHO, EOR/GA), GMCP and MSDP
(out-of-band packages routed by name; MSDP converted to JSON), MCCP2
(server->client zlib decompression via vendored miniz), auto-reconnect with
exponential backoff, and password masking driven by ECHO. TLS runs the telnet
stream over an mbedTLS BIO with
the handshake driven non-blocking by the same poll loop; the telnet layer is
unaware TLS is underneath. `bd_net_connect()` takes a `tls` flag; the trust
store is `$BIRDIE_CACERT` then the host's system CA bundle, with
`BIRDIE_TLS_INSECURE` to skip verification for testing. GMCP/MSDP are routed
to a `bd_net` package callback (the trigger/scripting layer is the eventual
consumer); on GMCP enable the client sends `Core.Hello` + `Core.Supports.Set`.
Done natively rather than via MTH, which is kept only as a standalone test
oracle. MCCP2 inflates the server stream below the telnet layer: telopt sees
the decompressed bytes and is unaware compression is underneath, the same way
the telnet layer is unaware of TLS. Auto-reconnect redials the stored target
on an unexpected drop or failed connect with exponential backoff (1s doubling
to 60s, via a libiox timer); a successful connect resets the backoff, and a
user close or connect cancels it (`bd_net_set_autoreconnect()` opts out, the
eventual home of the profile's `autoreconnect` setting). Not yet: scrollback
replay on reconnect (needs logging), the bundled Mozilla `cacert.pem` (a
packaging step), MSSP, MCCP3 (client->server compression), Happy Eyeballs,
keepalive, and encoding transcode. The single network thread multiplexing
several connections is the target; today `bd_net` handles one connection at a
time.

## Encoding

Each profile declares an encoding (`doc/profiles.md`). Default UTF-8.
Legacy MUDs commonly want `cp437`, `latin1`, or `gbk`. Incoming bytes
are transcoded to UTF-8 before they hit libvt; outgoing commands are
transcoded from UTF-8 to the profile encoding before being sent.

Transcoding uses a small built-in table for cp437 / latin1 / the other
ANSI codepages. For `gbk` and similarly large tables we vendor a
minimal converter rather than pulling in `iconv` as a system dep.

## NDJSON integration

`kind:"telnet"` records are **off by default** and opt-in per log sink
— they carry the raw IAC negotiation for debugging. `kind:"gmcp"`,
`"msdp"`, `"mssp"` records are on by default. The telnet wrapper emits
these records directly; the telopt layer is the one thing that sees
enough context to describe them.

## Open questions

None currently — MCCP3, Happy Eyeballs defaults, CA bundle location,
and SOCKS5 disposition are all decided above (or deferred to
`doc/wishlist.md`).
