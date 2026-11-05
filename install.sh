#!/bin/sh
# TuxDirStat'ı derleyip kullanıcı için kurar (~/.local/bin ve uygulama menüsü).
#
#   ./install.sh                 # yalnızca Qt arayüzü (tuxdirstat)
#   ./install.sh gtk4 tk         # seçilen arayüzler
#   ./install.sh all             # hepsi: qt gtk3 gtk4 tk
#
# Gerekenler: cmake, ninja, g++ ve seçilen arayüzün geliştirme paketleri
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
        *) echo "Bilinmeyen arayüz: $ui (qt, gtk3, gtk4, tk, all)" >&2; exit 1 ;;
    esac
done

cmake -S "$here" -B "$here/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DTUXDIRSTAT_QT=$qt -DTUXDIRSTAT_GTK3=$gtk3 -DTUXDIRSTAT_GTK4=$gtk4 -DTUXDIRSTAT_TK=$tk
cmake --build "$here/build"
cmake --install "$here/build" --prefix "$HOME/.local"
echo "Kuruldu (~/.local/bin):"
[ $qt = ON ] && echo "  tuxdirstat"
[ $gtk3 = ON ] && echo "  tuxdirstat-gtk3"
[ $gtk4 = ON ] && echo "  tuxdirstat-gtk4"
[ $tk = ON ] && echo "  tuxdirstat-tk"
exit 0
