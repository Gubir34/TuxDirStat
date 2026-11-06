#include "tools.h"

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <ostream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <fcntl.h>
#include <fnmatch.h>
#include <ftw.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include "i18n.h"
#include "session.h"

#ifndef TUXDIRSTAT_VERSION
#define TUXDIRSTAT_VERSION "dev"
#endif

// ───────────────────────────── ortak ─────────────────────────────

void realChildren(const Node *n, std::vector<Node *> &out)
{
    out.clear();
    std::vector<const Node *> st{n};
    while (!st.empty()) {
        const Node *d = st.back();
        st.pop_back();
        for (Node *c : d->children) {
            if (c->flags & F_GROUP)
                st.push_back(c);
            else
                out.push_back(c);
        }
    }
}

namespace {

// Ağacın tüm gerçek düğümlerini (gruplar atlanır) ziyaret eder; f false dönerse
// düğümün içine inilmez.
template <class F>
void walkReal(Node *root, F f)
{
    std::vector<Node *> st{root};
    while (!st.empty()) {
        Node *d = st.back();
        st.pop_back();
        for (Node *c : d->children) {
            if (c->flags & F_GROUP) {
                st.push_back(c);
                continue;
            }
            if (f(c) && c->isDir)
                st.push_back(c);
        }
    }
}

std::string lower(std::string_view s)
{
    std::string r(s);
    for (char &c : r)
        if (c >= 'A' && c <= 'Z')
            c = char(c + 32);
    return r;
}

size_t displayWidth(const std::string &s)
{
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80)
            ++n;
    return n;
}

std::string fmtDelta(int64_t d)
{
    if (d == 0)
        return "0";
    return (d > 0 ? "+" : "−") + fmtSize(uint64_t(d > 0 ? d : -d));
}

// strtod C yereline bağlıdır; from_chars değildir. Ondalık ayırıcı nokta ya da virgül olabilir.
bool parseNumber(std::string_view s, double &v, std::string_view &rest)
{
    std::string num(s);
    for (char &c : num)
        if (c == ',')
            c = '.';
    auto r = std::from_chars(num.data(), num.data() + num.size(), v, std::chars_format::fixed);
    if (r.ec != std::errc() || v < 0)
        return false;
    rest = s.substr(size_t(r.ptr - num.data()));
    return true;
}

bool parseSize(std::string_view s, uint64_t &out)
{
    double v;
    std::string_view end;
    if (s.empty() || !parseNumber(s, v, end))
        return false;
    std::string unit = lower(end);
    if (!unit.empty() && unit.back() == 'b')
        unit.pop_back();
    if (unit.size() > 1 && unit.back() == 'i')
        unit.pop_back();
    double mul = 1;
    if (unit.empty())
        mul = 1;
    else if (unit == "k")
        mul = 1024.0;
    else if (unit == "m")
        mul = 1024.0 * 1024;
    else if (unit == "g")
        mul = 1024.0 * 1024 * 1024;
    else if (unit == "t")
        mul = 1024.0 * 1024 * 1024 * 1024;
    else
        return false;
    out = uint64_t(v * mul);
    return true;
}

bool parseAge(std::string_view s, int64_t &out)
{
    double v;
    std::string_view end;
    if (s.empty() || !parseNumber(s, v, end))
        return false;
    const std::string unit = lower(end);
    double mul;
    if (unit == "h")
        mul = 3600;
    else if (unit.empty() || unit == "d")
        mul = 86400;
    else if (unit == "w")
        mul = 7 * 86400;
    else if (unit == "m")
        mul = 30 * 86400;
    else if (unit == "y")
        mul = 365 * 86400;
    else
        return false;
    out = int64_t(v * mul);
    return true;
}

ResultRow nodeRow(Node *n, bool apparent)
{
    ResultRow r;
    r.path = n->path();
    r.node = n;
    r.bytes = n->sizeOf(apparent);
    r.cells = {r.path, fmtSize(r.bytes), fmtTime(n->mtime)};
    return r;
}

}  // namespace

std::string deletionWarning(const ResultTable &t, const std::vector<int> &rows)
{
    if (!t.grouped)
        return {};
    std::unordered_map<int, int> selected, total;
    for (const ResultRow &r : t.rows)
        ++total[r.group];
    for (int i : rows)
        if (i >= 0 && size_t(i) < t.rows.size())
            ++selected[t.rows[size_t(i)].group];
    for (int i : rows) {
        const ResultRow &r = t.rows[size_t(i)];
        if (selected[r.group] >= total[r.group])
            return TF("You selected every copy of %1. Keep at least one of them.", {r.path});
    }
    return {};
}

void printTable(std::ostream &out, const ResultTable &t)
{
    out << t.title << "\n";
    if (!t.summary.empty())
        out << t.summary << "\n";
    out << "\n";
    if (t.rows.empty()) {
        out << "  " << t.emptyText << "\n";
        return;
    }
    const size_t nc = t.columns.size();
    std::vector<size_t> w(nc, 0);
    for (size_t c = 0; c < nc; ++c)
        w[c] = displayWidth(t.columns[c]);
    for (const ResultRow &r : t.rows)
        for (size_t c = 0; c < nc && c < r.cells.size(); ++c)
            w[c] = std::max(w[c], displayWidth(r.cells[c]));
    // Son sütun (çoğunlukla yol ya da komut) doldurulmaz.
    auto line = [&](const std::vector<std::string> &cells) {
        out << " ";
        for (size_t c = 0; c < nc; ++c) {
            const std::string s = c < cells.size() ? cells[c] : std::string();
            const size_t pad = w[c] - std::min(w[c], displayWidth(s));
            out << " ";
            if (c < t.numeric.size() && t.numeric[c])
                out << std::string(pad, ' ') << s;
            else if (c + 1 < nc)
                out << s << std::string(pad, ' ');
            else
                out << s;
            if (c + 1 < nc)
                out << " ";
        }
        out << "\n";
    };
    line(t.columns);
    int lastGroup = t.rows.front().group;
    for (const ResultRow &r : t.rows) {
        if (t.grouped && r.group != lastGroup)
            out << "\n";
        lastGroup = r.group;
        line(r.cells);
    }
}

// ───────────────────────────── iş ─────────────────────────────

Job::Job(std::string t, Fn fn) : title(std::move(t)), fn_(std::move(fn)) {}

Job::~Job()
{
    cancel();
    if (thread_.joinable())
        thread_.join();
}

void Job::start()
{
    thread_ = std::thread([this] {
        try {
            fn_(*this);
        } catch (const std::exception &e) {
            error = e.what();
        }
        done_ = true;
    });
}

void Job::setStatus(std::string s)
{
    std::lock_guard l(m_);
    status_ = std::move(s);
}

std::string Job::status() const
{
    std::lock_guard l(m_);
    return status_;
}

// ───────────────────────────── arama ─────────────────────────────

const char *searchHelp()
{
    return T("Words match file names (* and ? are wildcards). Filters:\n"
             "  >100M  <1G        size (K, M, G, T)\n"
             "  older:1y  newer:2w   last change (h, d, w, m, y)\n"
             "  type:f  type:d     files or folders only\n"
             "  ext:mp4,mkv        extensions\n"
             "  user:name          owner\n"
             "  path:Steam         text anywhere in the path\n"
             "Example: *.log >50M older:6m");
}

