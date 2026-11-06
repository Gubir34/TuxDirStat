// Scanning another computer over SSH. The remote side runs TuxDirStat 1.5 or newer
// (fast, ncdu JSON export) when installed, otherwise GNU find; no installation needed.

#include "tools.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

#include "i18n.h"
#include "session.h"

namespace {

std::string shellQuote(const std::string &s)
{
    std::string o = "'";
    for (char c : s) {
        if (c == '\'')
            o += "'\\''";
        else
            o += c;
    }
    return o + "'";
}

bool validHost(std::string_view h)
{
    if (h.empty() || h[0] == '-')
        return false;
    for (char c : h)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_' || c == '@'))
            return false;
    return true;
}

// The script runs under the remote user's shell as `sh -c SCRIPT sh PATH XDEV`.
const char *const REMOTE_SCRIPT = R"SH(p=$1; x=$2
case $p in "~") p=$HOME ;; "~/"*) p=$HOME/${p#"~/"} ;; esac
if command -v tuxdirstat >/dev/null 2>&1 &&
   tuxdirstat --version 2>/dev/null | awk '{ split($2, v, "."); exit !(v[1] > 1 || (v[1] == 1 && v[2] >= 5)) }'; then
  if [ -n "$x" ]; then a=; else a=--all-fs; fi
  exec tuxdirstat --export - $a -- "$p" 2>/dev/null
fi
[ -e "$p" ] || { echo "TUXDIRSTAT-ERROR missing" >&2; exit 3; }
printf 'TUXDIRSTAT-FIND\n'
exec find "$p" $x -printf '%y\t%s\t%b\t%T@\t%U\t%p\0' 2>/dev/null
)SH";

// Builds a tree from `find -printf '%y\t%s\t%b\t%T@\t%U\t%p\0'` output, which lists every
// folder before its contents (depth-first).
std::unique_ptr<Tree> treeFromFind(const std::string &data, size_t pos, const std::string &rootName, std::string *err,
                                   const std::atomic<bool> *cancel)
{
    auto tree = std::make_unique<Tree>();
    tree->arenas.push_back(std::make_unique<Arena>());
    Arena &arena = *tree->arenas.front();
    struct Frame {
        Node *dir;
        std::string path;
        std::vector<Node *> kids;
    };
    std::vector<Frame> st;
    auto close = [&] {
        Frame &f = st.back();
        f.dir->children = arena.kids(f.kids);
        updateDir(f.dir);
        st.pop_back();
    };
    uint64_t count = 0;
    while (pos < data.size()) {
        if ((++count & 0x3FFF) == 0 && cancel && *cancel)
            return nullptr;
        size_t end = data.find('\0', pos);
        if (end == std::string::npos)
            end = data.size();
        std::string_view rec(data.data() + pos, end - pos);
        pos = end + 1;
        // type, size, blocks, mtime, uid, path (the path may itself contain tabs)
        std::string_view f[5];
        bool ok = true;
        for (auto &field : f) {
            const size_t t = rec.find('\t');
            if (t == std::string_view::npos) {
                ok = false;
                break;
            }
            field = rec.substr(0, t);
            rec.remove_prefix(t + 1);
        }
        if (!ok || rec.empty())
            continue;
        const std::string path(rec);
        const bool dir = f[0] == "d";
        Node *n;
        if (st.empty()) {
            if (tree->root)
                continue;  // only one root
            n = arena.node(nullptr, rootName.size() > 65535 ? rootName.substr(0, 65535) : rootName, dir);
            tree->root = n;
        } else {
            const size_t slash = path.rfind('/');
            const std::string parent = slash == 0 ? "/" : path.substr(0, slash == std::string::npos ? 0 : slash);
            while (!st.empty() && st.back().path != parent)
                close();
            if (st.empty())
                continue;
            std::string_view name = std::string_view(path).substr(slash + 1);
            if (name.empty() || name.size() > 65535)
                continue;
            n = arena.node(st.back().dir, name, dir);
            st.back().kids.push_back(n);
            if (!dir)
                n->ext = ExtTable::forName(name);
        }
        n->mtime = int64_t(std::strtod(std::string(f[3]).c_str(), nullptr));
        n->uid = uint32_t(std::strtoul(std::string(f[4]).c_str(), nullptr, 10));
        if (dir) {
            st.push_back({n, path, {}});
        } else {
            n->size = std::strtoull(std::string(f[1]).c_str(), nullptr, 10);
            n->alloc = std::strtoull(std::string(f[2]).c_str(), nullptr, 10) * 512;
            if (f[0] == "l")
                n->flags |= F_LINK;
        }
    }
    while (!st.empty())
        close();
    if (!tree->root) {
        if (err)
            *err = T("The remote folder could not be read.");
        return nullptr;
    }
    if (!tree->root->isDir) {
        if (err)
            *err = T("The remote path is not a folder.");
        return nullptr;
    }
    return tree;
}

}  // namespace

