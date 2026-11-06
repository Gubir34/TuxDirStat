// Installed packages (pacman, Flatpak) and btrfs sharing/compression.

#include "tools.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include <fcntl.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#include "i18n.h"
#include "session.h"

namespace {

std::string trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return std::string(s);
}

// "1.50 MiB", "312.00 KiB", "1.2 GB", "980 bytes" → bytes.
uint64_t parseHumanSize(std::string_view s)
{
    std::string t = trim(s);
    for (size_t at; (at = t.find("\xc2\xa0")) != std::string::npos;)  // no-break space (Flatpak)
        t.replace(at, 2, " ");
    char *end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str())
        return 0;
    std::string unit = trim(end);
    for (char &c : unit)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    static const std::pair<const char *, double> units[] = {
        {"kib", 1024.0}, {"mib", 1048576.0}, {"gib", 1073741824.0}, {"tib", 1099511627776.0},
        {"kb", 1e3},     {"mb", 1e6},        {"gb", 1e9},           {"tb", 1e12},
        {"k", 1024.0},   {"m", 1048576.0},   {"g", 1073741824.0},   {"b", 1.0},
        {"bytes", 1.0},  {"byte", 1.0},      {"", 1.0}};
    for (const auto &[name, mul] : units)
        if (unit == name)
            return uint64_t(v * mul);
    return 0;
}

std::vector<std::string_view> lines(std::string_view s)
{
    std::vector<std::string_view> out;
    while (!s.empty()) {
        const size_t e = s.find('\n');
        out.push_back(s.substr(0, e));
        if (e == std::string_view::npos)
            break;
        s.remove_prefix(e + 1);
    }
    return out;
}

// Runs a program with the C locale so its output is parseable.
int runC(std::vector<std::string> args, std::string *out, int timeoutMs)
{
    std::vector<std::string> cmd{"env", "LC_ALL=C", "LANG=C"};
    cmd.insert(cmd.end(), args.begin(), args.end());
    return runCapture(cmd, out, timeoutMs);
}

}  // namespace

// ───────────────────────────── packages ─────────────────────────────

std::vector<PackageInfo> pacmanPackages(std::string *err)
{
    std::vector<PackageInfo> pkgs;
    std::string out;
    if (runC({"pacman", "-Qi"}, &out, 60000) != 0) {
        if (err)
            *err = T("pacman -Qi failed.");
        return pkgs;
    }
    PackageInfo cur;
    bool have = false;
    std::string lastKey;
    auto flush = [&] {
        if (have && !cur.name.empty())
            pkgs.push_back(cur);
        cur = PackageInfo{};
        have = false;
    };
    for (std::string_view l : lines(out)) {
        if (trim(l).empty()) {
            flush();
            continue;
        }
        const size_t colon = l.find(" : ");
        std::string key, val;
        if (colon == std::string_view::npos || l[0] == ' ') {
            val = trim(l);  // continuation of a long list
            key = lastKey;
        } else {
            key = trim(l.substr(0, colon));
            val = trim(l.substr(colon + 3));
            lastKey = key;
        }
        have = true;
        if (key == "Name")
            cur.name = val;
        else if (key == "Version")
            cur.version = val;
        else if (key == "Description")
            cur.description = val;
        else if (key == "Installed Size")
            cur.size = parseHumanSize(val);
        else if (key == "Install Reason")
            cur.explicitlyInstalled = val.rfind("Explicitly", 0) == 0;
        else if (key == "Required By" && val != "None") {
            size_t n = 0, at = 0;
            while ((at = val.find_first_not_of(' ', at)) != std::string::npos) {
                ++n;
                at = val.find(' ', at);
            }
            cur.requiredBy += int(n);
        }
    }
    flush();
    std::string orphans;
    if (runC({"pacman", "-Qdtq"}, &orphans, 30000) == 0) {
        std::unordered_set<std::string> names;
        for (std::string_view l : lines(orphans))
            if (!trim(l).empty())
                names.insert(trim(l));
        for (PackageInfo &p : pkgs)
            p.orphan = names.count(p.name) > 0;
    }
    return pkgs;
}

