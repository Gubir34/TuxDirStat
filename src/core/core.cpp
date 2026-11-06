#include "core.h"

#include "i18n.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

#include <strings.h>
#include <pwd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>


// ───────────────────────────── Node ─────────────────────────────

std::string Node::path() const
{
    std::vector<const Node *> chain;
    for (const Node *n = this; n; n = n->parent)
        if (!(n->flags & F_GROUP))
            chain.push_back(n);
    std::string p(chain.back()->nameView());
    for (auto it = chain.rbegin() + 1; it != chain.rend(); ++it) {
        if (p.empty() || p.back() != '/')
            p += '/';
        p += (*it)->nameView();
    }
    return p;
}

int Node::depth() const
{
    int d = 0;
    for (const Node *n = parent; n; n = n->parent)
        ++d;
    return d;
}

bool Node::isWithin(const Node *other) const
{
    for (const Node *n = this; n; n = n->parent)
        if (n == other)
            return true;
    return false;
}

void updateDir(Node *n)
{
    uint64_t size = 0, alloc = 0;
    uint32_t files = 0, dirs = 0;
    int64_t mt = n->mtime;
    for (const Node *c : n->children) {
        size += c->size;
        alloc += c->alloc;
        if (c->isDir) {
            files += c->files;
            dirs += c->dirs + ((c->flags & F_GROUP) ? 0 : 1);
        } else {
            ++files;
        }
        mt = std::max(mt, c->mtime);
    }
    n->size = size;
    n->alloc = alloc;
    n->files = files;
    n->dirs = dirs;
    n->mtime = mt;
}

void sortKids(Kids &k, SortKey key, bool descending, bool apparent)
{
    auto less = [key, apparent](const Node *a, const Node *b) {
        switch (key) {
        case SortKey::Name:
            return strcasecmp(a->name, b->name) < 0;
        case SortKey::Items: {
            uint64_t ia = a->isDir ? uint64_t(a->files) + a->dirs : 0;
            uint64_t ib = b->isDir ? uint64_t(b->files) + b->dirs : 0;
            return ia < ib;
        }
        case SortKey::Files:
            return a->files < b->files;
        case SortKey::Dirs:
            return a->dirs < b->dirs;
        case SortKey::MTime:
            return a->mtime < b->mtime;
        case SortKey::Size:
            break;
        }
        return a->sizeOf(apparent) < b->sizeOf(apparent);
    };
    if (descending)
        std::stable_sort(k.begin(), k.end(), [&](const Node *a, const Node *b) { return less(b, a); });
    else
        std::stable_sort(k.begin(), k.end(), less);
    for (uint32_t i = 0; i < k.n; ++i)
        k.ptr[i]->row = int32_t(i);
}

void detachNode(Node *n)
{
    Node *p = n->parent;
    if (!p)
        return;
    Kids &k = p->children;
    size_t i = size_t(n->row) < k.size() && k[n->row] == n ? size_t(n->row)
                                                           : size_t(std::find(k.begin(), k.end(), n) - k.begin());
    if (i == k.size())
        return;
    k.erase(i);
    for (size_t j = i; j < k.size(); ++j)
        k.ptr[j]->row = int32_t(j);
    n->parent = nullptr;
}

void takeChildren(Node *target, Node *fresh)
{
    target->children = fresh->children;
    fresh->children = Kids{};
    for (Node *c : target->children)
        c->parent = target;
    target->sgen = 0;
    target->flags = fresh->flags;
    target->size = fresh->size;
    target->alloc = fresh->alloc;
    target->files = fresh->files;
    target->dirs = fresh->dirs;
    target->mtime = fresh->mtime;
}

void propagateUp(Node *n)
{
    for (; n; n = n->parent)
        updateDir(n);
}

// ───────────────────────────── Gruplama ─────────────────────────────

// Bir klasörün öğesi, klasörün toplamının %1'inden küçükse "küçük" sayılır.
static bool isSmall(const Node *c, const Node *d)
{
    return (c->size + c->alloc) * 100 < d->size + d->alloc;
}

