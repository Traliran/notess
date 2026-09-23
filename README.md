# noteSS v1.0

Ultra-light desktop app for fast text-note capture to [Memos](https://www.usememos.com/) API v1.
Pure C, GTK 4, libcurl. No JSON libraries, no bloat — KISS.

A compact floating window with a single text field and a Send button.
Type, press `Ctrl+Enter`, the window closes — the note is already in Memos.

## Features

- Minimal GTK 4 window (fixed size), autofocus on start.
- `Ctrl+Enter` sends, `Escape` closes.
- Non-blocking UI: HTTP POST runs in a background `pthread`, UI updates via `g_idle_add`.
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
gcc main.c -o notess $(pkg-config --cflags --libs gtk4 libcurl) -lpthread
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
4. On network/server error a message dialog shows the libcurl or server error text.

## Project layout

- `main.c` — whole app (config, UI, JSON, curl thread).
- `Makefile` — `make`, `make clean`, `make install`.
- `LICENSE` — GNU GPLv3 license text.
- `README.md` — this file.

## License

GNU General Public License v3.0 or later (GPL-3.0-or-later).
See `LICENSE` for the full text. Contributions welcome.
