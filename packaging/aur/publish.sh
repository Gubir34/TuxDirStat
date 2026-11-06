#!/usr/bin/env bash
# Publishes (or updates) the tuxdirstat and tuxdirstat-git packages on the AUR.
#
# Needs: an AUR account with ~/.ssh/aur.pub added under "My Account", and the
# "Host aur.archlinux.org" entry in ~/.ssh/config. Run from anywhere:
#   packaging/aur/publish.sh            # both packages
#   packaging/aur/publish.sh tuxdirstat # only one
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=https://github.com/Gubir34/TuxDirStat.git
pkgs=("$@")
[ ${#pkgs[@]} -eq 0 ] && pkgs=(tuxdirstat tuxdirstat-git)

die() { echo "error: $*" >&2; exit 1; }

ssh -o BatchMode=yes aur@aur.archlinux.org help 2>&1 | grep -qi 'commands' \
    || die "the AUR does not accept your SSH key yet (add ~/.ssh/aur.pub under My Account)"

for pkg in "${pkgs[@]}"; do
    src="$here/$pkg"
    [ -f "$src/PKGBUILD" ] || die "no PKGBUILD in $src"
    grep -q '<e-posta@adresin>' "$src/PKGBUILD" \
        && die "fill in the Maintainer line in $src/PKGBUILD first"

    if [ "$pkg" = tuxdirstat ]; then
        ver=$(sed -n 's/^pkgver=//p' "$src/PKGBUILD")
        git ls-remote --exit-code --tags "$repo" "v$ver" >/dev/null \
            || die "tag v$ver is not on GitHub yet; push it before publishing $pkg"
    fi

    work=$(mktemp -d)
    trap 'rm -rf "$work"' EXIT
    echo "==> $pkg"
    git -c init.defaultBranch=master clone -q "ssh://aur@aur.archlinux.org/$pkg.git" "$work/$pkg" 2>/dev/null
    cp "$src/PKGBUILD" "$work/$pkg/PKGBUILD"
    (cd "$work/$pkg" && makepkg --printsrcinfo > .SRCINFO)
    cp "$work/$pkg/.SRCINFO" "$src/.SRCINFO"

    cd "$work/$pkg"
    git add PKGBUILD .SRCINFO
    if git diff --cached --quiet; then
        echo "    already up to date"
    else
        ver=$(sed -n 's/^\tpkgver = //p' .SRCINFO)
        rel=$(sed -n 's/^\tpkgrel = //p' .SRCINFO)
        git commit -q -m "Update to $ver-$rel"
        git push -q origin HEAD:master
        echo "    published $ver-$rel: https://aur.archlinux.org/packages/$pkg"
    fi
    cd "$here"
    rm -rf "$work"
    trap - EXIT
done