static Node *makeGroup(Arena &arena, Node *d, uint32_t kind, const std::string &name, const std::vector<Node *> &members)
{
    Node *g = arena.node(d, name, true);
    g->flags = F_GROUP;
    g->ext = kind;
    g->children = arena.kids(members);
    for (Node *m : members)
        m->parent = g;
    updateDir(g);
    return g;
}

void groupFiles(Node *root, Arena &arena, uint32_t minCount)
{
    minCount = std::max<uint32_t>(minCount, 2);
    std::vector<Node *> stack{root};
    std::unordered_map<uint32_t, std::vector<Node *>> byExt;
    std::vector<Node *> kids, loose, smallFiles, smallDirs, groups;
    while (!stack.empty()) {
        Node *d = stack.back();
        stack.pop_back();
        byExt.clear();
        smallDirs.clear();
        kids.clear();
        for (Node *c : d->children) {
            if (c->isDir) {
                stack.push_back(c);  // gruplansa da içi ayrıca gruplanır
                if (isSmall(c, d) && !(c->flags & F_MOUNT))
                    smallDirs.push_back(c);
                else
                    kids.push_back(c);
            } else {
                byExt[c->ext].push_back(c);
            }
        }
        groups.clear();
        loose.clear();
        for (auto &[e, files] : byExt) {
            if (files.size() >= minCount)
                groups.push_back(makeGroup(arena, d, e, e ? "*." + ExtTable::name(e) : std::string(T("no extension")), files));
            else
                loose.insert(loose.end(), files.begin(), files.end());
        }
        // Uzantıya göre toplanamayan küçük dosyalar
        smallFiles.clear();
        for (Node *f : loose)
            if (isSmall(f, d))
                smallFiles.push_back(f);
        if (smallFiles.size() >= minCount) {
            groups.push_back(makeGroup(arena, d, GROUP_SMALL, T("small files"), smallFiles));
            std::erase_if(loose, [](const Node *f) { return f->parent->flags & F_GROUP; });
        }
        if (smallDirs.size() >= minCount)
            groups.push_back(makeGroup(arena, d, GROUP_DIRS, T("small folders"), smallDirs));
        else
            kids.insert(kids.end(), smallDirs.begin(), smallDirs.end());
        if (groups.empty())
            continue;
        kids.insert(kids.end(), loose.begin(), loose.end());
        kids.insert(kids.end(), groups.begin(), groups.end());
        d->children = arena.kids(kids);
        d->sgen = 0;
        updateDir(d);
    }
}

void ungroupFiles(Node *root, Arena &arena)
{
    std::vector<Node *> stack{root};
    std::vector<Node *> kids;
    while (!stack.empty()) {
        Node *d = stack.back();
        stack.pop_back();
        bool any = false;
        kids.clear();
        for (Node *c : d->children) {
            if (c->flags & F_GROUP) {
                any = true;
                for (Node *m : c->children) {
                    m->parent = d;
                    kids.push_back(m);
                    if (m->isDir)
                        stack.push_back(m);
                }
                c->children = Kids{};
                c->parent = nullptr;
            } else {
                kids.push_back(c);
                if (c->isDir)
                    stack.push_back(c);
            }
        }
        if (!any)
            continue;
        d->children = arena.kids(kids);
        d->sgen = 0;
        updateDir(d);
    }
}

Node *realNode(Node *n)
{
    while (n && (n->flags & F_GROUP))
        n = n->parent;
    return n;
}

std::vector<std::string> diskPaths(const Node *n)
{
    if (!(n->flags & F_GROUP))
        return {n->path()};
    std::vector<std::string> out;
    for (const Node *c : n->children)
        out.push_back(c->path());
    return out;
}

std::string displayName(const Node *n, bool isRoot)
{
    std::string s = isRoot ? n->path() : std::string(n->nameView());
    if (n->flags & F_GROUP)
        s += "  (" + (n->ext == GROUP_DIRS ? TF("%1 folders", {fmtInt(n->children.size())}) : TF("%1 files", {fmtInt(n->files)})) + ")";
    else if (n->flags & F_ERROR)
        s += std::string("  [") + T("no access") + "]";
    else if (n->flags & F_MOUNT)
        s += std::string("  [") + T("other file system") + "]";
    return s;
}

// ───────────────────────────── Arena ─────────────────────────────