ResultTable searchTree(Node *root, const std::string &query, bool apparent, std::string *err, size_t limit)
{
    ResultTable t;
    t.title = T("Find files");
    t.columns = {T("Path"), T("Size"), T("Modified")};
    t.numeric = {false, true, false};
    t.emptyText = T("Nothing matches the search.");

    std::vector<std::string> tokens;
    {
        std::string cur;
        bool quoted = false, any = false;
        for (char c : query) {
            if (c == '"') {
                quoted = !quoted;
                any = true;
            } else if (!quoted && (c == ' ' || c == '\t')) {
                if (any)
                    tokens.push_back(cur);
                cur.clear();
                any = false;
            } else {
                cur += c;
                any = true;
            }
        }
        if (any)
            tokens.push_back(cur);
    }

    std::vector<std::string> names, paths;
    std::unordered_set<uint32_t> exts;
    bool anyExt = false;
    uint64_t minSize = 0, maxSize = UINT64_MAX;
    int64_t olderThan = -1, newerThan = -1;
    int type = 0;  // 1 dosya, 2 klasör
    bool anyUser = false;
    uint32_t uid = 0;
    auto fail = [&](const std::string &tok) {
        if (err)
            *err = TF("Could not understand \"%1\".", {tok});
        return t;
    };
    for (const std::string &tok : tokens) {
        std::string_view v = tok;
        const size_t colon = v.find(':');
        const std::string key = colon == std::string_view::npos ? std::string() : lower(v.substr(0, colon));
        const std::string_view val = colon == std::string_view::npos ? std::string_view() : v.substr(colon + 1);
        if (v.rfind("size", 0) == 0 && v.size() > 4 && (v[4] == '>' || v[4] == '<'))
            v.remove_prefix(4);
        if (v[0] == '>' || v[0] == '<') {
            const bool gt = v[0] == '>';
            v.remove_prefix(1);
            if (!v.empty() && v[0] == '=')
                v.remove_prefix(1);
            uint64_t sz;
            if (!parseSize(v, sz))
                return fail(tok);
            if (gt)
                minSize = std::max(minSize, sz);
            else
                maxSize = std::min(maxSize, sz);
        } else if (key == "older" || key == "newer") {
            int64_t age;
            if (!parseAge(val, age))
                return fail(tok);
            (key == "older" ? olderThan : newerThan) = age;
        } else if (key == "type") {
            const std::string tv = lower(val);
            if (tv == "f" || tv == "file" || tv == "files")
                type = 1;
            else if (tv == "d" || tv == "dir" || tv == "folder" || tv == "folders")
                type = 2;
            else
                return fail(tok);
        } else if (key == "ext") {
            anyExt = true;
            std::string e;
            for (char c : std::string(val) + ",") {
                if (c == ',') {
                    if (!e.empty() && e[0] == '.')
                        e.erase(0, 1);
                    exts.insert(e.empty() ? 0 : ExtTable::intern(lower(e)));
                    e.clear();
                } else {
                    e += c;
                }
            }
        } else if (key == "user") {
            const std::string u(val);
            if (const passwd *pw = getpwnam(u.c_str()))
                uid = pw->pw_uid;
            else if (!u.empty() && std::all_of(u.begin(), u.end(), ::isdigit))
                uid = uint32_t(std::stoul(u));
            else
                return fail(tok);
            anyUser = true;
        } else if (key == "path") {
            paths.push_back(lower(val));
        } else {
            names.push_back(tok);
        }
    }
    if (tokens.empty()) {
        if (err)
            *err = T("Type something to search for.");
        return t;
    }
    std::vector<std::pair<bool, std::string>> pats;  // (joker mi, desen)
    for (const std::string &n : names) {
        const bool glob = n.find_first_of("*?[") != std::string::npos;
        pats.emplace_back(glob, glob ? n : lower(n));
    }
    const int64_t now = std::time(nullptr);
    std::vector<Node *> hits;
    walkReal(root, [&](Node *n) {
        if ((type == 1 && n->isDir) || (type == 2 && !n->isDir))
            return true;
        const uint64_t sz = n->sizeOf(apparent);
        if (sz < minSize || sz > maxSize)
            return true;
        if (olderThan >= 0 && now - n->mtime < olderThan)
            return true;
        if (newerThan >= 0 && now - n->mtime > newerThan)
            return true;
        if (anyExt && (n->isDir || !exts.count(n->ext)))
            return true;
        if (anyUser && n->uid != uid)
            return true;
        if (!pats.empty()) {
            const std::string name(n->nameView());
            const std::string lname = lower(name);
            bool ok = false;
            for (const auto &[glob, p] : pats)
                if (glob ? fnmatch(p.c_str(), name.c_str(), FNM_CASEFOLD) == 0 : lname.find(p) != std::string::npos) {
                    ok = true;
                    break;
                }
            if (!ok)
                return true;
        }
        if (!paths.empty()) {
            const std::string lp = lower(n->path());
            for (const std::string &p : paths)
                if (lp.find(p) == std::string::npos)
                    return true;
        }
        hits.push_back(n);
        return true;
    });
    std::sort(hits.begin(), hits.end(), [apparent](const Node *a, const Node *b) {
        return a->sizeOf(apparent) > b->sizeOf(apparent);
    });
    uint64_t total = 0;
    for (const Node *n : hits)
        if (!n->isDir)
            total += n->sizeOf(apparent);
    const size_t shown = std::min(hits.size(), limit);
    for (size_t i = 0; i < shown; ++i)
        t.rows.push_back(nodeRow(hits[i], apparent));
    t.summary = TF("%1 matches", {fmtInt(hits.size())});
    if (type != 2)
        t.summary += " · " + TF("files total %1", {fmtSize(total)});
    if (shown < hits.size())
        t.summary += " · " + TF("showing the largest %1", {fmtInt(shown)});
    return t;
}

// ───────────────────────────── kopya dosyalar ─────────────────────────────

namespace {

// XXH64 (Yann Collet); iki farklı tohumla 128 bitlik özet üretmek için kullanılır.
class Xxh64 {
public:
    explicit Xxh64(uint64_t seed) : seed_(seed)
    {
        v_[0] = seed + P1 + P2;
        v_[1] = seed + P2;
        v_[2] = seed;
        v_[3] = seed - P1;
    }

    void update(const unsigned char *p, size_t len)
    {
        total_ += len;
        if (memLen_ + len < 32) {
            std::memcpy(mem_ + memLen_, p, len);
            memLen_ += len;
            return;
        }
        if (memLen_) {
            const size_t fill = 32 - memLen_;
            std::memcpy(mem_ + memLen_, p, fill);
            block(mem_);
            p += fill;
            len -= fill;
            memLen_ = 0;
        }
        while (len >= 32) {
            block(p);
            p += 32;
            len -= 32;
        }
        std::memcpy(mem_, p, len);
        memLen_ = len;
    }

