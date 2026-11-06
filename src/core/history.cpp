// Size history (weekly snapshots recorded by a systemd user timer) and the cached
// last scan that is reopened at the next start.

#include "tools.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <ostream>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>

#include "i18n.h"
#include "session.h"

namespace {

std::string xdg(const char *var, const char *fallback)
{
    const char *v = std::getenv(var);
    return v && *v == '/' ? std::string(v) : homeDir() + "/" + fallback;
}

bool makeDirs(const std::string &path)
{
    for (size_t i = 1; i <= path.size(); ++i)
        if (i == path.size() || path[i] == '/')
            if (::mkdir(path.substr(0, i).c_str(), 0700) != 0 && errno != EEXIST)
                return false;
    return true;
}

// Folder for one path's history: a readable part plus a hash so different paths never meet.
std::string historyDir(const std::string &path)
{
    std::string name;
    for (char c : path)
        name += std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' ? c : '-';
    while (!name.empty() && name.front() == '-')
        name.erase(0, 1);
    if (name.size() > 60)
        name.resize(60);
    uint64_t h = 1469598103934665603ULL;  // FNV-1a
    for (unsigned char c : path)
        h = (h ^ c) * 1099511628211ULL;
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(h));
    return xdg("XDG_DATA_HOME", ".local/share") + "/tuxdirstat/history/" + (name.empty() ? "root" : name) + "-" + std::string(hex, 8);
}

std::string configFile()
{
    return xdg("XDG_CONFIG_HOME", ".config") + "/tuxdirstat/history.conf";
}

void writePaths(const std::vector<std::string> &paths)
{
    const std::string f = configFile();
    makeDirs(f.substr(0, f.rfind('/')));
    std::ofstream o(f);
    o << "# Folders whose size TuxDirStat records weekly (one per line)\n";
    for (const std::string &p : paths)
        o << p << "\n";
}

std::string today()
{
    const std::time_t now = std::time(nullptr);
    char b[16];
    std::strftime(b, sizeof b, "%Y-%m-%d", std::localtime(&now));
    return b;
}

constexpr size_t KEEP = 60;  // entries per folder (more than a year of weeks)

bool saveEntry(const Node *root, const std::string &path, std::string *err)
{
    const std::string dir = historyDir(path);
    if (!makeDirs(dir)) {
        if (err)
            *err = std::strerror(errno);
        return false;
    }
    const std::string file = dir + "/" + today() + ".json.gz";
    const std::string tmp = dir + "/." + today() + ".part.json.gz";  // still .gz, so it is compressed
    if (!saveSnapshot(root, tmp, err))
        return false;
    if (::rename(tmp.c_str(), file.c_str()) != 0) {
        if (err)
            *err = std::strerror(errno);
        return false;
    }
    std::vector<std::pair<int64_t, std::string>> all = historyFiles(path);
    for (size_t i = 0; all.size() > KEEP && i < all.size() - KEEP; ++i)
        ::unlink(all[i].second.c_str());
    return true;
}

const char *const SERVICE = R"([Unit]
Description=TuxDirStat: record folder sizes for the growth view
Documentation=man:tuxdirstat(1)

[Service]
Type=oneshot
Nice=19
IOSchedulingClass=idle
ExecStart=%1 --history-record
)";

const char *const TIMER = R"([Unit]
Description=Weekly TuxDirStat size history

[Timer]
OnCalendar=weekly
Persistent=true
RandomizedDelaySec=1h

[Install]
WantedBy=timers.target
)";

std::string sparkline(const std::vector<uint64_t> &v)
{
    static const char *bars[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    if (v.empty())
        return {};
    const auto [lo, hi] = std::minmax_element(v.begin(), v.end());
    std::string s;
    for (uint64_t x : v)
        s += bars[*hi == *lo ? 3 : size_t((x - *lo) * 7 / (*hi - *lo))];
    return s;
}

std::string signedSize(int64_t d)
{
    if (d == 0)
        return "0";
    return (d > 0 ? "+" : "−") + fmtSize(uint64_t(d > 0 ? d : -d));
}

}  // namespace

std::vector<std::string> historyPaths()
{
    std::vector<std::string> out;
    std::ifstream f(configFile());
    std::string line;
    while (std::getline(f, line))
        if (!line.empty() && line[0] == '/' && std::find(out.begin(), out.end(), line) == out.end())
            out.push_back(line);
    return out;
}