void *Arena::alloc(size_t n, size_t align)
{
    constexpr size_t BLOCK = 1 << 20;
    auto aligned = [align](char *p) {
        return reinterpret_cast<char *>((reinterpret_cast<uintptr_t>(p) + align - 1) & ~uintptr_t(align - 1));
    };
    char *p = cur_ ? aligned(cur_) : nullptr;
    if (!p || p + n > end_) {
        const size_t sz = std::max(BLOCK, n + align);
        blocks_.emplace_back(new char[sz]);
        cur_ = blocks_.back().get();
        end_ = cur_ + sz;
        p = aligned(cur_);
    }
    cur_ = p + n;
    return p;
}

const char *Arena::str(std::string_view s)
{
    char *p = static_cast<char *>(alloc(s.size() + 1, 1));
    std::memcpy(p, s.data(), s.size());
    p[s.size()] = 0;
    return p;
}

Node *Arena::node(Node *parent, std::string_view name, bool dir)
{
    const char *nm = str(name);
    return new (alloc(sizeof(Node), alignof(Node))) Node(parent, nm, uint16_t(name.size()), dir);
}

Kids Arena::kids(const std::vector<Node *> &v)
{
    Kids k;
    if (v.empty())
        return k;
    k.ptr = static_cast<Node **>(alloc(v.size() * sizeof(Node *), alignof(Node *)));
    std::memcpy(k.ptr, v.data(), v.size() * sizeof(Node *));
    k.n = uint32_t(v.size());
    return k;
}

void Tree::adopt(std::unique_ptr<Tree> other)
{
    for (auto &a : other->arenas)
        arenas.push_back(std::move(a));
}

// ───────────────────────────── Uzantılar ─────────────────────────────

namespace {
std::mutex extMutex;
std::unordered_map<std::string, uint32_t> extMap{{"", 0}};
std::vector<std::string> extNames{""};
}

uint32_t ExtTable::intern(std::string_view e)
{
    std::lock_guard l(extMutex);
    std::string key(e);
    auto it = extMap.find(key);
    if (it != extMap.end())
        return it->second;
    uint32_t id = uint32_t(extNames.size());
    extNames.push_back(key);
    extMap.emplace(std::move(key), id);
    return id;
}

std::string ExtTable::name(uint32_t id)
{
    std::lock_guard l(extMutex);
    return id < extNames.size() ? extNames[id] : std::string();
}

uint32_t ExtTable::count()
{
    std::lock_guard l(extMutex);
    return uint32_t(extNames.size());
}

std::vector<ExtStat> computeExtStats(const Node *root)
{
    std::vector<ExtStat> stats(ExtTable::count());
    std::vector<const Node *> stack{root};
    while (!stack.empty()) {
        const Node *n = stack.back();
        stack.pop_back();
        for (const Node *c : n->children) {
            if (c->isDir) {
                stack.push_back(c);
            } else {
                ExtStat &s = stats[c->ext];
                s.size += c->size;
                s.alloc += c->alloc;
                ++s.count;
            }
        }
    }
    return stats;
}

// Dosya adının küçük harfli uzantısı; uzantı sayılmayacaksa boş.
static std::string extKey(std::string_view name)
{
    const size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == name.size() || name.size() - dot - 1 > 12)
        return {};
    std::string key(name.substr(dot + 1));
    for (char &c : key)
        if (c >= 'A' && c <= 'Z')
            c = char(c + 32);
    return key;
}

uint32_t ExtTable::forName(std::string_view fileName)
{
    const std::string key = extKey(fileName);
    return key.empty() ? 0 : intern(key);
}

static uint32_t extId(const char *name, std::unordered_map<std::string, uint32_t> &cache)
{
    std::string key = extKey(name);
    if (key.empty())
        return 0;
    auto it = cache.find(key);
    if (it != cache.end())
        return it->second;
    uint32_t id = ExtTable::intern(key);
    cache.emplace(std::move(key), id);
    return id;
}

// ───────────────────────────── Bağlama noktaları ─────────────────────────────

