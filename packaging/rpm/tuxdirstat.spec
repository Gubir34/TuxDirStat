# Fedora / openSUSE package (for example on Fedora COPR):
#   spectool -g -R packaging/rpm/tuxdirstat.spec && rpmbuild -ba packaging/rpm/tuxdirstat.spec
Name:           tuxdirstat
Version:        1.5
Release:        1%{?dist}
Summary:        Fast disk usage analyzer with treemap, duplicates and cleanup suggestions
License:        GPL-3.0-or-later
URL:            https://github.com/Gubir34/TuxDirStat
Source0:        %{url}/archive/v%{version}/TuxDirStat-%{version}.tar.gz

BuildRequires:  cmake
BuildRequires:  ninja-build
BuildRequires:  gcc-c++
BuildRequires:  python3
BuildRequires:  pkgconfig(zlib)
BuildRequires:  pkgconfig(Qt6Widgets)
BuildRequires:  pkgconfig(gtk+-3.0)
BuildRequires:  pkgconfig(gtk4)
BuildRequires:  pkgconfig(tk)
BuildRequires:  pkgconfig(gio-2.0)
BuildRequires:  pkgconfig(ncursesw)
BuildRequires:  desktop-file-utils
BuildRequires:  libappstream-glib
Requires:       hicolor-icon-theme
Recommends:     polkit
Suggests:       compsize

%description
TuxDirStat shows what takes up space on your disks: a folder tree sorted by size,
a list of file types and a shaded treemap (or sunburst) where every file is a box.
It finds duplicate files, suggests things to clean up, saves and compares snapshots,
records size history, scans other computers over SSH and exports HTML reports.
Qt, GTK 3, GTK 4, Tk and terminal (ncurses) interfaces are included.

%prep
%autosetup -n TuxDirStat-%{version}

%build
%cmake -G Ninja -DTUXDIRSTAT_QT=ON -DTUXDIRSTAT_GTK3=ON -DTUXDIRSTAT_GTK4=ON -DTUXDIRSTAT_TK=ON \
       -DTUXDIRSTAT_TUI=ON -DTUXDIRSTAT_TESTS=ON
%cmake_build

%install
%cmake_install

%check
%ctest
desktop-file-validate %{buildroot}%{_datadir}/applications/tuxdirstat*.desktop
appstream-util validate-relax --nonet %{buildroot}%{_metainfodir}/io.github.gubir34.TuxDirStat.metainfo.xml

%files
%license LICENSE
%doc README.md
%{_bindir}/tuxdirstat
%{_bindir}/tuxdirstat-gtk3
%{_bindir}/tuxdirstat-gtk4
%{_bindir}/tuxdirstat-tk
%{_bindir}/tuxdirstat-tui
%{_datadir}/applications/tuxdirstat*.desktop
%{_datadir}/icons/hicolor/*/apps/tuxdirstat.*
%{_metainfodir}/io.github.gubir34.TuxDirStat.metainfo.xml
%{_mandir}/man1/tuxdirstat*.1*
%{_datadir}/bash-completion/completions/tuxdirstat*
%{_datadir}/zsh/site-functions/_tuxdirstat
%{_datadir}/fish/vendor_completions.d/tuxdirstat*.fish
%{_datadir}/kio/servicemenus/tuxdirstat.desktop
%{_datadir}/nemo/actions/tuxdirstat.nemo_action
%{_datadir}/nautilus-python/extensions/tuxdirstat.py

%changelog
* Fri Nov 06 2026 Gubir34 - 1.5-1
- Sunburst view, SSH remote scans, size history, HTML/PNG export, package and btrfs
  views, terminal interface, file manager menus, translations in po files