    uint64_t digest() const
    {
        uint64_t h;
        if (total_ >= 32) {
            h = rotl(v_[0], 1) + rotl(v_[1], 7) + rotl(v_[2], 12) + rotl(v_[3], 18);
            for (uint64_t v : v_)
                h = (h ^ round(0, v)) * P1 + P4;
        } else {
            h = seed_ + P5;
        }
        h += total_;
        const unsigned char *p = mem_;
        size_t len = memLen_;
        while (len >= 8) {
            h ^= round(0, read64(p));
            h = rotl(h, 27) * P1 + P4;
            p += 8;
            len -= 8;
        }
        if (len >= 4) {
            uint32_t k;
            std::memcpy(&k, p, 4);
            h ^= uint64_t(k) * P1;
            h = rotl(h, 23) * P2 + P3;
            p += 4;
            len -= 4;
        }
        while (len--) {
            h ^= (*p++) * P5;
            h = rotl(h, 11) * P1;
        }
        h ^= h >> 33;
        h *= P2;
        h ^= h >> 29;
        h *= P3;
        h ^= h >> 32;
        return h;
    }

private:
    static constexpr uint64_t P1 = 11400714785074694791ULL, P2 = 14029467366897019727ULL,
                              P3 = 1609587929392839161ULL, P4 = 9650029242287828579ULL, P5 = 2870177450012600261ULL;
    static uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
    static uint64_t round(uint64_t acc, uint64_t in) { return rotl(acc + in * P2, 31) * P1; }
    static uint64_t read64(const unsigned char *p)
    {
        uint64_t v;
        std::memcpy(&v, p, 8);
        return v;
    }
    void block(const unsigned char *p)
    {
        for (int i = 0; i < 4; ++i)
            v_[i] = round(v_[i], read64(p + 8 * i));
    }

    uint64_t seed_, v_[4], total_ = 0;
    unsigned char mem_[32];
    size_t memLen_ = 0;
};

struct Digest {
    uint64_t a = 0, b = 0;
    bool operator==(const Digest &o) const { return a == o.a && b == o.b; }
};
struct DigestHash {
    size_t operator()(const Digest &d) const { return size_t(d.a ^ (d.b * 0x9E3779B97F4A7C15ULL)); }
};

// Dosyanın ilk `limit` baytının (0 = tamamı) özeti. Okunamazsa false.
bool hashFile(const std::string &path, uint64_t limit, Digest &out, dev_t &dev, ino_t &ino, Job &job,
              std::atomic<uint64_t> &done)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return false;
    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return false;
    }
    dev = st.st_dev;
    ino = st.st_ino;
    ::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
    Xxh64 h1(0), h2(0x7F4A7C159E3779B9ULL);
    static thread_local std::vector<unsigned char> buf(1 << 20);
    uint64_t left = limit ? limit : UINT64_MAX;
    while (left > 0) {
        if (job.cancelled()) {
            ::close(fd);
            return false;
        }
        const ssize_t n = ::read(fd, buf.data(), size_t(std::min<uint64_t>(buf.size(), left)));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            ::close(fd);
            return false;
        }
        if (n == 0)
            break;
        h1.update(buf.data(), size_t(n));
        h2.update(buf.data(), size_t(n));
        left -= uint64_t(n);
        done += uint64_t(n);
    }
    ::close(fd);
    out = {h1.digest(), h2.digest()};
    return true;
}

}  // namespace

ResultTable findDuplicates(Node *root, Job &job, uint64_t minSize)
{
    ResultTable t;
    t.title = T("Duplicate files");
    t.columns = {T("Path"), T("Size"), T("Modified")};
    t.numeric = {false, true, false};
    t.grouped = true;
    t.emptyText = T("No duplicate files found.");

    job.setStatus(T("Collecting files…"));
    std::unordered_map<uint64_t, std::vector<Node *>> bySize;
    walkReal(root, [&](Node *n) {
        if (!n->isDir && !(n->flags & F_LINK) && n->size >= minSize)
            bySize[n->size].push_back(n);
        return true;
    });
    struct Cand {
        Node *node;
        std::string path;
        Digest d;
    };
    std::vector<std::vector<Cand>> groups;
    uint64_t toRead = 0, files = 0;
    constexpr uint64_t HEAD = 64 * 1024;
    for (auto &[sz, v] : bySize) {
        if (v.size() < 2)
            continue;
        std::vector<Cand> g;
        for (Node *n : v)
            g.push_back({n, {}, {}});
        toRead += uint64_t(v.size()) * std::min(sz, HEAD);
        files += v.size();
        groups.push_back(std::move(g));
    }
    bySize.clear();
    std::atomic<uint64_t> done{0};
    uint64_t hashed = 0;
    std::vector<std::vector<Cand>> result;
    auto progress = [&] {
        job.setProgress(toRead ? std::min(1.0, double(done) / double(toRead)) : -1);
        job.setStatus(TF("Comparing contents: %1 of %2 files", {fmtInt(hashed), fmtInt(files)}));
    };
    // Önce ilk 64 KB, eşleşenler için dosyanın tamamı karşılaştırılır.
    for (int pass = 0; pass < 2 && !job.cancelled(); ++pass) {
        if (pass == 1) {
            toRead = done;
            hashed = files = 0;
            for (auto &g : groups) {
                const uint64_t sz = g.front().node->size;
                if (sz > HEAD) {
                    toRead += uint64_t(g.size()) * sz;
                    files += g.size();
                }
            }
        }
        std::vector<std::vector<Cand>> next;
        for (auto &g : groups) {
            if (job.cancelled())
                break;
            const uint64_t sz = g.front().node->size;
            if (pass == 1 && sz <= HEAD) {
                next.push_back(std::move(g));  // ilk geçiş zaten tüm içeriği okudu
                continue;
            }
            std::unordered_map<Digest, std::vector<Cand>, DigestHash> byHash;
            std::unordered_set<uint64_t> inodes;
            for (Cand &c : g) {
                if (c.path.empty())
                    c.path = c.node->path();
                dev_t dev;
                ino_t ino;
                Digest d;
                ++hashed;
                if (!hashFile(c.path, pass == 0 ? HEAD : 0, d, dev, ino, job, done))
                    continue;
                // Sabit bağlar aynı veridir; kopya sayılmaz.
                if (!inodes.insert(uint64_t(ino) * 1000003u ^ uint64_t(dev)).second)
                    continue;
                c.d = d;
                byHash[d].push_back(std::move(c));
                if ((hashed & 63) == 0)
                    progress();
            }
            for (auto &[d, v] : byHash)
                if (v.size() >= 2)
                    next.push_back(std::move(v));
        }
        groups = std::move(next);
        progress();
    }
    if (job.cancelled())
        return t;
    result = std::move(groups);
    std::sort(result.begin(), result.end(), [](const std::vector<Cand> &a, const std::vector<Cand> &b) {
        return a.front().node->size * (a.size() - 1) > b.front().node->size * (b.size() - 1);
    });
    uint64_t wasted = 0, nfiles = 0;
    int gid = 0;
    for (auto &g : result) {
        ++gid;
        std::sort(g.begin(), g.end(), [](const Cand &a, const Cand &b) { return a.node->mtime < b.node->mtime; });
        wasted += g.front().node->size * (g.size() - 1);
        nfiles += g.size();
        for (Cand &c : g) {
            ResultRow r;
            r.path = c.path;
            r.node = c.node;
            r.bytes = c.node->size;
            r.group = gid;
            r.cells = {c.path, fmtSize(c.node->size), fmtTime(c.node->mtime)};
            t.rows.push_back(std::move(r));
        }
    }
    t.summary = TF("%1 sets · %2 files · keeping one copy of each would free %3",
                   {fmtInt(result.size()), fmtInt(nfiles), fmtSize(wasted)});
    return t;
}