static std::string unescapeMount(const std::string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size()) {
            int v = 0;
            bool ok = true;
            for (int k = 1; k <= 3; ++k) {
                char c = s[i + k];
                if (c < '0' || c > '7') {
                    ok = false;
                    break;
                }
                v = v * 8 + (c - '0');
            }
            if (ok) {
                out += char(v);
                i += 3;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

std::vector<Mount> readMounts()
{
    std::vector<Mount> out;
    std::ifstream f("/proc/self/mounts");
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string src, mp, fs;
        if (ss >> src >> mp >> fs)
            out.push_back({unescapeMount(src), unescapeMount(mp), fs});
    }
    return out;
}

bool isVirtualFs(const std::string &fs)
{
    static const std::unordered_set<std::string> v{
        "proc", "sysfs", "devtmpfs", "devpts", "cgroup", "cgroup2", "securityfs", "debugfs",
        "tracefs", "pstore", "bpf", "configfs", "fusectl", "mqueue", "hugetlbfs", "autofs",
        "binfmt_misc", "efivarfs", "rpc_pipefs", "nsfs", "ramfs"};
    return v.count(fs) > 0;
}

// ───────────────────────────── Tarayıcı ─────────────────────────────

// İş parçacığına özel durum: tahsisler ve geçici tamponlar paylaşılmaz.
struct Scanner::Ctx {
    Arena *arena;
    std::unordered_map<std::string, uint32_t> extCache;
    std::vector<ExtStat> ext;
    std::vector<Node *> kids;
    std::vector<Item> subdirs;
};

Scanner::Scanner(std::string path, bool oneFs, int workers)
    : path_(std::move(path)), oneFs_(oneFs), nworkers_(std::max(1, workers))
{
    std::string best;
    for (const Mount &m : readMounts()) {
        mounts_[m.mp] = {m.src, m.fs};
        std::string prefix = m.mp == "/" ? "/" : m.mp + "/";
        if ((path_ == m.mp || path_.rfind(prefix, 0) == 0) && m.mp.size() >= best.size()) {
            best = m.mp;
            rootSrc_ = m.src;
        }
    }
}

Scanner::~Scanner()
{
    cancel();
    if (thread_.joinable())
        thread_.join();
}

void Scanner::start()
{
    thread_ = std::thread(&Scanner::run, this);
}

std::unique_ptr<Tree> Scanner::takeTree()
{
    return std::move(tree_);
}

std::string Scanner::currentPath()
{
    std::lock_guard l(curM_);
    return current_;
}

// Bir klasörün kendi taraması ya da bir alt klasörü bittiğinde çağrılır.
// Son biten, klasörün toplamlarını hesaplar ve üst klasöre haber verir.
void Scanner::finish(Node *n)
{
    while (n && n->pend.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        updateDir(n);
        n = n->parent;
    }
}

void Scanner::run()
{
    auto t0 = std::chrono::steady_clock::now();
    struct stat st;
    if (::stat(path_.c_str(), &st) != 0) {
        error = std::strerror(errno);
    } else if (!S_ISDIR(st.st_mode)) {
        error = TF("Not a folder: %1", {path_});
    } else {
        tree_ = std::make_unique<Tree>();
        tree_->arenas.push_back(std::make_unique<Arena>());
        Node *root = tree_->arenas[0]->node(nullptr, path_, true);
        root->mtime = st.st_mtim.tv_sec;
        root->uid = st.st_uid;
        tree_->root = root;
        stack_.push_back({root, path_, st.st_dev});
        pending_ = 1;
        workerExt_.resize(nworkers_);
        std::vector<std::unique_ptr<Arena>> arenas(nworkers_);
        std::vector<std::thread> ts;
        for (int i = 0; i < nworkers_; ++i) {
            arenas[i] = std::make_unique<Arena>();
            ts.emplace_back(&Scanner::worker, this, i, arenas[i].get());
        }
        for (auto &t : ts)
            t.join();
        for (auto &a : arenas)
            tree_->arenas.push_back(std::move(a));
        if (cancelled_) {
            tree_.reset();
        } else {
            size_t n = ExtTable::count();
            for (const auto &we : workerExt_)
                n = std::max(n, we.size());
            extStats.assign(n, {});
            for (const auto &we : workerExt_)
                for (size_t e = 0; e < we.size(); ++e) {
                    extStats[e].size += we[e].size;
                    extStats[e].alloc += we[e].alloc;
                    extStats[e].count += we[e].count;
                }
        }
    }
    elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    done_ = true;
}

void Scanner::worker(int idx, Arena *arena)
{
    Ctx ctx{arena, {}, {}, {}, {}};
    for (;;) {
        Item it;
        {
            std::unique_lock l(qm_);
            cv_.wait(l, [this] { return !stack_.empty() || pending_ == 0; });
            if (stack_.empty())
                break;
            it = std::move(stack_.back());
            stack_.pop_back();
        }
        if (!cancelled_)
            scanDir(it, ctx);
        std::lock_guard l(qm_);
        if (--pending_ == 0)
            cv_.notify_all();
    }
    workerExt_[idx] = std::move(ctx.ext);
}

void Scanner::scanDir(Item &it, Ctx &ctx)
{
    Node *node = it.node;
    if (curM_.try_lock()) {
        current_ = it.path;
        curM_.unlock();
    }
    int fd = ::open(it.path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR *d = fd >= 0 ? ::fdopendir(fd) : nullptr;
    if (!d) {
        if (fd >= 0)
            ::close(fd);
        node->flags |= F_ERROR;
        node->pend.store(1, std::memory_order_relaxed);
        finish(node);
        return;
    }
    auto &kids = ctx.kids;
    auto &subdirs = ctx.subdirs;
    kids.clear();
    subdirs.clear();
    uint64_t nbytes = 0;
    const std::string base = it.path == "/" ? std::string() : it.path;
    while (dirent *de = ::readdir(d)) {
        const char *nm = de->d_name;
        if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0)))
            continue;
        struct stat st;
        if (::fstatat(fd, nm, &st, AT_SYMLINK_NOFOLLOW) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            Node *c = ctx.arena->node(node, nm, true);
            c->mtime = st.st_mtim.tv_sec;
            c->uid = st.st_uid;
            kids.push_back(c);
            std::string cp = base + "/" + nm;
            if (st.st_dev != it.dev) {
                auto m = mounts_.find(cp);
                if (m != mounts_.end() &&
                    (isVirtualFs(m->second.second) || (oneFs_ && m->second.first != rootSrc_))) {
                    c->flags |= F_MOUNT;
                    continue;
                }
            }
            subdirs.push_back({c, std::move(cp), st.st_dev});
        } else {
            Node *c = ctx.arena->node(node, nm, false);
            c->size = uint64_t(st.st_size);
            uint64_t alloc = uint64_t(st.st_blocks) * 512;
            if (st.st_nlink > 1) {
                std::lock_guard l(seenM_);
                if (!seen_.insert({st.st_dev, st.st_ino}).second)
                    alloc = 0;
            }
            c->alloc = alloc;
            c->mtime = st.st_mtim.tv_sec;
            c->uid = st.st_uid;
            c->ext = extId(nm, ctx.extCache);
            if (S_ISLNK(st.st_mode))
                c->flags |= F_LINK;
            if (c->ext >= ctx.ext.size())
                ctx.ext.resize(c->ext + 64);
            ExtStat &es = ctx.ext[c->ext];
            es.size += c->size;
            es.alloc += alloc;
            ++es.count;
            kids.push_back(c);
            nbytes += alloc;
        }
    }
    ::closedir(d);
    count += kids.size();
    bytes += nbytes;
    node->children = ctx.arena->kids(kids);
    // Alt klasörler kuyruğa girmeden sayaç kurulmalı; +1 bu klasörün kendisi için.
    node->pend.store(int32_t(subdirs.size()) + 1, std::memory_order_relaxed);
    if (!subdirs.empty()) {
        std::lock_guard l(qm_);
        pending_ += subdirs.size();
        for (Item &s : subdirs)
            stack_.push_back(std::move(s));
        cv_.notify_all();
    }
    finish(node);
}