bool historyRecording(const std::string &path)
{
    const auto p = historyPaths();
    return std::find(p.begin(), p.end(), path) != p.end();
}

bool setHistoryRecording(const std::string &path, bool on, std::string *err)
{
    std::vector<std::string> paths = historyPaths();
    paths.erase(std::remove(paths.begin(), paths.end(), path), paths.end());
    if (on)
        paths.push_back(path);
    writePaths(paths);
    if (!haveProgram("systemctl")) {
        if (err && on)
            *err = T("systemd was not found; run \"tuxdirstat --history-record\" regularly yourself (e.g. from cron).");
        return !on;
    }
    const std::string unitDir = xdg("XDG_CONFIG_HOME", ".config") + "/systemd/user";
    if (on) {
        if (!makeDirs(unitDir)) {
            if (err)
                *err = std::strerror(errno);
            return false;
        }
        std::ofstream(unitDir + "/tuxdirstat-history.service") << TF(SERVICE, {selfExe()});
        std::ofstream(unitDir + "/tuxdirstat-history.timer") << TIMER;
    }
    std::string out;
    runCapture({"systemctl", "--user", "daemon-reload"}, &out, 15000);
    const bool any = !paths.empty();
    const int rc = runCapture({"systemctl", "--user", any ? "enable" : "disable", "--now", "tuxdirstat-history.timer"}, &out, 15000);
    if (rc != 0 && any) {
        if (err)
            *err = T("Could not enable the weekly timer (systemctl --user enable --now tuxdirstat-history.timer).");
        return false;
    }
    return true;
}

bool recordHistorySnapshot(const Node *root, std::string *err)
{
    if (!isLocalTree(root)) {
        if (err)
            *err = T("Only local folders can be recorded.");
        return false;
    }
    return saveEntry(root, root->path(), err);
}

int recordHistory(std::ostream &log)
{
    int rc = 0;
    const auto paths = historyPaths();
    if (paths.empty())
        log << T("No folders are recorded. Turn it on in the Tools menu (Record size history).") << "\n";
    for (const std::string &p : paths) {
        Scanner sc(p, true, scanWorkers());
        sc.start();
        while (!sc.isDone())
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::string err = sc.error;
        std::unique_ptr<Tree> t = err.empty() ? sc.takeTree() : nullptr;
        if (t && saveEntry(t->root, p, &err)) {
            log << TF("Recorded %1: %2", {p, fmtSize(t->root->alloc)}) << "\n";
        } else {
            log << p << ": " << (err.empty() ? std::string(T("scan cancelled")) : err) << "\n";
            rc = 1;
        }
    }
    return rc;
}

std::vector<std::pair<int64_t, std::string>> historyFiles(const std::string &path)
{
    std::vector<std::pair<int64_t, std::string>> out;
    const std::string dir = historyDir(path);
    DIR *d = ::opendir(dir.c_str());
    if (!d)
        return out;
    while (dirent *e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name.size() != 18 || name.compare(10, 8, ".json.gz") != 0)
            continue;
        struct tm tm{};
        if (!strptime(name.c_str(), "%Y-%m-%d", &tm))
            continue;
        tm.tm_hour = 12;
        tm.tm_isdst = -1;
        out.push_back({int64_t(std::mktime(&tm)), dir + "/" + name});
    }
    ::closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