// ───────────────────────────── temizlik önerileri ─────────────────────────────

namespace {

thread_local uint64_t duTotal;
thread_local const Job *duJob;

int duEntry(const char *, const struct stat *st, int type, struct FTW *)
{
    if (type == FTW_F || type == FTW_SL)
        duTotal += uint64_t(st->st_blocks) * 512;
    return duJob && duJob->cancelled() ? 1 : 0;
}

// Klasörün diskte kapladığı alan; okunamayan alt klasörler atlanır. Yoksa false.
bool dirSize(const std::string &path, uint64_t &out, const Job *job)
{
    struct stat st;
    if (::stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
        return false;
    duTotal = 0;
    duJob = job;
    ::nftw(path.c_str(), duEntry, 32, FTW_PHYS | FTW_MOUNT);
    out = duTotal;
    return true;
}

// "1.2G", "512.0M", "3,4 GB (50%)" gibi metinlerin başındaki boyutu okur.
bool leadingSize(std::string_view s, uint64_t &out)
{
    while (!s.empty() && s.front() == ' ')
        s.remove_prefix(1);
    size_t i = 0;
    while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.' || s[i] == ','))
        ++i;
    size_t j = i;
    while (j < s.size() && s[j] == ' ')
        ++j;
    size_t k = j;
    while (k < s.size() && std::isalpha(static_cast<unsigned char>(s[k])))
        ++k;
    std::string v(s.substr(0, i));
    std::string unit(s.substr(j, k - j));
    if (unit == "B" || unit == "kB")
        unit = unit == "kB" ? "K" : "";
    return !v.empty() && parseSize(v + unit, out);
}

}  // namespace

ResultTable findCleanup(Node *root, bool apparent, Job &job)
{
    ResultTable t;
    t.title = T("Cleanup suggestions");
    t.columns = {T("What"), T("Location"), T("Size"), T("How")};
    t.numeric = {false, false, true, false};
    t.emptyText = T("Nothing obvious to clean up.");

    const std::string home = homeDir();
    const std::string cacheDir = home + "/.cache";
    const std::string trashDir = home + "/.local/share/Trash";
    const std::string trashName = ".Trash-" + std::to_string(getuid());
    auto add = [&](const char *what, const std::string &where, uint64_t bytes, Node *node, std::string command) {
        ResultRow r;
        r.path = where;
        r.node = node;
        r.bytes = bytes;
        r.command = std::move(command);
        r.deletable = r.command.empty();
        r.cells = {T(what), where, bytes || node ? fmtSize(bytes) : std::string("?"),
                   r.command.empty() ? T("Move to trash") : r.command};
        t.rows.push_back(std::move(r));
    };
    // Çok küçük öğeler listeyi kalabalıklaştırır; 1 MB altı gösterilmez.
    constexpr uint64_t MIN_ROW = 1 << 20;
    auto addNode = [&](const char *what, Node *n) {
        if (n->sizeOf(apparent) >= MIN_ROW)
            add(what, n->path(), n->sizeOf(apparent), n, {});
    };
    // Gizli klasörlerin (~/.config/…, ~/.vscode/…) içindeki node_modules ve önbellekler
    // uygulamaların kendi parçasıdır; silinirse uygulama bozulabilir.
    auto insideHidden = [&](const Node *n) {
        for (const Node *p = n->parent; p && p != root; p = p->parent)
            if (!(p->flags & F_GROUP) && p->nameLen > 1 && p->name[0] == '.')
                return true;
        return false;
    };
    const std::string npmDir = home + "/.npm";

    job.setStatus(T("Looking through the scanned folders…"));
    std::vector<Node *> sib;
    constexpr uint64_t MIN_CACHE = 1 << 20;
    walkReal(root, [&](Node *n) {
        if (job.cancelled())
            return false;
        const std::string_view name = n->nameView();
        if (!n->isDir) {
            if (n->ext && n->sizeOf(apparent) > 0) {
                const std::string e = ExtTable::name(n->ext);
                if (e == "part" || e == "crdownload" || e == "partial")
                    addNode("Unfinished download", n);
            }
            return false;
        }
        if (n->flags & (F_ERROR | F_MOUNT))
            return false;
        if (name == "node_modules") {
            if (!insideHidden(n))
                addNode("Node.js packages (npm install brings them back)", n);
            return false;
        }
        if (name == "__pycache__") {
            if (!insideHidden(n))
                addNode("Python bytecode cache", n);
            return false;
        }
        if (name == ".pytest_cache" || name == ".mypy_cache" || name == ".ruff_cache") {
            addNode("Python tool cache", n);
            return false;
        }
        if (name == trashName) {
            addNode("Trash on this drive", n);
            return false;
        }
        if (name == "shadercache" && n->parent && realNode(n->parent)->nameView() == "steamapps") {
            addNode("Steam shader cache (rebuilt by Steam)", n);
            return false;
        }
        realChildren(n, sib);
        if (n != root && !insideHidden(n) &&
            std::any_of(sib.begin(), sib.end(), [](const Node *c) { return c->nameView() == "CMakeCache.txt"; })) {
            addNode("CMake build folder", n);
            return false;
        }
        if (name == "target" && n->parent) {
            std::vector<Node *> ps;
            realChildren(realNode(n->parent), ps);
            if (std::any_of(ps.begin(), ps.end(), [](const Node *c) { return c->nameView() == "Cargo.toml"; })) {
                addNode("Rust build output (cargo clean)", n);
                return false;
            }
        }
        const std::string p = n->path();
        if (p == cacheDir) {
            for (Node *c : sib)
                if (c->sizeOf(apparent) >= MIN_CACHE)
                    addNode("Application cache", c);
            return false;
        }
        if (p == npmDir) {
            for (Node *c : sib)
                if (c->isDir)
                    addNode("npm cache", c);
            return false;
        }
        if (p == trashDir) {
            add("Trash", p, n->sizeOf(apparent), n, "gio trash --empty");
            return false;
        }
        return true;
    });

    // Taranan ağaçtan bağımsız sistem önbellekleri
    uint64_t sz;
    job.setStatus(T("Checking package and system caches…"));
    if (!job.cancelled() && dirSize("/var/cache/pacman/pkg", sz, &job) && sz)
        add("Pacman package cache", "/var/cache/pacman/pkg", sz, nullptr,
            haveProgram("paccache") ? "sudo paccache -rk1" : "sudo pacman -Sc");
    if (!job.cancelled() && haveProgram("journalctl")) {
        std::string out;
        if (runCapture({"journalctl", "--disk-usage"}, &out, 5000) == 0) {
            const size_t at = out.find("take up ");
            if (at != std::string::npos && leadingSize(std::string_view(out).substr(at + 8), sz) && sz > (64u << 20))
                add("System journal", "/var/log/journal", sz, nullptr, "sudo journalctl --vacuum-size=200M");
        }
    }
    if (!job.cancelled() && dirSize("/var/lib/systemd/coredump", sz, &job) && sz)
        add("Crash dumps", "/var/lib/systemd/coredump", sz, nullptr,
            "sudo find /var/lib/systemd/coredump -type f -delete");
    for (const char *tool : {"docker", "podman"}) {
        if (job.cancelled() || !haveProgram(tool))
            continue;
        job.setStatus(TF("Asking %1 about unused images…", {tool}));
        std::string out;
        if (runCapture({tool, "system", "df", "--format", "{{.Reclaimable}}"}, &out, 8000) != 0)
            continue;
        uint64_t total = 0;
        size_t start = 0;
        while (start < out.size()) {
            size_t end = out.find('\n', start);
            if (end == std::string::npos)
                end = out.size();
            if (leadingSize(std::string_view(out).substr(start, end - start), sz))
                total += sz;
            start = end + 1;
        }
        if (total > (16u << 20))
            add(std::string(tool) == "docker" ? "Unused Docker data" : "Unused Podman data", tool, total, nullptr,
                std::string(tool) + " system prune");
    }
    if (!job.cancelled() && haveProgram("flatpak")) {
        uint64_t a = 0, b = 0;
        // Kurulu Flatpak'lerin ne kadarının kullanılmadığı bilinmez; boyut yerine "?" gösterilir.
        const bool sys = dirSize("/var/lib/flatpak", a, &job), usr = dirSize(home + "/.local/share/flatpak", b, &job);
        if ((sys || usr) && a + b > 0)
            add("Unused Flatpak runtimes", sys ? "/var/lib/flatpak" : home + "/.local/share/flatpak", 0, nullptr,
                "flatpak uninstall --unused");
    }
    if (job.cancelled())
        return t;
    std::stable_sort(t.rows.begin(), t.rows.end(), [](const ResultRow &a, const ResultRow &b) { return a.bytes > b.bytes; });
    uint64_t total = 0;
    for (const ResultRow &r : t.rows)
        total += r.bytes;
    t.summary = TF("%1 suggestions · up to %2 could be freed", {fmtInt(t.rows.size()), fmtSize(total)}) + "\n" +
                T("Review each one first. Rows with a command are run by you in a terminal.");
    return t;
}

// ───────────────────────────── sahipler ─────────────────────────────

ResultTable ownerUsage(Node *root, bool apparent)
{
    ResultTable t;
    t.title = T("Disk usage by owner");
    t.columns = {T("Owner"), T("Size"), T("Share"), T("Files")};
    t.numeric = {false, true, true, true};
    t.emptyText = T("No files.");
    struct Acc {
        uint64_t bytes = 0, files = 0;
    };
    std::unordered_map<uint32_t, Acc> m;
    walkReal(root, [&](Node *n) {
        if (!n->isDir) {
            Acc &a = m[n->uid];
            a.bytes += n->sizeOf(apparent);
            ++a.files;
        }
        return true;
    });
    std::vector<std::pair<uint32_t, Acc>> v(m.begin(), m.end());
    std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) { return a.second.bytes > b.second.bytes; });
    const double total = std::max<uint64_t>(1, root->sizeOf(apparent));
    for (const auto &[uid, a] : v) {
        ResultRow r;
        r.bytes = a.bytes;
        r.deletable = false;
        r.cells = {userName(uid) + " (" + std::to_string(uid) + ")", fmtSize(a.bytes), fmtPct(a.bytes / total), fmtInt(a.files)};
        t.rows.push_back(std::move(r));
    }
    t.summary = TF("%1 owners", {fmtInt(v.size())});
    return t;
}