ResultTable packageSizes(Job &job)
{
    ResultTable t;
    t.title = T("Installed packages");
    t.columns = {T("Package"), T("Size"), T("Status"), T("Needed by"), T("Description")};
    t.numeric = {false, true, false, true, false};
    t.emptyText = T("No package manager was found (pacman or Flatpak).");
    uint64_t pacTotal = 0, orphanTotal = 0, flatTotal = 0;
    size_t orphanCount = 0, pacCount = 0, flatCount = 0;
    if (havePacman()) {
        job.setStatus(T("Reading the pacman database…"));
        std::string err;
        for (const PackageInfo &p : pacmanPackages(&err)) {
            ResultRow r;
            r.bytes = p.size;
            r.deletable = false;
            const char *status = p.orphan ? N_("Orphan (nothing needs it)") : p.explicitlyInstalled ? N_("Installed by you") : N_("Dependency");
            r.command = p.orphan || p.explicitlyInstalled ? "sudo pacman -Rns " + p.name : std::string();
            r.cells = {p.name + " " + p.version, fmtSize(p.size), T(status), p.requiredBy ? fmtInt(uint64_t(p.requiredBy)) : "",
                       p.description};
            t.rows.push_back(std::move(r));
            pacTotal += p.size;
            ++pacCount;
            if (p.orphan) {
                orphanTotal += p.size;
                ++orphanCount;
            }
        }
        if (!err.empty())
            job.error = err;
    }
    if (!job.cancelled() && haveProgram("flatpak")) {
        job.setStatus(T("Asking Flatpak about installed apps…"));
        for (const char *kind : {"--app", "--runtime"}) {
            std::string out;
            if (runC({"flatpak", "list", kind, "--columns=application,version,size,name"}, &out, 30000) != 0)
                continue;
            for (std::string_view l : lines(out)) {
                std::vector<std::string> f;
                size_t at = 0;
                while (true) {
                    const size_t tab = l.find('\t', at);
                    f.push_back(trim(l.substr(at, tab == std::string_view::npos ? std::string_view::npos : tab - at)));
                    if (tab == std::string_view::npos)
                        break;
                    at = tab + 1;
                }
                if (f.size() < 3 || f[0].empty())
                    continue;
                ResultRow r;
                r.bytes = parseHumanSize(f[2]);
                r.deletable = false;
                r.command = "flatpak uninstall " + f[0];
                r.cells = {f[0] + (f[1].empty() ? "" : " " + f[1]), fmtSize(r.bytes),
                           T(std::string(kind) == "--app" ? N_("Flatpak app") : N_("Flatpak runtime")), "",
                           f.size() > 3 ? f[3] : ""};
                t.rows.push_back(std::move(r));
                flatTotal += r.bytes;
                ++flatCount;
            }
        }
    }
    std::stable_sort(t.rows.begin(), t.rows.end(), [](const ResultRow &a, const ResultRow &b) { return a.bytes > b.bytes; });
    std::string sum;
    if (pacCount)
        sum = TF("%1 pacman packages use %2.", {fmtInt(pacCount), fmtSize(pacTotal)});
    if (orphanCount)
        sum += " " + TF("%1 orphans (%2) can be removed with: sudo pacman -Rns $(pacman -Qdtq)", {fmtInt(orphanCount), fmtSize(orphanTotal)});
    if (flatCount)
        sum += (sum.empty() ? "" : "\n") + TF("%1 Flatpak apps and runtimes use %2.", {fmtInt(flatCount), fmtSize(flatTotal)});
    t.summary = sum;
    return t;
}

// ───────────────────────────── btrfs ─────────────────────────────

