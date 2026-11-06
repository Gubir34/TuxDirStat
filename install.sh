#!/bin/sh
# Builds TuxDirStat and installs it for the current user (~/.local: programs, menu
# entries, icon, man page and shell completions).
#
#   ./install.sh                 # Qt interface only (tuxdirstat)
#   ./install.sh gtk4 tk         # chosen interfaces
#   ./install.sh all             # all of them: qt gtk3 gtk4 tk
#
# Needs cmake, ninja, g++ and the development packages of the chosen toolkits
# (qt6-base, gtk3, gtk4, tk).
set -e
here="$(cd "$(dirname "$0")" && pwd)"

qt=OFF gtk3=OFF gtk4=OFF tk=OFF
[ $# -eq 0 ] && qt=ON
for ui in "$@"; do
    case "$ui" in
        qt) qt=ON ;;
        gtk3) gtk3=ON ;;
        gtk4) gtk4=ON ;;
        tk) tk=ON ;;
        all) qt=ON gtk3=ON gtk4=ON tk=ON ;;
        *) echo "Unknown interface: $ui (qt, gtk3, gtk4, tk, all)" >&2; exit 1 ;;
    esac
done

cmake -S "$here" -B "$here/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DTUXDIRSTAT_QT=$qt -DTUXDIRSTAT_GTK3=$gtk3 -DTUXDIRSTAT_GTK4=$gtk4 -DTUXDIRSTAT_TK=$tk
cmake --build "$here/build"
cmake --install "$here/build" --prefix "$HOME/.local"
echo "Installed (~/.local/bin):"
[ $qt = ON ] && echo "  tuxdirstat"
[ $gtk3 = ON ] && echo "  tuxdirstat-gtk3"
[ $gtk4 = ON ] && echo "  tuxdirstat-gtk4"
[ $tk = ON ] && echo "  tuxdirstat-tk"
exit 0