ResultTable historyGrowth(Node *root, bool apparent, Job &job)
{
    ResultTable t;
    t.title = T("Size history");
    const std::string base = root->path();
    t.emptyText = TF("No size history for %1 yet. Turn on \"Record size history\" in the Tools menu; it saves the "
                     "current scan now and the sizes every week.",
                     {base});
    auto files = historyFiles(base);
    if (!isLocalTree(root) || files.empty())
        return t;
    constexpr size_t COLS = 6;
    if (files.size() > COLS)
        files.erase(files.begin(), files.end() - COLS);
    // name of a top-level item → size at each date (0 = absent)
    std::vector<std::string> order;
    std::unordered_map<std::string, std::vector<uint64_t>> sizes;
    std::vector<uint64_t> totals;
    const size_t ncol = files.size() + 1;
    auto addTree = [&](const Node *r, size_t col) {
        std::vector<Node *> kids;
        realChildren(r, kids);
        for (const Node *c : kids) {
            const std::string name(c->nameView());
            auto [it, fresh] = sizes.try_emplace(name, std::vector<uint64_t>(ncol, 0));
            if (fresh)
                order.push_back(name);
            it->second[col] = c->sizeOf(apparent);
        }
        totals.push_back(r->sizeOf(apparent));
    };
    std::vector<std::string> dates;
    for (size_t i = 0; i < files.size(); ++i) {
        if (job.cancelled())
            return t;
        job.setStatus(TF("Reading %1…", {files[i].second}));
        job.setProgress(double(i) / double(ncol));
        std::string err;
        std::unique_ptr<Tree> old = loadSnapshot(files[i].second, &err, nullptr, job.cancelFlag());
        char b[32];
        const std::time_t tt = std::time_t(files[i].first);
        std::strftime(b, sizeof b, i18n::dateFormat(), std::localtime(&tt));
        std::string d = b;
        d = d.substr(0, d.find(' '));  // date without time
        dates.push_back(d);
        if (old)
            addTree(old->root, i);
        else
            totals.push_back(0);
    }
    addTree(root, files.size());
    t.columns = {T("Name")};
    t.numeric = {false};
    for (const std::string &d : dates) {
        t.columns.push_back(d);
        t.numeric.push_back(true);
    }
    for (const char *c : {N_("Now"), N_("Change"), N_("Trend")}) {
        t.columns.push_back(T(c));
        t.numeric.push_back(std::strcmp(c, "Trend") != 0);
    }
    auto makeRow = [&](const std::string &name, const std::vector<uint64_t> &v, Node *node) {
        ResultRow r;
        r.cells.push_back(name);
        for (uint64_t x : v)
            r.cells.push_back(x ? fmtSize(x) : "–");
        const int64_t change = int64_t(v.back()) - int64_t(v.front());
        r.cells.push_back(signedSize(change));
        r.cells.push_back(sparkline(v));
        r.bytes = uint64_t(change < 0 ? -change : change);
        r.node = node;
        r.path = node ? node->path() : std::string();
        r.deletable = false;
        return r;
    };
    std::vector<Node *> kids;
    realChildren(root, kids);
    std::unordered_map<std::string, Node *> live;
    for (Node *c : kids)
        live[std::string(c->nameView())] = c;
    for (const std::string &name : order) {
        auto it = live.find(name);
        t.rows.push_back(makeRow(name + (it != live.end() && it->second->isDir ? "/" : ""), sizes[name], it != live.end() ? it->second : nullptr));
    }
    std::stable_sort(t.rows.begin(), t.rows.end(), [](const ResultRow &a, const ResultRow &b) { return a.bytes > b.bytes; });
    if (t.rows.size() > 200)
        t.rows.resize(200);
    t.rows.insert(t.rows.begin(), makeRow(T("(total)"), totals, root));
    const int64_t change = int64_t(totals.back()) - int64_t(totals.front());
    t.summary = TF("%1 recorded sizes since %2 · total %3 → %4 (%5)",
                   {fmtInt(historyFiles(base).size()), dates.front(), fmtSize(totals.front()), fmtSize(totals.back()), signedSize(change)});
    if (!historyRecording(base))
        t.summary += "\n" + std::string(T("Weekly recording is off for this folder."));
    return t;
}

std::string lastScanFile()
{
    return xdg("XDG_CACHE_HOME", ".cache") + "/tuxdirstat/last-scan.json.gz";
}

bool saveLastScan(const Node *root, std::string *err)
{
    if (!isLocalTree(root)) {
        if (err)
            *err = T("Only local scans are kept.");
        return false;
    }
    if (uint64_t(root->files) + root->dirs > 3000000) {  // keeps the cache file reasonable
        ::unlink(lastScanFile().c_str());
        return true;
    }
    const std::string file = lastScanFile();
    makeDirs(file.substr(0, file.rfind('/')));
    const std::string tmp = file.substr(0, file.size() - 8) + ".part.json.gz";
    if (!saveSnapshot(root, tmp, err))
        return false;
    return ::rename(tmp.c_str(), file.c_str()) == 0;
}