// ───────────────────────────── pacman ─────────────────────────────

bool havePacman()
{
    return haveProgram("pacman") && ::access("/var/lib/pacman/local", R_OK) == 0;
}

std::string packageOwner(const std::string &path, std::string *err)
{
    std::string out;
    const int rc = runCapture({"pacman", "-Qqo", "--", path}, &out, 15000);
    if (rc == 0) {
        while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
            out.pop_back();
        const size_t nl = out.find('\n');
        return nl == std::string::npos ? out : out.substr(0, nl);
    }
    if (rc != 1 && err)
        *err = rc < 0 ? T("pacman could not be run.") : TF("pacman exited with code %1.", {std::to_string(rc)});
    return {};
}

std::string packageOwnerMessage(const std::string &path)
{
    std::string err;
    const std::string pkg = packageOwner(path, &err);
    if (!err.empty())
        return err;
    return pkg.empty() ? TF("%1\nis not owned by any package.", {path}) : TF("%1\nbelongs to the package %2.", {path, pkg});
}

namespace {

// pacman'in bilmediği ama sistemin kendisinin ürettiği dosyalar.
const char *const UNOWNED_IGNORE[] = {
    "/usr/lib/locale/locale-archive",
    "/usr/share/mime/*",
    "*/icon-theme.cache",
    "/usr/share/applications/mimeinfo.cache",
    "/usr/share/glib-2.0/schemas/gschemas.compiled",
    "/usr/lib/gdk-pixbuf-2.0/*/loaders.cache",
    "/usr/lib/gtk-*/*/immodules.cache",
    "/usr/lib/gio/modules/giomodule.cache",
    "/usr/share/info/dir",
    "/usr/lib/modules/*/modules.*",
    "*/fonts.dir",
    "*/fonts.scale",
    "*/.uuid",
    "/usr/lib/udev/hwdb.bin",
    "*/gconv-modules.cache",
    "/usr/share/texmf-dist/ls-R",
    "/usr/share/.mono",
    "/etc/udev/hwdb.bin",
    "/usr/lib/jvm/default",
    "/usr/lib/jvm/default-runtime",
    "/usr/.updated",
    "/etc/.updated",
    "/etc/.pwd.lock",
    "/etc/ld.so.cache",
    "/etc/ssl/certs/*",
    "/etc/ca-certificates/extracted/*",
    "/etc/pacman.d/gnupg",
    "/etc/machine-id",
    "/etc/adjtime",
    "/etc/localtime",
    "/etc/hostname",
    "/etc/locale.conf",
    "/etc/vconsole.conf",
    "/etc/*-",
    "/etc/systemd/system/*",
    "/etc/systemd/user/*",
};

bool ignoredUnowned(const std::string &p)
{
    for (const char *pat : UNOWNED_IGNORE)
        if (fnmatch(pat, p.c_str(), 0) == 0)
            return true;
    return false;
}

// Yolu `path` olan düğüm (gruplar içinden de); yoksa nullptr.
Node *findNode(Node *root, const std::string &path)
{
    const std::string rp = root->path();
    if (path == rp)
        return root;
    const std::string prefix = rp == "/" ? "/" : rp + "/";
    if (path.rfind(prefix, 0) != 0)
        return nullptr;
    Node *n = root;
    std::vector<Node *> kids;
    size_t pos = prefix.size();
    while (n && pos < path.size()) {
        size_t end = path.find('/', pos);
        if (end == std::string::npos)
            end = path.size();
        const std::string_view part(path.data() + pos, end - pos);
        realChildren(n, kids);
        Node *next = nullptr;
        for (Node *c : kids)
            if (c->nameView() == part) {
                next = c;
                break;
            }
        n = next;
        pos = end + 1;
    }
    return n;
}

}  // namespace

