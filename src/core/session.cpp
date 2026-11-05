#include "session.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <fcntl.h>
#include <ftw.h>
#include <pwd.h>
#include <spawn.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char **environ;

// ───────────────────────────── Oturum ─────────────────────────────

Session::~Session()
{
    scanner_.reset();
}

void Session::startScan(const std::string &path, Node *target)
{
    scanner_ = std::make_unique<Scanner>(path, oneFs, scanWorkers());
    target_ = target;
    pending_.reset();
    scanner_->start();
}

void Session::cancelScan()
{
    if (scanner_)
        scanner_->cancel();
}

uint64_t Session::progressCount() const
{
    return scanner_ ? scanner_->count.load() : 0;
}

uint64_t Session::progressBytes() const
{
    return scanner_ ? scanner_->bytes.load() : 0;
}

std::string Session::progressPath() const
{
    return scanner_ ? scanner_->currentPath() : std::string();
}

Session::Result Session::poll()
{
    if (!scanner_)
        return Result::Idle;
    if (!scanner_->isDone())
        return Result::Running;
    std::unique_ptr<Scanner> sc = std::move(scanner_);
    error_ = sc->error;
    elapsed_ = sc->elapsed;
    count_ = sc->count;
    if (!error_.empty())
        return Result::Failed;
    pending_ = sc->takeTree();
    if (!pending_)
        return Result::Cancelled;
    if (target_ && (!root() || !target_->isWithin(root()))) {
        pending_.reset();
        target_ = nullptr;
        return Result::Cancelled;
    }
    pendingExt_ = std::move(sc->extStats);
    if (grouping)
        groupFiles(pending_->root, pending_->scratch(), groupMin);
    return target_ ? Result::Refresh : Result::NewTree;
}

void Session::commitNewTree()
{
    tree_ = std::move(pending_);  // eski ağacın belleği burada topluca bırakılır
    recomputeExt(&pendingExt_);
    pendingExt_.clear();
}

void Session::commitRefresh()
{
    Node *t = target_;
    tree_->adopt(std::move(pending_));
    target_ = nullptr;
    propagateUp(t->parent);
    recomputeExt();
}

void Session::setGrouping(bool on, uint32_t minCount)
{
    if (root() && grouping)
        ungroupFiles(root(), tree_->scratch());
    grouping = on;
    groupMin = minCount;
    if (root() && grouping)
        groupFiles(root(), tree_->scratch(), groupMin);
}

Node *Session::removalTarget(Node *n) const
{
    while (n->parent && (n->parent->flags & F_GROUP) && n->parent->children.size() == 1)
        n = n->parent;
    return n;
}

void Session::remove(Node *n)
{
    Node *p = n->parent;
    detachNode(n);  // bellek ağaçla birlikte bırakılır
    propagateUp(p);
    recomputeExt();
}

void Session::recomputeExt(const std::vector<ExtStat> *precomputed)
{
    ext.clear();
    colors.clear();
    if (!root())
        return;
    const std::vector<ExtStat> stats = precomputed && !precomputed->empty() ? *precomputed : computeExtStats(root());
    for (uint32_t i = 0; i < stats.size(); ++i)
        if (stats[i].count)
            ext.push_back({i, apparent ? stats[i].size : stats[i].alloc, stats[i].count, {}});
    std::sort(ext.begin(), ext.end(), [](const ExtRow &a, const ExtRow &b) { return a.bytes > b.bytes; });
    colors.assign(std::max<size_t>(stats.size(), ExtTable::count()), dirColor());
    const auto &pal = palette();
    for (size_t r = 0; r < ext.size(); ++r) {
        ext[r].color = pal[r % pal.size()];
        colors[ext[r].ext] = ext[r].color;
    }
}

std::string Session::extLabel(uint32_t e) const
{
    return e ? "." + ExtTable::name(e) : std::string("(yok)");
}

// ───────────────────────────── Sistem işlemleri ─────────────────────────────

static int runWait(const std::vector<std::string> &args)
{
    std::vector<char *> argv;
    for (const auto &a : args)
        argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0)
        return -1;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Ana süreç beklemeden çalıştırır (çift fork ile zombi bırakmaz).
