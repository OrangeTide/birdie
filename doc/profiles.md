# Profiles (the MUD list)

The user-managed MUD list. Each entry is a **profile**: a set of
named fields describing one MUD and how to connect to it. Profiles are
the import/export format for sharing lists, the runtime record for
active connections, and the anchor for per-MUD state (triggers, vars,
logs).

## Data model

A profile is a **property list** — an unordered bag of `(key, value)`
string pairs. Nothing more. No schemas, no enums, no required fields
enforced at the storage layer.

- Storage is `AList<Profile>` where each `Profile` is a `prop_list`.
- An opaque `profile_id` iterates the array (stable for the lifetime
  of the process; not persisted).
- Access from C:

      profile_id id = profile_find("aardwolf");
      const char *host = prop_get(profile_get(id), "host");
      prop_set(profile_get(id), "autoconnect", "yes");

- Access from Lua mirrors the C API:

      local p = profile.find("aardwolf")
      print(p.host, p.port)
      p.autoconnect = "yes"

Values are strings. Callers that want integers or booleans parse on
read. This matches the CSV-round-trip story (CSV is strings-only) and
means unknown columns cost nothing to preserve.

Rationale for strings-only / plist:

- The wire format (CSV) is string-typed; matching storage avoids a
  lossy transcoding step at the import/export boundary.
- Unknown columns from an imported list are preserved as-is and can be
  exported again without data loss.
- The data model is trivial to implement, test, and bind to Lua.
- Jon likely has existing plist code to drop in — check before writing.

## CSV title schema

The first row of the CSV is the column-title header. Import maps known
titles to internal fields; unknown columns are preserved in the profile
plist verbatim.

Known v1.0 titles (column order is not significant):

| title           | meaning                                                       |
|-----------------|---------------------------------------------------------------|
| `name`          | short display name, also the profile key (must be unique)     |
| `host`          | DNS name or IP                                                |
| `port`          | TCP port                                                      |
| `tls`           | `yes` / `no` — use TLS telnet                                 |
| `encoding`      | `utf-8` (default), `cp437`, `latin1`, `gbk`, etc.             |
| `description`   | longer human-readable description                             |
| `url`           | MUD's homepage                                                |
| `tags`          | space-separated list, user-defined                            |
| `autoconnect`   | `yes` / `no` — connect on startup                             |
| `autoreconnect` | `yes` / `no` / `<seconds>` (default `yes`)                    |
| `character`     | auto-fill for login (SENSITIVE — see export filter)           |
| `password`      | NEVER stored in the CSV; only the runtime keyring             |
| `on_connect`    | path to a Lua script run on connect (SENSITIVE)               |
| `notes`         | freeform user notes (SENSITIVE)                               |

Anything not on this list is a custom column: preserved, exported if
it passes the filter, available from Lua as `profile.p.<name>`.

### Example

    name,host,port,tls,encoding,description,url,tags
    Aardwolf,aardmud.org,23,no,utf-8,"Classic stock DIKU derivative",https://www.aardwolf.com,"classic dikuderiv"
    Achaea,achaea.com,23,no,utf-8,"IRE flagship — Greek pantheon",https://www.achaea.com,"ire rp pvp"
    Discworld,discworld.starturtle.net,4242,no,utf-8,"Terry Pratchett's Discworld MUD",https://discworld.starturtle.net,"lpmud classic"

## Import / export filters

Two **comma-separated column lists** configured per-install (and
overridable per-export):

- **`export_filter`** — columns allowed to leave the client. Default:
  `name,host,port,tls,encoding,description,url,tags`.
- **`import_filter`** — columns accepted from a foreign CSV. Default:
  the same safe set above, plus whatever custom columns the user has
  opted into.

Anything not matched by the active filter is **dropped silently** on
that direction. This is the primary protection against accidentally
sharing `notes`, `character`, `on_connect`, or any future sensitive
column added later. A column added in a future version starts in
**neither** filter and therefore round-trips through neither direction
until the user explicitly opts in.

CLI / UI affordance: an `Export…` dialog shows the filter as checkboxes
per known column, with the persisted default pre-selected.

### Redaction on export

For fields that are in the filter but contain user-specific data by
convention (`tags`, `description`), export is verbatim. The filter is
the only privacy mechanism at export time; there is no automatic
scrubbing.

## Import sources

- Local CSV file.
- Pasted text (clipboard).
- GitHub Gist URL (fetch the raw `.csv` blob).
- paste.net / pastebin-style "raw" URLs.

All import paths funnel through the same CSV parser and the same
`import_filter`. The source URL, if any, is recorded in the imported
rows' `source` column (which itself is a custom column preserved under
the default filter if present).

## Persistence

Profiles live in a single CSV file under the per-user config directory
(`$XDG_CONFIG_HOME/birdie/profiles.csv` on Linux,
`%APPDATA%\birdie\profiles.csv` on Windows). The on-disk format is the
same CSV format used for sharing — "save" is "export with no filter."

Per-profile scripts, trigger classes, and variable dumps live beside
the CSV in `profiles/<name>/`:

    profiles/
        Aardwolf/
            triggers.lua
            vars.json
            classes.toml
        Achaea/
            ...

These are not part of the CSV and are not shared by the CSV export
path. They have their own (later) export-as-zip story.

`vars.json` is the persistent script `var` table (doc/triggers.md), a JSON
dump written via the engine's `json.encode`/`json.decode` codecs (the design
originally said a Lua dump; JSON reuses the GMCP codecs already in the
bootstrap). `bd_session` loads it when its data dir is set and saves it on
disconnect and at free.

## Password storage

Passwords are **never** in the CSV and never in `profiles/`. Runtime
storage uses the OS keyring (libsecret on Linux, Credential Manager on
Windows). On platforms without a keyring, the password field is
session-only and prompted at connect time.

## Open questions

- Whether the profile CSV supports multi-line fields (RFC 4180 allows
  `"..."` with embedded newlines). Lean yes — `description` and
  `notes` want it — but test the cut-paste path through gists to
  confirm it survives.
- Unique-key collisions on import (two rows named `Aardwolf`). Propose:
  prompt with three options — skip, rename-new (`Aardwolf (2)`),
  overwrite.
- Whether the `tls` column should default to `yes` for ports commonly
  associated with TLS (992, 2223). Leaning no: explicit beats clever.
- Export filter UX: one global default vs. per-list? One default is
  simpler; per-list can come later if demand appears.
