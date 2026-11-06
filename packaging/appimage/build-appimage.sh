#!/usr/bin/env bash
# Builds TuxDirStat-<version>-<arch>.AppImage (Qt interface) with linuxdeploy.
# Run on an old enough distribution (the release workflow uses Ubuntu 24.04):
#   packaging/appimage/build-appimage.sh
# Needs cmake, ninja, a C++20 compiler, Qt 6 and zlib development files, wget.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
work=${WORK:-$root/build-appimage}
arch=$(uname -m)
version=$(sed -n 's/^project(TuxDirStat VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")

mkdir -p "$work/tools"
cmake -S "$root" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
      -DTUXDIRSTAT_QT=ON -DTUXDIRSTAT_TUI=OFF -DTUXDIRSTAT_FILEMANAGERS=OFF
cmake --build "$work/build"
rm -rf "$work/AppDir"
DESTDIR="$work/AppDir" cmake --install "$work/build"

cd "$work/tools"
for tool in linuxdeploy linuxdeploy-plugin-qt; do
    file=$tool-$arch.AppImage
    [ -x "$file" ] || {
        repo=$tool; [ "$tool" = linuxdeploy ] && repo=linuxdeploy
        wget -q "https://github.com/linuxdeploy/$repo/releases/download/continuous/$file"
        chmod +x "$file"
    }
done
cd "$work"
export QMAKE=${QMAKE:-$(command -v qmake6 || command -v qmake)}
export APPIMAGE_EXTRACT_AND_RUN=1 OUTPUT="TuxDirStat-$version-$arch.AppImage"
"tools/linuxdeploy-$arch.AppImage" --appdir AppDir --plugin qt --output appimage \
    --desktop-file AppDir/usr/share/applications/tuxdirstat.desktop \
    --icon-file AppDir/usr/share/icons/hicolor/256x256/apps/tuxdirstat.png
echo "Built $work/$OUTPUT"