namespace {

constexpr long BTRFS_MAGIC = 0x9123683E;

struct Extents {
    uint64_t total = 0, shared = 0, compressed = 0;
    bool ok = false;
};

Extents fileExtents(const std::string &path)
{
    Extents e;
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return e;
    constexpr unsigned N = 128;
    std::vector<char> buf(sizeof(fiemap) + N * sizeof(fiemap_extent));
    auto *fm = reinterpret_cast<fiemap *>(buf.data());
    uint64_t start = 0;
    bool last = false;
    while (!last) {
        std::memset(buf.data(), 0, buf.size());
        fm->fm_start = start;
        fm->fm_length = FIEMAP_MAX_OFFSET - start;
        fm->fm_flags = 0;  // no FIEMAP_FLAG_SYNC: analyzing must not force writeback
        fm->fm_extent_count = N;
        if (::ioctl(fd, FS_IOC_FIEMAP, fm) != 0)
            break;
        if (fm->fm_mapped_extents == 0) {
            e.ok = true;
            break;
        }
        for (unsigned i = 0; i < fm->fm_mapped_extents; ++i) {
            const fiemap_extent &x = fm->fm_extents[i];
            if (x.fe_flags & FIEMAP_EXTENT_DATA_INLINE)
                continue;
            e.total += x.fe_length;
            if (x.fe_flags & FIEMAP_EXTENT_SHARED)
                e.shared += x.fe_length;
            if (x.fe_flags & FIEMAP_EXTENT_ENCODED)
                e.compressed += x.fe_length;
            if (x.fe_flags & FIEMAP_EXTENT_LAST)
                last = true;
            start = x.fe_logical + x.fe_length;
        }
        e.ok = true;
    }
    ::close(fd);
    return e;
}

}  // namespace

bool isBtrfs(const std::string &path)
{
    struct statfs sf;
    return ::statfs(path.c_str(), &sf) == 0 && long(sf.f_type) == BTRFS_MAGIC;
}

