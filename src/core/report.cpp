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
#include "tools.h"
#include "i18n.h"
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
        else if (k == "language" && (v == "system" || v == "en" || v == "tr"))
            s.language = v;
        else if (k == "color_by_age")
            s.colorByAge = b;
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
    f << "# TuxDirStat settings (shared by all interfaces)\n"
      << "apparent=" << apparent << "\n"
      << "one_fs=" << oneFs << "\n"
      << "grouping=" << grouping << "\n"
      << "group_min=" << groupMin << "\n"
      << "show_treemap=" << showTreemap << "\n"
      << "theme=" << theme << "\n"
      << "language=" << language << "\n"
      << "color_by_age=" << colorByAge << "\n";
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

    out << TF("TuxDirStat report: %1", {base}) << "\n"
        << TF("Total: %1 (%2) · %3 files · %4 folders · scanned in %5 s",
              {fmtSize(root->sizeOf(ap)), ap ? T("apparent size") : T("size on disk"), fmtInt(root->files),
               fmtInt(root->dirs), fmtDecimal(seconds, 2)})
        << "\n";

    topN(dirs);
    out << "\n" << T("Largest folders:") << "\n";
    for (Node *d : dirs)
        line(d, rel(d) + "/");

    topN(files);
    out << "\n" << T("Largest files:") << "\n";
    for (Node *f : files)
        line(f, rel(f));

    out << "\n" << T("Extensions:") << "\n";
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
            << pad(fmtInt(st[e].count), 9) << " " << T("files") << "  " << (e ? "." + ExtTable::name(e) : std::string(T("(no extension)")))
            << "  — " << describeExt(e) << "\n";
    }

    if (o.grouping) {
        std::sort(groups.begin(), groups.end(), [](const Node *a, const Node *b) {
            return a->files + a->dirs > b->files + b->dirs;
        });
        groups.resize(std::min(o.top, groups.size()));
        out << "\n" << TF("Most crowded groups (threshold %1):", {std::to_string(o.groupMin)}) << "\n";
        if (groups.empty())
            out << "  " << T("(no groups)") << "\n";
        for (Node *g : groups)
            line(g, rel(g->parent) + "/" + displayName(g, false));
    }
}

// ───────────────────────────── Komut satırı ─────────────────────────────

namespace {

bool takesValue(const std::string &a)
{
    return a == "--top" || a == "--group-min" || a == "--find" || a == "--export" || a == "--csv" ||
           a == "--compare" || a == "--import" || a == "--lang";
}

void usage(const char *prog)
{
    std::cout << TF("Usage: %1 [folder]", {prog}) << "\n"
              << "       " << TF("%1 [options] <folder>   (no window when an output option is given)", {prog}) << "\n\n"
              << T("Output options (can be combined):") << "\n"
              << "  --report          " << T("text report: largest folders, files and extensions") << "\n"
              << "  --find QUERY      " << T("search, e.g. --find '*.iso >1G older:1y'") << "\n"
              << "  --duplicates      " << T("list duplicate files (compares contents)") << "\n"
              << "  --cleanup         " << T("caches, build output and other things that can be cleaned") << "\n"
              << "  --owners          " << T("disk usage per owner") << "\n"
              << "  --unowned         " << T("files under /usr, /etc and /opt that no pacman package owns") << "\n"
              << "  --export FILE     " << T("save the scan as a snapshot (ncdu JSON format)") << "\n"
              << "  --csv FILE        " << T("write every file and folder to a CSV file") << "\n"
              << "  --compare FILE    " << T("show what changed since an earlier snapshot") << "\n\n"
              << T("Other options:") << "\n"
              << "  --import FILE     " << T("read a snapshot (TuxDirStat or ncdu -o) instead of scanning") << "\n"
              << "  --top N           " << TF("rows per report list (default %1)", {"15"}) << "\n"
              << "  --apparent        " << T("use apparent size (default: size on disk)") << "\n"
              << "  --group           " << T("group similar items and list the most crowded groups") << "\n"
              << "  --group-min N     " << TF("minimum items per group (default %1)", {std::to_string(GROUP_MIN)}) << "\n"
              << "  --all-fs          " << T("cross into other file systems") << "\n"
              << "  --lang en|tr      " << T("interface language") << "\n"
              << "  -h, --help        " << T("show this help") << "\n"
              << "  --version         " << T("show the version") << "\n";
}

}  // namespace

std::string pathArgument(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--") {
            if (i + 1 < argc) {
                std::string p = argv[i + 1];
                if (p.rfind("~/", 0) == 0)
                    p = homeDir() + p.substr(1);
                return p;
            }
            return {};
        }
        if (a[0] == '-') {
            if (takesValue(a))
                ++i;
            continue;
        }
        std::string p = a;
        if (p.rfind("~/", 0) == 0)
            p = homeDir() + p.substr(1);
        return p;
    }
    return {};
}