static void spawnDetached(const std::vector<std::string> &args)
{
    std::vector<std::string> sh{"/bin/sh", "-c", "\"$@\" >/dev/null 2>&1 &", "sh"};
    sh.insert(sh.end(), args.begin(), args.end());
    runWait(sh);
}

static int rmEntry(const char *path, const struct stat *, int type, struct FTW *)
{
    return (type == FTW_DP ? ::rmdir(path) : ::unlink(path)) == 0 ? 0 : -1;
}

bool removePath(const std::string &path, std::string *err)
{
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) {
        if (err)
            *err = std::strerror(errno);
        return false;
    }
    int rc = S_ISDIR(st.st_mode) ? ::nftw(path.c_str(), rmEntry, 64, FTW_DEPTH | FTW_PHYS) : ::unlink(path.c_str());
    if (rc != 0 && err)
        *err = std::strerror(errno);
    return rc == 0;
}

bool trashWithCommand(const std::string &path)
{
    if (runWait({"gio", "trash", "--", path}) == 0)
        return true;
    return runWait({"kioclient", "move", path, "trash:/"}) == 0;
}

void openPath(const std::string &path)
{
    spawnDetached({"xdg-open", path});
}

static std::string fileUrl(const std::string &path)
{
    static const char *hex = "0123456789ABCDEF";
    std::string u = "file://";
    for (unsigned char c : path) {
        if (std::isalnum(c) || std::strchr("/-_.~", c)) {
            u += char(c);
        } else {
            u += '%';
            u += hex[c >> 4];
            u += hex[c & 15];
        }
    }
    return u;
}

void revealPath(const std::string &path)
{
    int rc = runWait({"dbus-send", "--session", "--dest=org.freedesktop.FileManager1", "--type=method_call",
                      "/org/freedesktop/FileManager1", "org.freedesktop.FileManager1.ShowItems",
                      "array:string:" + fileUrl(path), "string:"});
    if (rc != 0) {
        auto slash = path.rfind('/');
        openPath(slash == 0 ? "/" : path.substr(0, slash));
    }
}

uint64_t freeSpace(const std::string &path)
{
    struct statvfs sv;
    return ::statvfs(path.c_str(), &sv) == 0 ? uint64_t(sv.f_bavail) * sv.f_frsize : 0;
}

bool diskUsage(const std::string &path, uint64_t &total, uint64_t &avail)
{
    struct statvfs sv;
    if (::statvfs(path.c_str(), &sv) != 0 || sv.f_blocks == 0)
        return false;
    total = uint64_t(sv.f_blocks) * sv.f_frsize;
    avail = uint64_t(sv.f_bavail) * sv.f_frsize;
    return true;
}

std::string homeDir()
{
    if (const char *h = std::getenv("HOME"))
        return h;
    if (const passwd *pw = getpwuid(getuid()))
        return pw->pw_dir;
    return "/";
}

int scanWorkers()
{
    return std::clamp(int(std::thread::hardware_concurrency()) * 2, 4, 32);
}

std::vector<Drive> listDrives()
{
    static const std::unordered_set<std::string> net{"nfs", "nfs4", "cifs", "smb3", "fuse.sshfs",
                                                     "9p", "virtiofs", "zfs", "fuseblk"};
    std::vector<Mount> mounts = readMounts();
    std::sort(mounts.begin(), mounts.end(), [](const Mount &a, const Mount &b) { return a.mp < b.mp; });
    std::vector<Drive> out;
    std::unordered_set<std::string> seen;
    for (const Mount &m : mounts) {
        const bool real = (m.src.rfind("/dev/", 0) == 0 && m.fs != "squashfs" && m.fs != "iso9660") || net.count(m.fs);
        if (!real || !seen.insert(m.mp).second)
            continue;
        struct statvfs sv;
        if (::statvfs(m.mp.c_str(), &sv) != 0 || sv.f_blocks == 0)
            continue;
        const uint64_t total = uint64_t(sv.f_blocks) * sv.f_frsize;
        out.push_back({m.src, m.mp, m.fs, total, total - uint64_t(sv.f_bfree) * sv.f_frsize});
    }
    return out;
}
