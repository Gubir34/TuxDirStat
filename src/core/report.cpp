#include "report.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>

#include <sys/stat.h>

#include "extinfo.h"
#include "session.h"

#ifndef TUXDIRSTAT_VERSION
#define TUXDIRSTAT_VERSION "dev"
#endif

// ───────────────────────────── Ayarlar ─────────────────────────────

static std::string configDir()
{
    const char *x = std::getenv("XDG_CONFIG_HOME");
    return (x && *x ? std::string(x) : homeDir() + "/.config") + "/tuxdirstat";
}

Settings Settings::load()
{
    Settings s;
    std::ifstream f(configDir() + "/settings.conf");
    if (!f) {  // 1.2 öncesinin adı: DirTree
        const std::string dir = configDir();
        f.open(dir.substr(0, dir.rfind('/')) + "/dirtree/settings.conf");
    }
    std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#')
            continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        const bool b = v == "1" || v == "true";
        if (k == "apparent")
            s.apparent = b;
        else if (k == "one_fs")
            s.oneFs = b;
        else if (k == "grouping")
            s.grouping = b;
        else if (k == "show_treemap")
            s.showTreemap = b;
        else if (k == "theme" && (v == "system" || v == "light" || v == "dark"))
            s.theme = v;
        else if (k == "group_min")
            s.groupMin = uint32_t(std::clamp(std::atoi(v.c_str()), 2, 100000));
    }
    return s;
}

void Settings::save() const
{
    const std::string dir = configDir();
    ::mkdir((dir.substr(0, dir.rfind('/'))).c_str(), 0700);
    ::mkdir(dir.c_str(), 0700);
    std::ofstream f(dir + "/settings.conf");
    f << "# TuxDirStat ayarları (tüm arayüzler paylaşır)\n"
      << "apparent=" << apparent << "\n"
      << "one_fs=" << oneFs << "\n"
      << "grouping=" << grouping << "\n"
      << "group_min=" << groupMin << "\n"
      << "show_treemap=" << showTreemap << "\n"
      << "theme=" << theme << "\n";
}

static bool darkColor(const std::string &rgb)
{
    int r = 255, g = 255, b = 255;
    return std::sscanf(rgb.c_str(), "%d,%d,%d", &r, &g, &b) == 3 && 0.299 * r + 0.587 * g + 0.114 * b < 128;
}

bool systemPrefersDark()
{
    if (const char *gt = std::getenv("GTK_THEME"); gt && std::strstr(gt, ":dark"))
        return true;
    // KDE: pencere arka plan rengi
    std::ifstream kde(homeDir() + "/.config/kdeglobals");
    std::string line;
    bool inWindow = false;
    while (std::getline(kde, line)) {
        if (!line.empty() && line[0] == '[')
            inWindow = line == "[Colors:Window]";
        else if (inWindow && line.rfind("BackgroundNormal=", 0) == 0)
            return darkColor(line.substr(17));
    }
    // GNOME ve türevleri
    if (FILE *p = popen("gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null", "r")) {
        char buf[64] = {};
        const bool ok = std::fgets(buf, sizeof buf, p) != nullptr;
        pclose(p);
        if (ok && std::strstr(buf, "prefer-dark"))
            return true;
    }
    return false;
}

bool wantsDark(const std::string &theme)
{
    return theme == "dark" || (theme == "system" && systemPrefersDark());
}

// ───────────────────────────── Rapor ─────────────────────────────

static std::string pad(const std::string &s, size_t w)
{
    return s.size() >= w ? s : std::string(w - s.size(), ' ') + s;
}