ResultTable findUnowned(Node *root, bool apparent, Job &job)
{
    ResultTable t;
    t.title = T("Files no package owns");
    t.columns = {T("Path"), T("Size"), T("Modified")};
    t.numeric = {false, true, false};

    // Taranan ağaçta /usr, /etc, /opt ya da bunların altındaki kök
    std::vector<Node *> starts;
    const std::string rp = root->path();
    for (const char *sys : {"/usr", "/etc", "/opt"}) {
        const std::string s = sys;
        if (rp == s || rp.rfind(s + "/", 0) == 0)
            starts.push_back(root);
        else if (Node *n = findNode(root, s))
            starts.push_back(n);
    }
    if (starts.empty()) {
        t.emptyText = T("Scan / (or /usr, /etc, /opt) to look for files that no package owns.");
        return t;
    }
    t.emptyText = T("Every scanned file under /usr, /etc and /opt belongs to a package.");
    job.setStatus(T("Reading package file lists…"));
    std::string out;
    if (runCapture({"pacman", "-Qlq"}, &out) != 0) {
        job.error = T("pacman -Ql failed.");
        return t;
    }
    std::unordered_set<std::string_view> owned;
    owned.reserve(out.size() / 48);
    size_t start = 0;
    while (start < out.size()) {
        size_t end = out.find('\n', start);
        if (end == std::string::npos)
            end = out.size();
        size_t e = end;
        while (e > start + 1 && out[e - 1] == '/')
            --e;
        owned.emplace(out.data() + start, e - start);
        start = end + 1;
    }
    job.setStatus(T("Comparing with the scanned files…"));
    std::vector<Node *> hits;
    std::vector<Node *> kids;
    struct Item {
        Node *n;
        std::string path;
    };
    std::vector<Item> st;
    for (Node *s : starts)
        st.push_back({s, s->path()});
    while (!st.empty() && !job.cancelled()) {
        Item it = std::move(st.back());
        st.pop_back();
        realChildren(it.n, kids);
        for (Node *c : kids) {
            std::string p = (it.path == "/" ? "" : it.path) + "/" + std::string(c->nameView());
            if (owned.count(p)) {
                if (c->isDir && !(c->flags & (F_ERROR | F_MOUNT)))
                    st.push_back({c, std::move(p)});
            } else if (!ignoredUnowned(p) && !(c->flags & F_MOUNT)) {
                hits.push_back(c);
            }
        }
    }
    if (job.cancelled())
        return t;
    std::sort(hits.begin(), hits.end(), [apparent](const Node *a, const Node *b) {
        return a->sizeOf(apparent) > b->sizeOf(apparent);
    });
    uint64_t total = 0;
    for (Node *n : hits) {
        total += n->sizeOf(apparent);
        if (t.rows.size() < 5000) {
            ResultRow r = nodeRow(n, apparent);
            r.deletable = false;  // sistem dosyaları: yol kopyalanıp elle silinir
            if (n->isDir)
                r.cells[0] += "/";
            t.rows.push_back(std::move(r));
        }
    }
    t.summary = TF("%1 items · %2", {fmtInt(hits.size()), fmtSize(total)}) + "\n" +
                T("Leftovers of removed packages, manual installs and generated files. Check before deleting (as root).");
    return t;
}

// ───────────────────────────── anlık görüntü ─────────────────────────────

namespace {

class JsonOut {
public:
    explicit JsonOut(std::FILE *f) : f_(f) {}
    void raw(std::string_view s) { std::fwrite(s.data(), 1, s.size(), f_); }
    void str(std::string_view s)
    {
        std::fputc('"', f_);
        for (unsigned char c : s) {
            if (c == '"' || c == '\\') {
                std::fputc('\\', f_);
                std::fputc(c, f_);
            } else if (c < 0x20) {
                std::fprintf(f_, "\\u%04x", c);
            } else {
                std::fputc(c, f_);
            }
        }
        std::fputc('"', f_);
    }
    void num(const char *key, uint64_t v) { std::fprintf(f_, ",\"%s\":%llu", key, static_cast<unsigned long long>(v)); }
    void snum(const char *key, int64_t v) { std::fprintf(f_, ",\"%s\":%lld", key, static_cast<long long>(v)); }

private:
    std::FILE *f_;
};

void writeInfo(JsonOut &o, const Node *n, std::string_view name)
{
    o.raw("{\"name\":");
    o.str(name);
    if (!n->isDir) {
        o.num("asize", n->size);
        o.num("dsize", n->alloc);
    }
    if (n->flags & F_ERROR)
        o.raw(",\"read_error\":true");
    if (n->flags & F_MOUNT)
        o.raw(",\"excluded\":\"otherfs\"");
    if (n->flags & F_LINK)
        o.raw(",\"notreg\":true");
    o.num("uid", n->uid);
    o.snum("mtime", n->mtime);
    o.raw("}");
}

}  // namespace

bool saveSnapshot(const Node *root, const std::string &file, std::string *err)
{
    std::FILE *f = openOutput(file, err);
    if (!f)
        return false;
    std::vector<char> buf(1 << 20);
    if (f != stdout)
        std::setvbuf(f, buf.data(), _IOFBF, buf.size());
    JsonOut o(f);
    std::fprintf(f, "[1,2,{\"progname\":\"tuxdirstat\",\"progver\":\"%s\",\"timestamp\":%lld},\n", TUXDIRSTAT_VERSION,
                 static_cast<long long>(std::time(nullptr)));
    // (düğüm, sıradaki çocuk) yığını; gruplar açılarak yazılır
    struct Frame {
        std::vector<Node *> kids;
        size_t next = 0;
    };
    std::vector<Frame> st;
    auto open = [&](const Node *d, std::string_view name) {
        o.raw("[");
        writeInfo(o, d, name);
        Frame fr;
        realChildren(d, fr.kids);
        st.push_back(std::move(fr));
    };
    open(root, root->path());
    while (!st.empty()) {
        Frame &fr = st.back();
        if (fr.next == fr.kids.size()) {
            o.raw("]");
            st.pop_back();
            continue;
        }
        const Node *c = fr.kids[fr.next++];
        o.raw(",\n");
        if (c->isDir && !(c->flags & F_MOUNT))
            open(c, c->nameView());
        else
            writeInfo(o, c, c->nameView());
    }
    o.raw("]\n");
    return closeOutput(f, err);
}

namespace {

class JsonIn {
public:
    JsonIn(const std::string &d) : p_(d.data()), b_(d.data()), e_(d.data() + d.size()) {}

    bool ok() const { return !bad_; }
    size_t offset() const { return size_t(p_ - b_); }
    void fail() { bad_ = true; }
    void ws()
    {
        while (p_ < e_ && (*p_ == ' ' || *p_ == '\n' || *p_ == '\r' || *p_ == '\t'))
            ++p_;
    }
    char peek()
    {
        ws();
        return p_ < e_ ? *p_ : 0;
    }
    bool eat(char c)
    {
        if (peek() != c) {
            bad_ = true;
            return false;
        }
        ++p_;
        return true;
    }
    bool tryEat(char c)
    {
        if (peek() != c)
            return false;
        ++p_;
        return true;
    }

