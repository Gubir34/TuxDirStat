# TuxDirStat

A fast disk usage analyzer for Linux in the spirit of WinDirStat: a folder tree sorted by size,
a list of file types and a shaded (cushion) treemap where every file is a box. The multithreaded
scanner reads millions of files in seconds.

*Türkçe özet [aşağıda](#türkçe).*

The same core has four interfaces, each a separate program:

| Program           | Toolkit | CMake option                         |
|-------------------|---------|--------------------------------------|
| `tuxdirstat`      | Qt 6    | `TUXDIRSTAT_QT` (on by default)      |
| `tuxdirstat-gtk3` | GTK 3   | `TUXDIRSTAT_GTK3`                    |
| `tuxdirstat-gtk4` | GTK 4   | `TUXDIRSTAT_GTK4`                    |
| `tuxdirstat-tk`   | Tcl/Tk  | `TUXDIRSTAT_TK`                      |

## Features

- **Tree, extension list and treemap** that stay in sync; zoom into folders in the treemap.
- **Grouping** of the thousands of small asset files games ship with (`*.dds (1,500 files)`).
- **Duplicate files**: compares sizes, then the first 64 KB, then whole files. Hard links are not
  counted as duplicates, and you are warned before deleting every copy.
- **Cleanup suggestions**: `node_modules`, CMake and Cargo build folders, Python caches,
  `~/.cache`, trash, unfinished downloads, Steam shader cache, the pacman cache, the systemd journal,
  crash dumps and unused Docker, Podman and Flatpak data. Things that need root are shown as a
  command for you to run, never run for you.
- **Snapshots**: save a scan and open it later without rescanning, or **compare** it with today to
  see what grew. The format is ncdu's JSON export, so `ncdu -o` files open too.
- **Search** by name, size, age, type, extension, owner or path: `*.iso >1G older:1y`.
- **Color by age**: color the treemap by when files last changed, to spot forgotten data.
- **Usage by owner** and **CSV export**.
- **Arch Linux**: see which package owns a file, and list files under `/usr`, `/etc` and `/opt` that
  no package owns (leftovers, manual installs).
- **Administrator scan**: a read-only scan through `pkexec` for folders your user cannot read.
  Deleting still uses your own rights.
- Light and dark themes, **English and Turkish**, app icon, man page and shell completions.

## Install

Arch Linux (once the AUR packages are up): `tuxdirstat` or `tuxdirstat-git`. Until then you can
build the same package from this repository:

```sh
git clone https://github.com/Gubir34/TuxDirStat.git
cd TuxDirStat/packaging/aur/tuxdirstat-git && makepkg -si
```

Into `~/.local` on any distribution:

```sh
./install.sh             # Qt only
./install.sh gtk4 tk     # chosen interfaces
./install.sh all         # all four
```

You need `cmake`, `ninja`, a C++20 compiler and the development packages of the chosen toolkit
(`qt6-base`, `gtk3`, `gtk4`, `tk`; the Tk interface also uses `glib2`/GIO). Optional: `polkit` for
administrator scans, `pacman-contrib` for `paccache`, `zstd`/`xz` to open compressed snapshots.

Building by hand:

```sh
cmake -S . -B build -G Ninja -DTUXDIRSTAT_GTK4=ON -DTUXDIRSTAT_TESTS=ON
cmake --build build
ctest --test-dir build
```

## Using it

```sh
tuxdirstat                  # opens the location picker
tuxdirstat ~/Games          # scans right away
tuxdirstat home.json        # opens a saved snapshot
```

- Clicking an item in the tree or the treemap selects it in the other.
- Double-click in the treemap zooms into a folder; **Backspace** goes back.
- Selecting a row in the extension list marks those files in the treemap.
- Right-click: open, show in file manager, copy path, which package owns it, rescan, trash, delete.
- The **Tools** menu has search, duplicates, cleanup suggestions, usage by owner, files no package
  owns and snapshot comparison. Results open in a window where you can show an item in the tree,
  open its folder, copy its path (or the suggested command) and move items to the trash.

| Shortcut | Action |
|---|---|
| Ctrl+O | Choose a location |
| F5 | Rescan |
| Esc | Stop the scan |
| Ctrl+F | Find files |
| Ctrl+S | Save snapshot |
| Ctrl++ / Backspace / Ctrl+0 | Zoom in / out / back to the root |
| Ctrl+G | Group files |
| F8 | Color by age |
| F9 | Show or hide the treemap |
| Delete / Shift+Delete | Move to trash / delete permanently |
| Ctrl+C | Copy path |

### Search syntax

Words match file names (`*` and `?` are wildcards; without them, any part of the name matches).
Filters can be combined:

| Filter | Meaning |
|---|---|
| `>100M`, `<1G` | size (K, M, G, T) |
| `older:1y`, `newer:2w` | last change (h, d, w, m, y) |
| `type:f`, `type:d` | only files or only folders |
| `ext:mp4,mkv` | extensions |
| `user:name` | owner |
| `path:Steam` | text anywhere in the path |

### Grouping

With **Group files** on, a folder with at least *threshold* similar items (16 by default, 8–128
selectable) collects them into one virtual node: files with the same extension, small files (under
1 % of the folder) and small subfolders. The treemap draws a group as one block; double-click to go
inside. Deleting a group deletes the files in it.

### "One file system"

When on, the scan does not cross into other disks or mount points; btrfs subvolumes of the same
device (such as `/home` and `/var/log`) are still included. Virtual file systems like `/proc` and
`/sys` are always skipped.

## Command line

Every program also works without a window when you give it an output option. They can be combined:

```sh
tuxdirstat --report --group --top 10 ~/.local/share/Steam/steamapps/common
tuxdirstat --find '*.iso >1G older:1y' ~
tuxdirstat --duplicates --cleanup ~
tuxdirstat --export home-$(date +%F).json ~       # snapshot (ncdu JSON)
tuxdirstat --compare home-2026-10-01.json ~       # what changed since then
tuxdirstat --import home.json --csv home.csv      # snapshot to CSV
sudo tuxdirstat --unowned /                       # Arch: files no package owns
```

Other options: `--owners`, `--apparent`, `--group-min N`, `--all-fs`, `--lang en|tr`. See
`tuxdirstat --help` or `man tuxdirstat`. Bash, zsh and fish completions are installed.

## Language, theme and settings

The interface follows your locale (Turkish for `tr_*`, English otherwise). You can choose it under
*View → Language* (Qt), the menu button (GTK) or *View* (Tk); the change applies after a restart.
**System** theme follows the desktop; **Light** and **Dark** can be forced in every interface.

Size mode, one file system, grouping, threshold, theme, language, age coloring and treemap
visibility are stored in `~/.config/tuxdirstat/settings.conf`, shared by all four interfaces.

## Project layout

```
src/core/       scanner, data model, grouping, treemap renderer, tools, snapshots, i18n, CLI
src/qt/         Qt 6 interface
src/gtk/        GTK 3 / GTK 4 interface (one source)
src/tk/         Tk interface (embedded Tcl script + C++)
data/           icon, AppStream metadata, man page
completions/    bash, zsh and fish completions
packaging/aur/  PKGBUILDs for tuxdirstat and tuxdirstat-git
tests/          core unit tests and the translation check
```

Translations live in `src/core/i18n_tr.inc`. Source strings are English; `tests/check_i18n.py`
(run by `ctest`) fails when a string has no Turkish entry.

## License

TuxDirStat is released under the [GNU General Public License version 3](LICENSE) (GPL-3.0-or-later).

## Türkçe

TuxDirStat, Linux için WinDirStat tarzında bir disk kullanım analizörüdür: boyuta göre sıralı klasör
ağacı, dosya türü listesi ve gölgeli treemap. Kopya dosya bulma, temizlik önerileri, anlık görüntü
kaydetme ve karşılaştırma, arama, yaşa göre renklendirme, sahibe göre kullanım, CSV dışa aktarma,
pacman denetimleri ve yönetici olarak (salt okunur) tarama içerir. Arayüz dili sistem diline uyar
(`tr_*` için Türkçe); *Görünüm → Dil* menüsünden değiştirilebilir. Komut satırı seçenekleri için
`tuxdirstat --help` ya da `man tuxdirstat`.