void writeReport(std::ostream &out, Node *root, const ReportOptions &o, double seconds)
{
    const bool ap = o.apparent;
    const uint64_t total = std::max<uint64_t>(1, root->sizeOf(ap));
    const std::string base = root->path();
    auto rel = [&](const Node *n) {
        std::string p = n->path();
        return p.size() > base.size() ? p.substr(base.size() + (base == "/" ? 0 : 1)) : std::string(".");
    };
    auto line = [&](const Node *n, const std::string &label) {
        out << "  " << pad(fmtSize(n->sizeOf(ap)), 10) << "  " << pad(fmtPct(double(n->sizeOf(ap)) / total), 7) << "  "
            << label << "\n";
    };

    std::vector<Node *> dirs, files, groups;
    std::vector<Node *> stack{root};
    while (!stack.empty()) {
        Node *n = stack.back();
        stack.pop_back();
        for (Node *c : n->children) {
            if (c->flags & F_GROUP)
                groups.push_back(c);
            else if (c->isDir)
                dirs.push_back(c);
            else
                files.push_back(c);
            if (c->isDir)
                stack.push_back(c);
        }
    }
    auto bySize = [ap](const Node *a, const Node *b) { return a->sizeOf(ap) > b->sizeOf(ap); };
    auto topN = [&](std::vector<Node *> &v) {
        const size_t n = std::min(o.top, v.size());
        std::partial_sort(v.begin(), v.begin() + long(n), v.end(), bySize);
        v.resize(n);
    };

    char secs[32];
    std::snprintf(secs, sizeof secs, "%.2f", seconds);
    if (char *dot = std::strchr(secs, '.'))
        *dot = ',';
    out << "TuxDirStat raporu: " << base << "\n"
        << "Toplam: " << fmtSize(root->sizeOf(ap)) << " (" << (ap ? "görünür boyut" : "diskte kapladığı alan") << ") · "
        << fmtInt(root->files) << " dosya · " << fmtInt(root->dirs) << " klasör · tarama " << secs << " sn\n";

    topN(dirs);
    out << "\nEn büyük klasörler:\n";
    for (Node *d : dirs)
        line(d, rel(d) + "/");

    topN(files);
    out << "\nEn büyük dosyalar:\n";
    for (Node *f : files)
        line(f, rel(f));

    out << "\nUzantılar:\n";
    std::vector<ExtStat> st = computeExtStats(root);
    std::vector<uint32_t> ids;
    for (uint32_t i = 0; i < st.size(); ++i)
        if (st[i].count)
            ids.push_back(i);
    auto extBytes = [&](uint32_t i) { return ap ? st[i].size : st[i].alloc; };
    std::sort(ids.begin(), ids.end(), [&](uint32_t a, uint32_t b) { return extBytes(a) > extBytes(b); });
    for (size_t i = 0; i < std::min(o.top, ids.size()); ++i) {
        const uint32_t e = ids[i];
        out << "  " << pad(fmtSize(extBytes(e)), 10) << "  " << pad(fmtPct(double(extBytes(e)) / total), 7) << "  "
            << pad(fmtInt(st[e].count), 9) << " dosya  " << (e ? "." + ExtTable::name(e) : std::string("(uzantısız)"))
            << "  — " << describeExt(e) << "\n";
    }

    if (o.grouping) {
        std::sort(groups.begin(), groups.end(), [](const Node *a, const Node *b) {
            return a->files + a->dirs > b->files + b->dirs;
        });
        groups.resize(std::min(o.top, groups.size()));
        out << "\nEn kalabalık gruplar (eşik " << o.groupMin << "):\n";
        if (groups.empty())
            out << "  (grup yok)\n";
        for (Node *g : groups)
            line(g, rel(g->parent) + "/" + displayName(g, false));
    }
}

// ───────────────────────────── Komut satırı ─────────────────────────────

static void usage(const char *prog)
{
    std::cout << "Kullanım: " << prog << " [klasör]\n"
              << "          " << prog << " --report [seçenekler] <klasör>\n\n"
              << "Seçenekler (--report ile):\n"
              << "  --top N        her listede N satır (varsayılan 15)\n"
              << "  --apparent     görünür boyutu kullan (varsayılan: diskte kapladığı alan)\n"
              << "  --group        benzer öğeleri grupla ve en kalabalık grupları listele\n"
              << "  --group-min N  grup için en az öğe sayısı (varsayılan " << GROUP_MIN << ")\n"
              << "  --all-fs       başka dosya sistemlerine de geç\n"
              << "  -h, --help     bu yardımı göster\n"
              << "  --version      sürümü göster\n";
}

std::string pathArgument(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '-') {
            if (!std::strcmp(argv[i], "--top") || !std::strcmp(argv[i], "--group-min"))
                ++i;  // değer alan seçenekler
            continue;
        }
        std::string p = argv[i];
        if (p.rfind("~/", 0) == 0)
            p = homeDir() + p.substr(1);
        return p;
    }
    return {};
}

int runCommandLine(int argc, char **argv)
{
    const char *prog = std::strrchr(argv[0], '/') ? std::strrchr(argv[0], '/') + 1 : argv[0];
    bool report = false, oneFs = true;
    ReportOptions o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> long {
            if (i + 1 >= argc) {
                std::cerr << prog << ": " << a << " bir değer bekliyor\n";
                std::exit(2);
            }
            return std::atol(argv[++i]);
        };
        if (a == "-h" || a == "--help") {
            usage(prog);
            return 0;
        } else if (a == "--version") {
            std::cout << "TuxDirStat " << TUXDIRSTAT_VERSION << "\n";
            return 0;
        } else if (a == "--report") {
            report = true;
        } else if (a == "--top") {
            o.top = size_t(std::max(1L, value()));
        } else if (a == "--apparent") {
            o.apparent = true;
        } else if (a == "--group") {
            o.grouping = true;
        } else if (a == "--group-min") {
            o.groupMin = uint32_t(std::clamp(value(), 2L, 100000L));
        } else if (a == "--all-fs") {
            oneFs = false;
        } else if (a[0] == '-') {
            // Araç takımlarının kendi seçenekleri (ör. GTK'nin --display) arayüze kalır.
            if (report) {
                std::cerr << prog << ": bilinmeyen seçenek: " << a << "\n";
                return 2;
            }
        }
    }
    if (!report)
        return -1;
    std::string path = pathArgument(argc, argv);
    if (path.empty()) {
        std::cerr << prog << ": --report için bir klasör verin\n";
        return 2;
    }
    if (char *real = realpath(path.c_str(), nullptr)) {
        path = real;
        std::free(real);
    }
    Scanner sc(path, oneFs, scanWorkers());
    sc.start();
    while (!sc.isDone())
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (!sc.error.empty()) {
        std::cerr << prog << ": " << sc.error << "\n";
        return 1;
    }
    std::unique_ptr<Tree> tree = sc.takeTree();
    if (o.grouping)
        groupFiles(tree->root, tree->scratch(), o.groupMin);
    writeReport(std::cout, tree->root, o, sc.elapsed);
    return 0;
}