int runCommandLine(int argc, char **argv)
{
    const char *prog = std::strrchr(argv[0], '/') ? std::strrchr(argv[0], '/') + 1 : argv[0];
    std::string lang;
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--lang"))
            lang = argv[i + 1];
    i18n::init(lang.empty() ? Settings::load().language : lang);
    i18n::applyToEnvironment();

    bool oneFs = true, cli = false;
    ReportOptions o;
    // Çıktılar komut satırındaki sırayla üretilir.
    std::vector<std::pair<std::string, std::string>> outputs;
    std::string importFile;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--")
            break;
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << prog << ": " << TF("%1 needs a value", {a}) << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            usage(prog);
            return 0;
        } else if (a == "--version") {
            std::cout << "TuxDirStat " << TUXDIRSTAT_VERSION << "\n";
            return 0;
        } else if (a == "--report" || a == "--duplicates" || a == "--cleanup" || a == "--owners" || a == "--unowned") {
            outputs.emplace_back(a, "");
        } else if (a == "--find" || a == "--export" || a == "--csv" || a == "--compare") {
            outputs.emplace_back(a, value());
        } else if (a == "--import") {
            importFile = value();
            cli = true;
        } else if (a == "--top") {
            o.top = size_t(std::max(1L, std::atol(value().c_str())));
        } else if (a == "--apparent") {
            o.apparent = true;
        } else if (a == "--group") {
            o.grouping = true;
        } else if (a == "--group-min") {
            o.groupMin = uint32_t(std::clamp(std::atol(value().c_str()), 2L, 100000L));
        } else if (a == "--all-fs") {
            oneFs = false;
        } else if (a == "--lang") {
            value();
        } else if (a[0] == '-') {
            // Araç takımlarının kendi seçenekleri (ör. GTK'nin --display) arayüze kalır.
            cli = cli || !outputs.empty();
            if (cli) {
                std::cerr << prog << ": " << TF("unknown option: %1", {a}) << "\n";
                return 2;
            }
        }
    }
    if (outputs.empty() && !cli)
        return -1;
    if (outputs.empty())
        outputs.emplace_back("--report", "");

    std::unique_ptr<Tree> tree;
    double seconds = 0;
    if (!importFile.empty()) {
        std::string err;
        auto t0 = std::chrono::steady_clock::now();
        tree = loadSnapshot(importFile, &err);
        if (!tree) {
            std::cerr << prog << ": " << importFile << ": " << err << "\n";
            return 1;
        }
        seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    } else {
        std::string path = pathArgument(argc, argv);
        if (path.empty()) {
            std::cerr << prog << ": " << T("give a folder to scan (or --import FILE)") << "\n";
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
        tree = sc.takeTree();
        seconds = sc.elapsed;
    }
    Node *root = tree->root;
    int rc = 0;
    bool first = true;
    auto sep = [&] {
        if (!first)
            std::cout << "\n";
        first = false;
    };
    Job job("", [](Job &) {});
    for (const auto &[opt, val] : outputs) {
        std::string err;
        if (opt == "--export") {
            if (!saveSnapshot(root, val, &err)) {
                std::cerr << prog << ": " << val << ": " << err << "\n";
                rc = 1;
            }
        } else if (opt == "--csv") {
            if (!exportCsv(root, val, &err)) {
                std::cerr << prog << ": " << val << ": " << err << "\n";
                rc = 1;
            }
        } else if (opt == "--report") {
            sep();
            if (o.grouping)
                groupFiles(root, tree->scratch(), o.groupMin);
            writeReport(std::cout, root, o, seconds);
            if (o.grouping)
                ungroupFiles(root, tree->scratch());
        } else {
            ResultTable t;
            if (opt == "--find")
                t = searchTree(root, val, o.apparent, &err);
            else if (opt == "--duplicates")
                t = findDuplicates(root, job);
            else if (opt == "--cleanup")
                t = findCleanup(root, o.apparent, job);
            else if (opt == "--owners")
                t = ownerUsage(root, o.apparent);
            else if (opt == "--unowned") {
                if (!havePacman()) {
                    std::cerr << prog << ": " << T("pacman was not found; --unowned works on Arch-based systems.") << "\n";
                    rc = 1;
                    continue;
                }
                t = findUnowned(root, o.apparent, job);
            } else if (opt == "--compare") {
                t = compareWithSnapshot(root, val, o.apparent, job);
            }
            if (err.empty())
                err = job.error;
            job.error.clear();
            if (!err.empty()) {
                std::cerr << prog << ": " << err << "\n";
                rc = 1;
                continue;
            }
            sep();
            printTable(std::cout, t);
        }
    }
    return rc;
}