    std::string str()
    {
        std::string s;
        if (!eat('"'))
            return s;
        while (p_ < e_ && *p_ != '"') {
            char c = *p_++;
            if (c != '\\') {
                s += c;
                continue;
            }
            if (p_ >= e_)
                break;
            c = *p_++;
            switch (c) {
            case 'n': s += '\n'; break;
            case 't': s += '\t'; break;
            case 'r': s += '\r'; break;
            case 'b': s += '\b'; break;
            case 'f': s += '\f'; break;
            case 'u': {
                uint32_t cp = hex4();
                if (cp >= 0xD800 && cp < 0xDC00 && p_ + 1 < e_ && p_[0] == '\\' && p_[1] == 'u') {
                    p_ += 2;
                    const uint32_t lo = hex4();
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(s, cp);
                break;
            }
            default: s += c;
            }
        }
        if (p_ >= e_)
            bad_ = true;
        else
            ++p_;
        return s;
    }

    int64_t num()
    {
        ws();
        char *end = nullptr;
        const long long v = std::strtoll(p_, &end, 10);
        if (end == p_) {
            bad_ = true;
            return 0;
        }
        p_ = end;
        // Kesirli ya da üslü sayılar (ncdu yazmaz) yine de atlanır.
        while (p_ < e_ && (*p_ == '.' || *p_ == 'e' || *p_ == 'E' || *p_ == '+' || *p_ == '-' || std::isdigit(static_cast<unsigned char>(*p_))))
            ++p_;
        return v;
    }

    bool boolean()
    {
        ws();
        if (e_ - p_ >= 4 && std::memcmp(p_, "true", 4) == 0) {
            p_ += 4;
            return true;
        }
        if (e_ - p_ >= 5 && std::memcmp(p_, "false", 5) == 0) {
            p_ += 5;
            return false;
        }
        bad_ = true;
        return false;
    }

    void skip()
    {
        const char c = peek();
        if (c == '"') {
            str();
        } else if (c == '{' || c == '[') {
            int depth = 0;
            while (p_ < e_ && !bad_) {
                const char d = *p_;
                if (d == '"') {
                    str();
                    continue;
                }
                ++p_;
                if (d == '{' || d == '[')
                    ++depth;
                else if ((d == '}' || d == ']') && --depth == 0)
                    return;
            }
            bad_ = true;
        } else if (c == 't' || c == 'f') {
            boolean();
        } else if (c == 'n') {
            if (e_ - p_ >= 4 && std::memcmp(p_, "null", 4) == 0)
                p_ += 4;
            else
                bad_ = true;
        } else {
            num();
        }
    }

private:
    uint32_t hex4()
    {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            if (p_ >= e_) {
                bad_ = true;
                return 0;
            }
            const char c = *p_++;
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f')
                v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v |= uint32_t(c - 'A' + 10);
            else
                bad_ = true;
        }
        return v;
    }
    static void utf8(std::string &s, uint32_t cp)
    {
        if (cp < 0x80) {
            s += char(cp);
        } else if (cp < 0x800) {
            s += char(0xC0 | (cp >> 6));
            s += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s += char(0xE0 | (cp >> 12));
            s += char(0x80 | ((cp >> 6) & 0x3F));
            s += char(0x80 | (cp & 0x3F));
        } else {
            s += char(0xF0 | (cp >> 18));
            s += char(0x80 | ((cp >> 12) & 0x3F));
            s += char(0x80 | ((cp >> 6) & 0x3F));
            s += char(0x80 | (cp & 0x3F));
        }
    }

    const char *p_, *b_, *e_;
    bool bad_ = false;
};

struct Info {
    std::string name;
    uint64_t asize = 0, dsize = 0;
    int64_t mtime = 0;
    uint32_t uid = 0;
    bool readError = false, excluded = false, notreg = false;
};

void readInfo(JsonIn &in, Info &info)
{
    info = Info{};
    in.eat('{');
    if (in.tryEat('}'))
        return;
    do {
        const std::string key = in.str();
        in.eat(':');
        if (key == "name")
            info.name = in.str();
        else if (key == "asize")
            info.asize = uint64_t(in.num());
        else if (key == "dsize")
            info.dsize = uint64_t(in.num());
        else if (key == "mtime")
            info.mtime = in.num();
        else if (key == "uid")
            info.uid = uint32_t(in.num());
        else if (key == "read_error")
            info.readError = in.boolean();
        else if (key == "notreg")
            info.notreg = in.boolean();
        else if (key == "excluded") {
            in.skip();
            info.excluded = true;
        } else
            in.skip();
    } while (in.ok() && in.tryEat(','));
    in.eat('}');
}

bool readFile(const std::string &file, std::string &data, std::string *err)
{
    auto endsWith = [&](const char *suf) {
        const size_t n = std::strlen(suf);
        return file.size() > n && file.compare(file.size() - n, n, suf) == 0;
    };
    const char *tool = endsWith(".gz") ? "gzip" : endsWith(".zst") ? "zstd" : endsWith(".xz") ? "xz" : endsWith(".bz2") ? "bzip2" : nullptr;
    if (tool) {
        if (::access(file.c_str(), R_OK) != 0) {
            if (err)
                *err = std::strerror(errno);
            return false;
        }
        const int rc = runCapture({tool, "-dc", "--", file}, &data);
        if (rc != 0) {
            if (err)
                *err = TF("Could not decompress the file with %1.", {tool});
            return false;
        }
        return true;
    }
    std::ifstream f(file, std::ios::binary);
    if (!f) {
        if (err)
            *err = std::strerror(errno);
        return false;
    }
    f.seekg(0, std::ios::end);
    data.resize(size_t(f.tellg()));
    f.seekg(0);
    f.read(data.data(), std::streamsize(data.size()));
    return true;
}

}  // namespace

std::unique_ptr<Tree> loadSnapshot(const std::string &file, std::string *err, int64_t *timestamp, const std::atomic<bool> *cancel)
{
    std::string data;
    if (!readFile(file, data, err))
        return nullptr;
    return parseSnapshot(data, err, timestamp, cancel);
}

std::unique_ptr<Tree> parseSnapshot(const std::string &data, std::string *err, int64_t *timestamp, const std::atomic<bool> *cancel)
{
    if (data.rfind("\xbf" "ncduEX1", 0) == 0) {
        if (err)
            *err = T("This is an ncdu binary export. Export with ncdu -o (JSON) instead.");
        return nullptr;
    }
    JsonIn in(data);
    auto bad = [&]() -> std::unique_ptr<Tree> {
        if (err)
            *err = TF("Not a valid ncdu / TuxDirStat snapshot (problem at byte %1).", {fmtInt(in.offset())});
        return nullptr;
    };
    in.eat('[');
    const int64_t major = in.num();
    in.eat(',');
    in.num();
    in.eat(',');
    if (!in.ok() || major != 1)
        return bad();
    // meta
    in.eat('{');
    if (!in.tryEat('}')) {
        do {
            const std::string key = in.str();
            in.eat(':');
            if (key == "timestamp") {
                const int64_t ts = in.num();
                if (timestamp)
                    *timestamp = ts;
            } else {
                in.skip();
            }
        } while (in.ok() && in.tryEat(','));
        in.eat('}');
    }
    in.eat(',');
    if (!in.ok() || in.peek() != '[')
        return bad();

    auto tree = std::make_unique<Tree>();
    tree->arenas.push_back(std::make_unique<Arena>());
    Arena &arena = *tree->arenas.front();
    struct Frame {
        Node *dir;
        std::vector<Node *> kids;
    };
    std::vector<Frame> st;
    Info info;
    uint64_t count = 0;
    auto openDir = [&]() {
        in.eat('[');
        readInfo(in, info);
        Node *parent = st.empty() ? nullptr : st.back().dir;
        const std::string_view nm = info.name.size() > 65535 ? std::string_view(info.name).substr(0, 65535) : info.name;
        Node *d = arena.node(parent, nm, true);
        d->mtime = info.mtime;
        d->uid = info.uid;
        if (info.readError)
            d->flags |= F_ERROR;
        if (parent)
            st.back().kids.push_back(d);
        else
            tree->root = d;
        st.push_back({d, {}});
    };
    openDir();
    while (in.ok() && !st.empty()) {
        if ((++count & 0xFFFF) == 0 && cancel && *cancel)
            return nullptr;
        if (in.tryEat(']')) {
            Frame &fr = st.back();
            fr.dir->children = arena.kids(fr.kids);
            updateDir(fr.dir);
            st.pop_back();
            continue;
        }
        if (!in.eat(','))
            break;
        const char c = in.peek();
        if (c == '[') {
            openDir();
            continue;
        }
        if (c != '{')
            return bad();
        readInfo(in, info);
        if (info.name.empty() || info.name.size() > 65535)
            return bad();
        Node *parent = st.back().dir;
        Node *n;
        if (info.excluded) {
            n = arena.node(parent, info.name, true);
            n->flags |= F_MOUNT;
        } else {
            n = arena.node(parent, info.name, false);
            n->size = info.asize;
            n->alloc = info.dsize;
            n->ext = ExtTable::forName(info.name);
            if (info.notreg)
                n->flags |= F_LINK;
        }
        if (info.readError)
            n->flags |= F_ERROR;
        n->mtime = info.mtime;
        n->uid = info.uid;
        st.back().kids.push_back(n);
    }
    if (!in.ok() || !st.empty() || !tree->root)
        return bad();
    return tree;
}