bool parseRemote(const std::string &spec, std::string &host, std::string &path)
{
    if (spec.rfind("ssh://", 0) == 0) {
        const std::string rest = spec.substr(6);
        const size_t slash = rest.find('/');
        host = rest.substr(0, slash);
        path = slash == std::string::npos ? "~" : rest.substr(slash);
        return validHost(host);
    }
    const size_t colon = spec.find(':');
    if (colon == std::string::npos || colon == 0 || spec.find('/') < colon)
        return false;
    host = spec.substr(0, colon);
    path = spec.substr(colon + 1);
    if (path.empty())
        path = "~";
    if (!validHost(host) || (path[0] != '/' && path[0] != '~'))
        return false;
    struct stat st;
    return ::stat(spec.c_str(), &st) != 0;  // an existing local name wins
}

bool isLocalTree(const Node *root)
{
    return root && root->nameLen > 0 && root->name[0] == '/';
}

std::unique_ptr<Tree> remoteScan(const std::string &spec, bool oneFs, std::string *err, const std::atomic<bool> *cancel)
{
    std::string host, path;
    if (!parseRemote(spec, host, path)) {
        if (err)
            *err = TF("Not a remote location: %1", {spec});
        return nullptr;
    }
    if (!haveProgram("ssh")) {
        if (err)
            *err = T("ssh was not found; install OpenSSH to scan other computers.");
        return nullptr;
    }
    const std::string cmd = "sh -c " + shellQuote(REMOTE_SCRIPT) + " sh " + shellQuote(path) + (oneFs ? " -xdev" : " ''");
    std::string out;
    const int rc = runCapture({"ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=15", "--", host, cmd}, &out);
    if (cancel && *cancel)
        return nullptr;
    if (rc == 255 || rc < 0) {
        if (err)
            *err = TF("Could not connect to %1. Scanning over SSH needs key login without a password prompt; "
                      "check that \"ssh %1\" works in a terminal.",
                      {host});
        return nullptr;
    }
    if (rc == 3 || out.empty()) {
        if (err)
            *err = TF("%1 does not exist on %2 or cannot be read.", {path, host});
        return nullptr;
    }
    static const std::string marker = "TUXDIRSTAT-FIND\n";
    std::unique_ptr<Tree> t;
    if (out.compare(0, marker.size(), marker) == 0) {
        // GNU find names the root as given; show it as host:path.
        const size_t first = out.find('\0', marker.size());
        std::string rootPath = path;
        if (first != std::string::npos) {
            const std::string_view rec(out.data() + marker.size(), first - marker.size());
            size_t tabs = 0, at = 0;
            while (tabs < 5 && (at = rec.find('\t', at)) != std::string_view::npos) {
                ++at;
                ++tabs;
            }
            if (tabs == 5)
                rootPath = std::string(rec.substr(at));
        }
        t = treeFromFind(out, marker.size(), host + ":" + rootPath, err, cancel);
    } else {
        t = parseSnapshot(out, err, nullptr, cancel);
        if (t) {
            Arena &a = t->scratch();
            const std::string name = host + ":" + std::string(t->root->nameView());
            t->root->name = a.str(name);
            t->root->nameLen = uint16_t(std::min<size_t>(name.size(), 65535));
        }
    }
    return t;
}
