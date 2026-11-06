#pragma once
// Arayüzlerin ortak ayarları ve arayüz açmadan çalışan komut satırı (--report).

#include <cstdint>
#include <ostream>
#include <string>

#include "core.h"

// ~/.config/tuxdirstat/settings.conf içinde saklanan, dört arayüzün paylaştığı ayarlar.
struct Settings {
    bool apparent = false;
    bool oneFs = true;
    bool grouping = false;
    bool showTreemap = true;
    uint32_t groupMin = GROUP_MIN;
    std::string theme = "system";     // "system", "light" ya da "dark"
    std::string language = "system";  // "system", "en" ya da "tr"
    bool colorByAge = false;          // treemap'i son değişiklik yaşına göre renklendir

    static Settings load();
    void save() const;
};

// Masaüstünün koyu tema tercih edip etmediği (KDE renk şeması, GNOME color-scheme, GTK_THEME).
bool systemPrefersDark();

// "system" ayarını çözer: koyu tema kullanılmalı mı?
bool wantsDark(const std::string &theme);

// Arayüzde sunulan grup eşiği seçenekleri.
constexpr uint32_t GROUP_MIN_CHOICES[] = {8, 16, 32, 64, 128};

struct ReportOptions {
    size_t top = 15;
    bool apparent = false;
    bool grouping = false;
    uint32_t groupMin = GROUP_MIN;
};
void writeReport(std::ostream &out, Node *root, const ReportOptions &o, double seconds);

// --help, --version ve --report seçeneklerini işler. Arayüz başlatılacaksa -1,
// aksi halde sürecin çıkış kodunu döndürür.
int runCommandLine(int argc, char **argv);

// Komut satırındaki ilk seçenek olmayan argüman (taranacak klasör); "~/" genişletilir.
std::string pathArgument(int argc, char **argv);
