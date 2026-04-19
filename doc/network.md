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
under `share/birdie/cacert.pem`, overridable per-profile. Document the
update path in `doc/vendoring.md` conventions — it's effectively a
vendored asset.

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
| `MCCP3`      | client → server compression                | off by default; opt-in per profile (rare) |
| `MSSP`       | MUD status report                          | surfaced in profile dialog    |
| `MSDP`       | structured out-of-band messaging           | routed to `bd_vm` as tables   |
| `GMCP`       | structured out-of-band messaging (JSON)    | routed to `bd_vm` as tables; NDJSON-logged |

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
this); the UI thread is never blocked on resolution. Happy Eyeballs is
v1.0 because dual-stack MUD hosts are common and IPv4-first blocking
resolution produces multi-second connect stalls.

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

- Do we ship MCCP3 on? It's rare enough that the added complexity may
  not be worth it. Leaning off-by-default, opt-in per profile.
- IPv6 preference order when both records resolve. Happy Eyeballs
  (RFC 8305) parameters: 250 ms delay, prefer IPv6. Document and move
  on.
- Windows: where does the CA bundle live? `%LOCALAPPDATA%\birdie\` is
  the natural answer; confirm at build.md time.
- Proxying (SOCKS5). Deferred unless asked — goes in `wishlist.md`.
