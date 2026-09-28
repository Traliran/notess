# noteSS v1.1.0

Ultra-light desktop app for fast text-note capture to [Memos](https://www.usememos.com/) API v1.
Pure C, GTK 4, libcurl. No JSON libraries, no bloat — KISS.

A compact floating window with a single text field and a Send button.
Type, press `Ctrl+Enter`, the window closes — the note is already in Memos.
If the server is unreachable, the note is saved locally and uploaded on the next start.

## Features

- Minimal GTK 4 window (fixed size), autofocus on start.
- `Ctrl+Enter` sends, `Escape` closes.
- Non-blocking UI: HTTP POST runs in a background `pthread`, UI updates via `g_idle_add`.
- Offline cache: failed notes are stored under `$XDG_CACHE_HOME/noteSS`
  (fallback `~/.cache/noteSS`, `chmod 600`) and uploaded in the background
  on the next start; each uploaded note is deleted from the cache.
- XDG config with `chmod 600`; friendly GTK setup window on first run.
- Manual safe JSON escaping (`"`, `\`, newlines, control chars) — no extra deps.

## Dependencies

| Distro | Command |
|---|---|
| Arch Linux | `sudo pacman -S gtk4 curl base-devel pkgconf` |
| Ubuntu / Debian | `sudo apt install libgtk-4-dev libcurl4-openssl-dev build-essential pkgconf` |
| Fedora | `sudo dnf install gtk4-devel libcurl-devel gcc make pkgconf-pkg-config` |

## Build & Run

```sh
git clone https://github.com/Traliran/notess
cd notess
make
./notess
```

One-liner (same as the `Makefile` does):

```sh
gcc main.c cache.c -o notess $(pkg-config --cflags --libs gtk4 libcurl) -lpthread
```

Optional system install:

```sh
sudo make install   # installs to /usr/local/bin/notess
```

## Configuration

On first start (missing or incomplete config) noteSS opens a setup window.
Enter the values, they are saved automatically with mode `600`.

Config path (XDG standard):

- `$XDG_CONFIG_HOME/noteSS/config.conf`, fallback to `~/.config/noteSS/config.conf`

Format — plain `key=value`:

```ini
# noteSS config (chmod 600, keep it secret)
memos_url=https://memos.example.com
access_token=memos_pat_xxx
```

- `memos_url` — base URL of your Memos instance, no trailing slash needed.
- `access_token` — Memos access token (Memos → Settings → API / Access Tokens).

Requests go to `POST {memos_url}/api/v1/memos` with headers:

- `Content-Type: application/json`
- `Authorization: Bearer {access_token}`

Body: `{"content": "your text"}`.

## Usage

1. Run `./notess` (bind it to a global hotkey in your WM/DE for quick capture).
2. Type the note.
3. `Ctrl+Enter` or click **Send** — success closes the window silently.
4. On network/server error the note is saved to the local cache and an info
   dialog shows the pending count; the app closes and cached notes are
   uploaded in the background on the next start.
5. If even the local save fails, a message dialog shows the libcurl or server error text.

## Offline cache

Cache path (XDG standard):

- `$XDG_CACHE_HOME/noteSS`, fallback to `~/.cache/noteSS`

Each unsent note is one plain-text file (`note-<epoch>-<pid>-<counter>.txt`,
mode `600`). On every start noteSS uploads cached notes oldest-first in a
background thread, and again right after the next successful send (the app
would otherwise quit before the background upload finishes); each note that
gets HTTP `200`/`201` is deleted from the cache, the rest stay for the next
launch. No extra configuration needed.

## Project layout

- `main.c` — app (config, UI, JSON, curl thread).
- `cache.c` / `cache.h` — offline cache (local save, background upload, cleanup).
- `Makefile` — `make`, `make clean`, `make install`.
- `LICENSE` — GNU GPLv3 license text.
- `README.md` — this file.

## License

GNU General Public License v3.0 or later (GPL-3.0-or-later).
See `LICENSE` for the full text. Contributions welcome.