// ───────────────────────────── Biçimlendirme ─────────────────────────────

// printf C yereline (LC_NUMERIC) bağlıdır; arayüzler setlocale() çağırdığı için to_chars kullanılır.
std::string fmtDecimal(double v, int digits)
{
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed, digits);
    std::string s(buf, r.ptr);
    for (char &c : s)
        if (c == '.')
            c = i18n::decimalPoint();
    return s;
}

std::string fmtSize(uint64_t b)
{
    if (b < 1024)
        return std::to_string(b) + " B";
    static const char *units[] = {"KB", "MB", "GB", "TB", "PB"};
    double v = double(b);
    for (int i = 0; i < 5; ++i) {
        v /= 1024;
        if (v < 1024 || i == 4)
            return fmtDecimal(v, 1) + ' ' + units[i];
    }
    return {};
}

std::string fmtInt(uint64_t n)
{
    std::string s = std::to_string(n);
    for (int i = int(s.size()) - 3; i > 0; i -= 3)
        s.insert(size_t(i), 1, i18n::thousandsSep());
    return s;
}

std::string fmtTime(int64_t t)
{
    if (!t)
        return {};
    time_t tt = time_t(t);
    struct tm tm;
    localtime_r(&tt, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, i18n::dateFormat(), &tm);
    return buf;
}