ResultTable btrfsUsage(Node *root, Job &job)
{
    ResultTable t;
    t.title = T("Btrfs: shared and compressed data");
    t.columns = {T("Folder"), T("Kind"), T("Size on disk"), T("Shared"), T("Exclusive"), T("Compressed")};
    t.numeric = {false, false, true, true, true, true};
    const std::string base = root->path();
    if (!isLocalTree(root) || !isBtrfs(base)) {
        t.emptyText = T("This folder is not on a btrfs file system.");
        return t;
    }
    t.emptyText = T("No files to analyze.");
    // Rows: the top-level folders plus every subvolume (snapshots included) found in the scan.
    std::vector<Node *> rows;
    realChildren(root, rows);
    rows.erase(std::remove_if(rows.begin(), rows.end(), [](const Node *n) { return !n->isDir || (n->flags & F_MOUNT); }), rows.end());
    std::unordered_map<const Node *, std::string> kind;
    std::vector<Node *> files;
    constexpr uint64_t MIN_FILE = 128 << 10;
    uint64_t skipped = 0;
    std::vector<std::pair<Node *, std::string>> st{{root, base}};
    job.setStatus(T("Looking for subvolumes…"));
    while (!st.empty() && !job.cancelled()) {
        auto [d, path] = std::move(st.back());
        st.pop_back();
        std::vector<Node *> kids;
        realChildren(d, kids);
        for (Node *c : kids) {
            if (c->flags & (F_MOUNT | F_ERROR | F_LINK))
                continue;
            if (!c->isDir) {
                // Asking btrfs whether extents are shared is slow (hundreds of files per second);
                // small files hardly change the totals, so they are counted separately.
                if (c->alloc >= MIN_FILE)
                    files.push_back(c);
                else
                    skipped += c->alloc;
                continue;
            }
            const std::string p = (path == "/" ? "" : path) + "/" + std::string(c->nameView());
            struct stat sb;
            if (::lstat(p.c_str(), &sb) == 0 && sb.st_ino == 256) {  // the root of a btrfs subvolume
                const bool snap = p.find("snapshot") != std::string::npos || p.find("timeshift") != std::string::npos;
                kind[c] = T(snap ? N_("Snapshot") : N_("Subvolume"));
                if (std::find(rows.begin(), rows.end(), c) == rows.end())
                    rows.push_back(c);
            }
            st.push_back({c, p});
        }
    }
    struct Acc {
        uint64_t total = 0, shared = 0, compressed = 0;
    };
    std::unordered_map<const Node *, size_t> rowIndex;
    for (size_t i = 0; i < rows.size(); ++i)
        rowIndex[rows[i]] = i;
    std::vector<Acc> acc(rows.size());
    Acc all;
    std::mutex m;
    std::atomic<size_t> next{0}, done{0};
    const size_t nf = files.size();
    auto work = [&] {
        std::vector<Acc> mine(rows.size());
        Acc myAll;
        while (!job.cancelled()) {
            const size_t i = next.fetch_add(1);
            if (i >= nf)
                break;
            Node *f = files[i];
            const Extents e = fileExtents(f->path());
            if (e.ok) {
                myAll.total += e.total;
                myAll.shared += e.shared;
                myAll.compressed += e.compressed;
                for (const Node *p = f->parent; p; p = p->parent) {
                    auto it = rowIndex.find(p);
                    if (it != rowIndex.end()) {
                        Acc &a = mine[it->second];
                        a.total += e.total;
                        a.shared += e.shared;
                        a.compressed += e.compressed;
                    }
                }
            }
            const size_t d = done.fetch_add(1) + 1;
            if ((d & 1023) == 0)
                job.setProgress(double(d) / double(nf));
        }
        std::lock_guard l(m);
        for (size_t i = 0; i < rows.size(); ++i) {
            acc[i].total += mine[i].total;
            acc[i].shared += mine[i].shared;
            acc[i].compressed += mine[i].compressed;
        }
        all.total += myAll.total;
        all.shared += myAll.shared;
        all.compressed += myAll.compressed;
    };
    job.setStatus(TF("Reading the extents of %1 files…", {fmtInt(nf)}));
    job.setProgress(0);
    std::vector<std::thread> ts;
    for (int i = 0; i < 8; ++i)
        ts.emplace_back(work);
    for (auto &th : ts)
        th.join();
    if (job.cancelled())
        return t;
    for (size_t i = 0; i < rows.size(); ++i) {
        const Acc &a = acc[i];
        if (a.total == 0)
            continue;
        ResultRow r;
        r.node = rows[i];
        r.path = rows[i]->path();
        r.bytes = a.total;
        r.deletable = false;
        auto k = kind.find(rows[i]);
        r.cells = {r.path.size() > base.size() ? r.path.substr(base.size() + (base == "/" ? 0 : 1)) : r.path,
                   k != kind.end() ? k->second : std::string(T("Folder")), fmtSize(a.total), fmtSize(a.shared),
                   fmtSize(a.total - a.shared), fmtSize(a.compressed)};
        t.rows.push_back(std::move(r));
    }
    std::stable_sort(t.rows.begin(), t.rows.end(), [](const ResultRow &a, const ResultRow &b) { return a.bytes > b.bytes; });
    std::string sum = TF("%1 of %2 is shared with snapshots or reflinked copies; deleting those files frees only the "
                         "exclusive part (%3).",
                         {fmtSize(all.shared), fmtSize(all.total), fmtSize(all.total - all.shared)});
    if (all.compressed)
        sum += "\n" + TF("%1 is stored compressed (size before compression).", {fmtSize(all.compressed)});
    if (skipped)
        sum += "\n" + TF("Files under %1 (%2 in total) were not analyzed.", {fmtSize(MIN_FILE), fmtSize(skipped)});
    if (::geteuid() == 0 && haveProgram("compsize")) {
        std::string out;
        if (runC({"compsize", "-x", base}, &out, 600000) == 0)
            for (std::string_view l : lines(out))
                if (l.rfind("TOTAL", 0) == 0)
                    sum += "\ncompsize: " + trim(l);
    } else {
        sum += "\n" + (haveProgram("compsize") ? TF("For the compressed size on disk run: sudo compsize -x %1", {base})
                                               : std::string(T("Install compsize to see the compressed size on disk.")));
    }
    t.summary = sum;
    return t;
}