ResultTable compareTrees(Node *root, Node *old, int64_t oldTime, bool apparent, size_t limit)
{
    ResultTable t;
    t.title = T("Changes since the snapshot");
    t.columns = {T("Path"), T("Before"), T("Now"), T("Change")};
    t.numeric = {false, true, true, true};
    t.emptyText = T("Nothing changed.");
    struct Entry {
        std::string path;
        uint64_t before, after;
        Node *node;
        bool isDir, added, removed;
    };
    std::vector<Entry> entries;
    std::vector<Node *> kids;
    struct Item {
        Node *cur, *old;
        std::string path;
    };
    std::vector<Item> st{{root, old, root->path()}};
    while (!st.empty()) {
        Item it = std::move(st.back());
        st.pop_back();
        std::unordered_map<std::string_view, Node *> prev;
        for (Node *o : it.old->children)
            prev.emplace(o->nameView(), o);
        realChildren(it.cur, kids);
        const std::string base = it.path == "/" ? "" : it.path;
        for (Node *c : kids) {
            auto f = prev.find(c->nameView());
            Node *o = f == prev.end() ? nullptr : f->second;
            if (o)
                prev.erase(f);
            const uint64_t b = o ? o->sizeOf(apparent) : 0, a = c->sizeOf(apparent);
            std::string p = base + "/" + std::string(c->nameView());
            if (a != b || !o)
                entries.push_back({p, b, a, c, c->isDir, !o, false});
            if (o && c->isDir && o->isDir && a != b)
                st.push_back({c, o, std::move(p)});
        }
        for (auto &[name, o] : prev)
            entries.push_back({base + "/" + std::string(name), o->sizeOf(apparent), 0, nullptr, o->isDir, false, true});
    }
    auto mag = [](const Entry &e) { return e.after > e.before ? e.after - e.before : e.before - e.after; };
    std::sort(entries.begin(), entries.end(), [&](const Entry &a, const Entry &b) { return mag(a) > mag(b); });
    for (size_t i = 0; i < entries.size() && i < limit; ++i) {
        const Entry &e = entries[i];
        if (mag(e) == 0 && !e.added)
            continue;
        ResultRow r;
        r.path = e.path;
        r.node = e.node;
        r.bytes = mag(e);
        r.deletable = e.node != nullptr;
        r.cells = {e.path + (e.isDir ? "/" : ""), e.added ? std::string("—") : fmtSize(e.before),
                   e.removed ? std::string("—") : fmtSize(e.after),
                   e.added ? std::string(T("new")) + " (" + fmtDelta(int64_t(e.after)) + ")"
                   : e.removed ? std::string(T("deleted")) + " (" + fmtDelta(-int64_t(e.before)) + ")"
                               : fmtDelta(int64_t(e.after) - int64_t(e.before))};
        t.rows.push_back(std::move(r));
    }
    const uint64_t b = old->sizeOf(apparent), a = root->sizeOf(apparent);
    t.summary = TF("Total: %1 → %2 (%3)", {fmtSize(b), fmtSize(a), fmtDelta(int64_t(a) - int64_t(b))});
    if (oldTime)
        t.summary += " · " + TF("snapshot from %1", {fmtTime(oldTime)});
    if (std::string_view(old->nameView()) != root->path())
        t.summary += "\n" + TF("Note: the snapshot is of %1.", {std::string(old->nameView())});
    return t;
}

ResultTable compareWithSnapshot(Node *root, const std::string &file, bool apparent, Job &job)
{
    job.setStatus(T("Loading the snapshot…"));
    int64_t ts = 0;
    std::unique_ptr<Tree> old = loadSnapshot(file, &job.error, &ts, job.cancelFlag());
    if (!old || job.cancelled())
        return {};
    job.setStatus(T("Comparing…"));
    return compareTrees(root, old->root, ts, apparent);
}

// ───────────────────────────── CSV ─────────────────────────────

bool exportCsv(const Node *root, const std::string &file, std::string *err)
{
    std::FILE *f = openOutput(file, err);
    if (!f)
        return false;
    std::vector<char> buf(1 << 20);
    if (f != stdout)
        std::setvbuf(f, buf.data(), _IOFBF, buf.size());
    auto field = [f](const std::string &s) {
        if (s.find_first_of(",\"\n\r") == std::string::npos) {
            std::fwrite(s.data(), 1, s.size(), f);
            return;
        }
        std::fputc('"', f);
        for (char c : s) {
            if (c == '"')
                std::fputc('"', f);
            std::fputc(c, f);
        }
        std::fputc('"', f);
    };
    std::fputs("path,type,size,disk_size,files,folders,modified,owner\n", f);
    struct Item {
        const Node *n;
        std::string path;
    };
    std::vector<Item> st{{root, root->path()}};
    std::vector<Node *> kids;
    while (!st.empty()) {
        Item it = std::move(st.back());
        st.pop_back();
        const Node *n = it.n;
        const char *type = (n->flags & F_MOUNT) ? "mount" : (n->flags & F_ERROR) ? "error" : n->isDir ? "dir" : (n->flags & F_LINK) ? "link" : "file";
        char when[32] = "";
        if (n->mtime) {
            time_t tt = time_t(n->mtime);
            struct tm tm;
            localtime_r(&tt, &tm);
            std::strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S", &tm);
        }
        field(it.path);
        std::fprintf(f, ",%s,%llu,%llu,%u,%u,%s,", type, static_cast<unsigned long long>(n->size),
                     static_cast<unsigned long long>(n->alloc), n->isDir ? n->files : 0u, n->isDir ? n->dirs : 0u, when);
        field(userName(n->uid));
        std::fputc('\n', f);
        if (n->isDir) {
            realChildren(n, kids);
            const std::string base = it.path == "/" ? "" : it.path;
            for (size_t i = kids.size(); i-- > 0;)
                st.push_back({kids[i], base + "/" + std::string(kids[i]->nameView())});
        }
    }
    return closeOutput(f, err);
}