std::string fmtPct(double frac)
{
    return fmtDecimal(frac * 100, 1) + " %";
}

std::string userName(uint32_t uid)
{
    static std::mutex m;
    static std::unordered_map<uint32_t, std::string> cache;
    std::lock_guard l(m);
    auto it = cache.find(uid);
    if (it != cache.end())
        return it->second;
    std::string name = std::to_string(uid);
    struct passwd pw, *res = nullptr;
    char buf[4096];
    if (getpwuid_r(uid_t(uid), &pw, buf, sizeof buf, &res) == 0 && res)
        name = res->pw_name;
    cache.emplace(uid, name);
    return name;
}

// ───────────────────────────── Renkler ─────────────────────────────

RGBf normalizeColor(int r, int g, int b, float brightness)
{
    float c[3] = {r / 255.f, g / 255.f, b / 255.f};
    float s = c[0] + c[1] + c[2];
    if (s <= 0)
        s = 1;
    float f = 3 * brightness / s;
    for (float &x : c)
        x *= f * 255;
    for (int pass = 0; pass < 3; ++pass) {
        float over = 0;
        int under = 0;
        for (float &x : c) {
            if (x > 255) {
                over += x - 255;
                x = 255;
            } else if (x < 255) {
                ++under;
            }
        }
        if (over <= 0 || under == 0)
            break;
        for (float &x : c)
            if (x < 255)
                x += over / under;
    }
    return {std::min(255.f, c[0]), std::min(255.f, c[1]), std::min(255.f, c[2])};
}

const std::vector<RGBf> &palette()
{
    static const std::vector<RGBf> p = [] {
        const int base[][3] = {
            {40, 90, 255}, {235, 40, 40}, {40, 200, 60}, {40, 210, 220}, {220, 50, 220},
            {240, 220, 40}, {255, 140, 30}, {140, 120, 255}, {255, 130, 150}, {130, 240, 130},
            {120, 200, 255}, {200, 160, 90}, {230, 230, 230}};
        std::vector<RGBf> v;
        for (auto &c : base)
            v.push_back(normalizeColor(c[0], c[1], c[2]));
        return v;
    }();
    return p;
}

RGBf dirColor()
{
    static const RGBf c = normalizeColor(128, 128, 128);
    return c;
}

const std::vector<AgeBucket> &ageBuckets()
{
    constexpr int64_t DAY = 86400;
    static const std::vector<AgeBucket> b{
        {DAY, "today", normalizeColor(255, 40, 40)},
        {7 * DAY, "this week", normalizeColor(255, 140, 30)},
        {30 * DAY, "this month", normalizeColor(240, 220, 40)},
        {182 * DAY, "6 months", normalizeColor(40, 200, 60)},
        {365 * DAY, "1 year", normalizeColor(40, 210, 220)},
        {2 * 365 * DAY, "2 years", normalizeColor(40, 90, 255)},
        {5 * 365 * DAY, "5 years", normalizeColor(140, 120, 255)},
        {INT64_MAX, "older", normalizeColor(150, 150, 150)},
    };
    return b;
}

RGBf ageColor(int64_t mtime, int64_t now)
{
    const int64_t age = now - mtime;
    for (const AgeBucket &b : ageBuckets())
        if (age < b.maxAge)
            return b.color;
    return ageBuckets().back().color;
}
