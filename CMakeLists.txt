cmake_minimum_required(VERSION 3.21)
project(TuxDirStat VERSION 1.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Her arayüz ayrı bir program olarak derlenir (GTK 3 ve GTK 4 aynı süreçte yüklenemez).
option(TUXDIRSTAT_QT   "Qt 6 arayüzü (tuxdirstat)"        ON)
option(TUXDIRSTAT_GTK3 "GTK 3 arayüzü (tuxdirstat-gtk3)"  OFF)
option(TUXDIRSTAT_GTK4 "GTK 4 arayüzü (tuxdirstat-gtk4)"  OFF)
option(TUXDIRSTAT_TK   "Tk arayüzü (tuxdirstat-tk)"       OFF)
option(TUXDIRSTAT_TESTS "Çekirdek birim testleri (ctest)" OFF)

find_package(Threads REQUIRED)
find_package(PkgConfig)
include(GNUInstallDirs)
set(TUXDIRSTAT_PROGRAMS "")

add_library(tuxdirstat_core STATIC
  src/core/core.cpp
  src/core/session.cpp
  src/core/treemap_render.cpp
  src/core/report.cpp
  src/core/extinfo.cpp
  src/core/tools.cpp
  src/core/i18n.cpp
)
target_include_directories(tuxdirstat_core PUBLIC src/core)
target_compile_options(tuxdirstat_core PRIVATE -Wall -Wextra)
target_compile_definitions(tuxdirstat_core PUBLIC TUXDIRSTAT_VERSION="${PROJECT_VERSION}"
                                                  TUXDIRSTAT_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}")
target_link_libraries(tuxdirstat_core PUBLIC Threads::Threads)

function(tuxdirstat_desktop target name categories)
  set(DT_NAME "${name}")
  set(DT_EXEC "${target}")
  set(DT_CATS "${categories}")
  configure_file(${CMAKE_SOURCE_DIR}/tuxdirstat.desktop.in ${CMAKE_BINARY_DIR}/${target}.desktop @ONLY)
  install(TARGETS ${target})
  install(FILES ${CMAKE_BINARY_DIR}/${target}.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  set(TUXDIRSTAT_PROGRAMS ${TUXDIRSTAT_PROGRAMS} ${target} PARENT_SCOPE)
endfunction()

if(TUXDIRSTAT_QT)
  find_package(Qt6 REQUIRED COMPONENTS Widgets)
  add_executable(tuxdirstat
    src/qt/main.cpp
    src/qt/treemodel.cpp
    src/qt/treemap.cpp
    src/qt/mainwindow.cpp
    src/qt/theme.cpp
    src/qt/resultdialog.cpp
  )
  set_target_properties(tuxdirstat PROPERTIES AUTOMOC ON)
  target_compile_options(tuxdirstat PRIVATE -Wall -Wextra)
  target_link_libraries(tuxdirstat PRIVATE tuxdirstat_core Qt6::Widgets)
  tuxdirstat_desktop(tuxdirstat "TuxDirStat" "Qt;System;Filesystem;")
endif()

if(TUXDIRSTAT_GTK3)
  pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
  add_executable(tuxdirstat-gtk3 src/gtk/main.cpp)
  target_compile_options(tuxdirstat-gtk3 PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
  target_link_libraries(tuxdirstat-gtk3 PRIVATE tuxdirstat_core PkgConfig::GTK3)
  tuxdirstat_desktop(tuxdirstat-gtk3 "TuxDirStat (GTK 3)" "GTK;System;Filesystem;")
endif()

if(TUXDIRSTAT_GTK4)
  pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
  add_executable(tuxdirstat-gtk4 src/gtk/main.cpp)
  target_compile_options(tuxdirstat-gtk4 PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
  target_link_libraries(tuxdirstat-gtk4 PRIVATE tuxdirstat_core PkgConfig::GTK4)
  tuxdirstat_desktop(tuxdirstat-gtk4 "TuxDirStat (GTK 4)" "GTK;System;Filesystem;")
endif()

if(TUXDIRSTAT_TK)
  pkg_check_modules(TK REQUIRED IMPORTED_TARGET tk)
  pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
  add_executable(tuxdirstat-tk src/tk/main.cpp)
  target_compile_options(tuxdirstat-tk PRIVATE -Wall -Wextra)
  target_link_libraries(tuxdirstat-tk PRIVATE tuxdirstat_core PkgConfig::TK PkgConfig::GIO)
  tuxdirstat_desktop(tuxdirstat-tk "TuxDirStat (Tk)" "System;Filesystem;")
endif()

if(TUXDIRSTAT_TESTS)
  enable_testing()
  add_executable(tuxdirstat_core_test tests/core_test.cpp)
  target_compile_options(tuxdirstat_core_test PRIVATE -Wall -Wextra)
  target_link_libraries(tuxdirstat_core_test PRIVATE tuxdirstat_core)
  add_test(NAME core COMMAND tuxdirstat_core_test)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME translations COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tests/check_i18n.py)
  endif()
endif()

# Simge, AppStream bilgisi, kılavuz sayfası ve kabuk tamamlamaları
set(ICONDIR ${CMAKE_INSTALL_DATADIR}/icons/hicolor)
install(FILES data/tuxdirstat.svg DESTINATION ${ICONDIR}/scalable/apps)
install(FILES data/tuxdirstat-48.png DESTINATION ${ICONDIR}/48x48/apps RENAME tuxdirstat.png)
install(FILES data/tuxdirstat-256.png DESTINATION ${ICONDIR}/256x256/apps RENAME tuxdirstat.png)
if(TUXDIRSTAT_QT)
  install(FILES data/io.github.gubir34.TuxDirStat.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
endif()
install(FILES data/tuxdirstat.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
install(FILES completions/_tuxdirstat DESTINATION ${CMAKE_INSTALL_DATADIR}/zsh/site-functions)
foreach(prog ${TUXDIRSTAT_PROGRAMS})
  if(NOT prog STREQUAL "tuxdirstat")
    file(WRITE ${CMAKE_BINARY_DIR}/${prog}.1 ".so man1/tuxdirstat.1\n")
    install(FILES ${CMAKE_BINARY_DIR}/${prog}.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
  endif()
  install(FILES completions/tuxdirstat.bash DESTINATION ${CMAKE_INSTALL_DATADIR}/bash-completion/completions RENAME ${prog})
  install(FILES completions/tuxdirstat.fish DESTINATION ${CMAKE_INSTALL_DATADIR}/fish/vendor_completions.d RENAME ${prog}.fish)
endforeach()

if(NOT TUXDIRSTAT_QT AND NOT TUXDIRSTAT_GTK3 AND NOT TUXDIRSTAT_GTK4 AND NOT TUXDIRSTAT_TK)
  message(FATAL_ERROR "En az bir arayüz seçin: -DTUXDIRSTAT_QT/GTK3/GTK4/TK=ON")
endif()
