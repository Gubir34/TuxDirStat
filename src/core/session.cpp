#include "session.h"

#include <chrono>
#include <csignal>
#include <poll.h>

#include "tools.h"
#include "i18n.h"

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

// Arka planda ağaç üreten iş (anlık görüntü yükleme, yönetici taraması). İptal edilen
// yükleyici kendi iş parçacığında bitene dek yaşar; oturum yalnızca ona bıraktığı
// paylaşılan işaretçiyi düşürür.
struct Session::Loader {
    std::mutex m;
    std::string status;
    std::unique_ptr<Tree> tree;
    std::string error;
    double elapsed = 0;
    uint64_t count = 0;
    std::atomic<bool> done{false}, cancelled{false};
    pid_t child = 0;

    void setStatus(std::string s)
    {
        std::lock_guard l(m);
        status = std::move(s);
    }
};

Session::~Session()
{
    scanner_.reset();
    if (loader_)
        loader_->cancelled = true;
}

static uint64_t countNodes(const Node *root)
{
    uint64_t n = 0;
    std::vector<const Node *> st{root};
    while (!st.empty()) {
        const Node *d = st.back();
        st.pop_back();
        n += d->children.size();
        for (const Node *c : d->children)
            if (c->isDir)
                st.push_back(c);
    }
    return n;
}

void Session::startLoader(std::string status, std::function<std::unique_ptr<Tree>(Loader &)> fn)
{
    scanner_.reset();
    auto ld = std::make_shared<Loader>();
    ld->status = std::move(status);
    loader_ = ld;
    target_ = nullptr;
    pending_.reset();
    std::thread([ld, fn = std::move(fn)] {
        auto t0 = std::chrono::steady_clock::now();
        std::unique_ptr<Tree> t = fn(*ld);
        if (t)
            ld->count = countNodes(t->root);
        ld->tree = std::move(t);
        ld->elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        ld->done = true;
    }).detach();
}

void Session::openSnapshot(const std::string &file)
{
    startLoader(TF("Loading %1…", {file}), [file](Loader &ld) {
        std::unique_ptr<Tree> t = loadSnapshot(file, &ld.error, nullptr, &ld.cancelled);
        return t;
    });
}

void Session::startPrivilegedScan(const std::string &path)
{
    const bool allFs = !oneFs;
    startLoader(TF("Waiting for administrator scan of %1…", {path}), [path, allFs](Loader &ld) -> std::unique_ptr<Tree> {
        const char *rt = std::getenv("XDG_RUNTIME_DIR");
        std::string tmpl = std::string(rt && *rt ? rt : "/tmp") + "/tuxdirstat-root-XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back(0);
        const int fd = ::mkstemp(buf.data());
        if (fd < 0) {
            ld.error = std::strerror(errno);
            return nullptr;
        }
        ::close(fd);
        const std::string tmp = buf.data();
        std::vector<std::string> args{"pkexec", selfExe(), "--export", tmp};
        if (allFs)
            args.push_back("--all-fs");
        args.push_back("--");
        args.push_back(path);
        std::string out;
        const int rc = runCapture(args, &out);
        std::unique_ptr<Tree> t;
        if (ld.cancelled) {
        } else if (rc == 126 || rc == 127) {
            ld.error = T("Authorization was refused or cancelled.");
        } else if (rc != 0) {
            ld.error = rc < 0 ? T("pkexec could not be started (is polkit installed?)")
                              : TF("The administrator scan failed (exit code %1).", {std::to_string(rc)});
        } else {
            ld.setStatus(T("Loading administrator scan…"));
            t = loadSnapshot(tmp, &ld.error, nullptr, &ld.cancelled);
        }
        ::unlink(tmp.c_str());
        return t;
    });
}

void Session::startScan(const std::string &path, Node *target)
{
    if (loader_)
        loader_->cancelled = true;
    loader_.reset();
    scanner_ = std::make_unique<Scanner>(path, oneFs, scanWorkers());
    target_ = target;
    pending_.reset();
    scanner_->start();
}

void Session::cancelScan()
{
    if (scanner_)
        scanner_->cancel();
    if (loader_)
        loader_->cancelled = true;
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

std::string Session::progressText() const
{
    if (loader_) {
        std::lock_guard l(loader_->m);
        return loader_->status;
    }
    return TF("Scanning… %1 items · %2 · %3", {fmtInt(progressCount()), fmtSize(progressBytes()), progressPath()});
}

Session::Result Session::poll()
{
    if (loader_) {
        if (loader_->cancelled) {
            loader_.reset();
            return Result::Cancelled;
        }
        if (!loader_->done)
            return Result::Running;
        std::shared_ptr<Loader> ld = std::move(loader_);
        error_ = ld->error;
        elapsed_ = ld->elapsed;
        count_ = ld->count;
        if (!ld->tree)
            return error_.empty() ? Result::Cancelled : Result::Failed;
        error_.clear();
        pending_ = std::move(ld->tree);
        pendingExt_.clear();
        target_ = nullptr;
        if (grouping)
            groupFiles(pending_->root, pending_->scratch(), groupMin);
        return Result::NewTree;
    }
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
    return e ? "." + ExtTable::name(e) : std::string(T("(none)"));
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

int runCapture(const std::vector<std::string> &args, std::string *out, int timeoutMs)
{
    if (args.empty())
        return -1;
    int pfd[2];
    if (::pipe2(pfd, O_CLOEXEC) != 0)
        return -1;
    std::vector<char *> argv;
    for (const auto &a : args)
        argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, pfd[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    const int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(pfd[1]);
    if (rc != 0) {
        ::close(pfd[0]);
        return -1;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    bool timedOut = false;
    char buf[65536];
    for (;;) {
        int wait = -1;
        if (timeoutMs > 0) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) {
                timedOut = true;
                break;
            }
            wait = int(left);
        }
        pollfd p{pfd[0], POLLIN, 0};
        const int pr = ::poll(&p, 1, wait);
        if (pr < 0 && errno == EINTR)
            continue;
        if (pr == 0)
            continue;
        const ssize_t n = ::read(pfd[0], buf, sizeof buf);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        if (out)
            out->append(buf, size_t(n));
    }
    ::close(pfd[0]);
    if (timedOut)
        ::kill(pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (timedOut)
        return -2;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool haveProgram(const std::string &name)
{
    const char *path = std::getenv("PATH");
    std::string p = path ? path : "/usr/local/bin:/usr/bin:/bin";
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find(':', start);
        if (end == std::string::npos)
            end = p.size();
        const std::string dir = p.substr(start, end - start);
        if (!dir.empty() && ::access((dir + "/" + name).c_str(), X_OK) == 0)
            return true;
        start = end + 1;
    }
    return false;
}

std::string selfExe()
{
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0)
        return "tuxdirstat";
    return std::string(buf, size_t(n));
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
