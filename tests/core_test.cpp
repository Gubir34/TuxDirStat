// Çekirdek birim testleri: tarama, toplamlar, sıralama, gruplama, silme ve rapor.
// Çalıştırma: cmake -DTUXDIRSTAT_TESTS=ON … && ctest --test-dir build

#include "treemap_render.h"
#include <iterator>
#include <chrono>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include <sys/stat.h>
#include <unistd.h>

#include "extinfo.h"
#include "i18n.h"
#include "tools.h"
#include "report.h"
#include "session.h"

static int failures = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "%s:%d: BAŞARISIZ: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

static void writeFile(const std::string &path, size_t bytes)
{
    std::ofstream(path, std::ios::binary) << std::string(bytes, 'x');
}

static Node *child(Node *n, const char *name)
{
    for (Node *c : n->children)
        if (c->nameView() == name)
            return c;
    return nullptr;
}

static Node *groupOf(Node *d, uint32_t kind)
{
    for (Node *c : d->children)
        if ((c->flags & F_GROUP) && c->ext == kind)
            return c;
    return nullptr;
}

static std::unique_ptr<Tree> scan(const std::string &path)
{
    Scanner sc(path, true, 4);
    sc.start();
    while (!sc.isDone())
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return sc.takeTree();
}

// Ağacın tüm toplamlarının çocuklarından tutarlı hesaplandığını doğrular.
static bool consistent(Node *n)
{
    if (!n->isDir)
        return true;
    uint64_t size = 0, alloc = 0;
    uint32_t files = 0, dirs = 0;
    for (Node *c : n->children) {
        if (c->parent != n || !consistent(c))
            return false;
        size += c->size;
        alloc += c->alloc;
        files += c->isDir ? c->files : 1;
        dirs += c->isDir ? c->dirs + ((c->flags & F_GROUP) ? 0 : 1) : 0;
    }
    return size == n->size && alloc == n->alloc && files == n->files && dirs == n->dirs;
}

static std::unique_ptr<Tree> loadAsync(const std::string &file, Session &s)
{
    s.openSnapshot(file);
    Session::Result r;
    while ((r = s.poll()) == Session::Result::Running)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(r == Session::Result::NewTree);
    return nullptr;
}

static bool sameTotals(const Node *a, const Node *b)
{
    return a->size == b->size && a->alloc == b->alloc && a->files == b->files && a->dirs == b->dirs;
}

// Yeni araçlar: arama, kopyalar, anlık görüntü, karşılaştırma, CSV, sahipler, temizlik.
static void toolTests(const std::string &dir)
{
    // Arayüzler setlocale() çağırır; biçimlendirme C yerelinden etkilenmemeli.
    std::setlocale(LC_ALL, "tr_TR.UTF-8");
    i18n::init("en");
    ::mkdir((dir + "/dup").c_str(), 0755);
    std::ofstream(dir + "/dup/a.bin", std::ios::binary) << std::string(70000, 'q') << "end";
    std::ofstream(dir + "/dup/b.bin", std::ios::binary) << std::string(70000, 'q') << "end";
    std::ofstream(dir + "/dup/c.bin", std::ios::binary) << std::string(70000, 'q') << "END";  // aynı boyut, farklı son
    CHECK(::link((dir + "/dup/a.bin").c_str(), (dir + "/dup/hard.bin").c_str()) == 0);   // sabit bağ: kopya değil
    ::mkdir((dir + "/proj").c_str(), 0755);
    ::mkdir((dir + "/proj/node_modules").c_str(), 0755);
    writeFile(dir + "/proj/node_modules/x.js", 1500000);
    ::mkdir((dir + "/proj/build").c_str(), 0755);
    writeFile(dir + "/proj/build/CMakeCache.txt", 1500000);
    writeFile(dir + "/proj/movie.mp4.part", 1500000);

    auto tree = scan(dir);
    Node *root = tree->root;
    Job job("test", [](Job &) {});

    // — kopyalar —
    ResultTable d = findDuplicates(root, job);
    // Üç küme: dup/ içindeki a|hard + b, içerikleri aynı 5 .wav ve proj/ altındaki 3 dosya.
    std::vector<int> dupRows;
    for (size_t i = 0; i < d.rows.size(); ++i)
        if (d.rows[i].path.find("/dup/") != std::string::npos)
            dupRows.push_back(int(i));
    CHECK(d.grouped && d.rows.size() == 2 + 5 + 3);
    CHECK(dupRows.size() == 2 && d.rows[size_t(dupRows[0])].group == d.rows[size_t(dupRows[1])].group);
    for (int i : dupRows)
        CHECK(d.rows[size_t(i)].path.find("c.bin") == std::string::npos);
    CHECK(!deletionWarning(d, dupRows).empty());
    CHECK(deletionWarning(d, {dupRows[0]}).empty());

    // — arama —
    std::string err;
    ResultTable q = searchTree(root, "*.wav >100K", true, &err);
    CHECK(err.empty() && q.rows.size() == 5);
    q = searchTree(root, "type:d node", true, &err);
    CHECK(q.rows.size() == 1 && q.rows[0].node && q.rows[0].node->nameView() == "node_modules");
    q = searchTree(root, "ext:wav,.exe newer:1d", true, &err);
    CHECK(q.rows.size() == 6);
    q = searchTree(root, "older:2y", true, &err);
    CHECK(q.rows.empty());
    searchTree(root, ">12Q", true, &err);
    CHECK(!err.empty());

    // — temizlik —
    ResultTable c = findCleanup(root, true, job);
    auto hasPath = [&](const ResultTable &t, const std::string &p) {
        for (const ResultRow &r : t.rows)
            if (r.path == p)
                return true;
        return false;
    };
    CHECK(hasPath(c, dir + "/proj/node_modules"));
    CHECK(hasPath(c, dir + "/proj/build"));
    CHECK(hasPath(c, dir + "/proj/movie.mp4.part"));

    // — sahipler —
    ResultTable ow = ownerUsage(root, true);
    CHECK(ow.rows.size() == 1 && ow.rows[0].cells[0].find("(" + std::to_string(getuid()) + ")") != std::string::npos);

    // — anlık görüntü: kaydet, yükle, karşılaştır —
    const std::string snap = dir + "/../" + std::string(std::strrchr(dir.c_str(), '/') + 1) + ".json";
    CHECK(saveSnapshot(root, snap, &err));
    int64_t ts = 0;
    auto loaded = loadSnapshot(snap, &err, &ts);
    CHECK(loaded && ts > 0);
    CHECK(loaded && consistent(loaded->root) && sameTotals(loaded->root, root));
    CHECK(loaded && loaded->root->path() == root->path());
    Session ls;
    loadAsync(snap, ls);
    ls.commitNewTree();
    CHECK(ls.root() && sameTotals(ls.root(), root) && !ls.ext.empty());
    writeFile(dir + "/content/new.bin", 123456);
    auto again = scan(dir);
    ResultTable cmp = compareTrees(again->root, loaded->root, ts, true);
    CHECK(!cmp.rows.empty() && hasPath(cmp, dir + "/content/new.bin"));
    CHECK(hasPath(cmp, dir + "/content"));

    // — ncdu biçimi —
    const std::string ncdu = dir + "/../ncdu-" + std::string(std::strrchr(dir.c_str(), '/') + 1) + ".json";
    std::ofstream(ncdu) << R"([1,1,{"progname":"ncdu","progver":"1.19","timestamp":1700000000},
[{"name":"/data","asize":4096,"dsize":4096,"dev":2049,"ino":2},
{"name":"caf\u00e9.txt","asize":10,"dsize":4096,"ino":3},
[{"name":"sub","asize":4096,"dsize":4096,"ino":4,"read_error":true},
{"name":"x \"q\".iso","asize":1000000,"dsize":1003520,"ino":5,"mtime":1600000000}],
{"name":"mnt","excluded":"othfs"},
{"name":"lnk","asize":7,"dsize":0,"notreg":true}]])";
    auto nt = loadSnapshot(ncdu, &err, &ts);
    CHECK(nt && ts == 1700000000);
    if (nt) {
        Node *r = nt->root;
        CHECK(r->files == 3 && r->dirs == 2 && r->alloc == 4096 + 1003520);
        CHECK(child(r, "café.txt") != nullptr);
        Node *sub = child(r, "sub");
        CHECK(sub && (sub->flags & F_ERROR) && child(sub, "x \"q\".iso"));
        CHECK(child(r, "mnt") && (child(r, "mnt")->flags & F_MOUNT));
        CHECK(child(r, "lnk") && (child(r, "lnk")->flags & F_LINK));
    }
    std::ofstream(ncdu) << "[1,2,{},[{\"name\":\"/x\"},{\"name\":";
    CHECK(!loadSnapshot(ncdu, &err) && !err.empty());

    // — CSV —
    const std::string csv = dir + "/../" + std::string(std::strrchr(dir.c_str(), '/') + 1) + ".csv";
    CHECK(exportCsv(root, csv, &err));
    std::ifstream cf(csv);
    std::string line;
    size_t lines = 0;
    while (std::getline(cf, line))
        ++lines;
    CHECK(lines == 1 + 1 + root->files + root->dirs);  // başlık + kök + her öğe

    // — yaş renkleri —
    const int64_t now = std::time(nullptr);
    CHECK(ageColor(now, now).r == ageBuckets().front().color.r);
    CHECK(ageColor(now - 10LL * 365 * 86400, now).b == ageBuckets().back().color.b);

    // — İngilizce biçim ve metinler —
    CHECK(fmtSize(1536) == "1.5 KB");
    CHECK(fmtInt(1234567) == "1,234,567");
    CHECK(fmtPct(0.125) == "12.5 %");
    CHECK(describeExt(0) == "Files without an extension");
    CHECK(builtinExtDescription("dds") == "DirectDraw texture");
    CHECK(genericExtDescription("xyz") == "XYZ file");
    CHECK(builtinExtCount(true) == builtinExtCount(false));
    CHECK(std::string(T("Size")) == "Size");
    i18n::init("tr");
    CHECK(std::string(T("Size")) == "Boyut");
    CHECK(TF("%1 files", {"3"}) == "3 dosya");
    ::unlink(snap.c_str());
    ::unlink(ncdu.c_str());
    ::unlink(csv.c_str());
}

// 1.5: remote scans, PNG/HTML export, sunburst, gzip snapshots, history, languages
static void newTests(const std::string &dir)
{
    std::string host, rpath, err;
    CHECK(parseRemote("pi:/home", host, rpath) && host == "pi" && rpath == "/home");
    CHECK(parseRemote("user@nas.local:~/media", host, rpath) && host == "user@nas.local" && rpath == "~/media");
    CHECK(parseRemote("ssh://box/srv/data", host, rpath) && host == "box" && rpath == "/srv/data");
    CHECK(parseRemote("pi:", host, rpath) && rpath == "~");
    CHECK(!parseRemote("/home/a:b", host, rpath));
    CHECK(!parseRemote("relative/x:/y", host, rpath));
    CHECK(!parseRemote("-oProxyCommand=x:/y", host, rpath));  // never an ssh option
    CHECK(!parseRemote("C:\\x", host, rpath));
    CHECK(!removePath("pi:/home/x", &err) && !err.empty());   // remote paths are never deleted

    // Remote scan through a fake ssh that runs the command locally (GNU find fallback).
    const std::string bin = dir + "/fakebin";
    ::mkdir(bin.c_str(), 0755);
    {
        std::ofstream f(bin + "/ssh");
        f << "#!/bin/sh\nwhile [ $# -gt 0 ]; do case $1 in -o) shift 2 ;; --) shift; break ;; *) break ;; esac; done\n"
             "shift\nexec sh -c \"$*\"\n";
    }
    ::chmod((bin + "/ssh").c_str(), 0755);
    const std::string oldPath = std::getenv("PATH") ? std::getenv("PATH") : "";
    setenv("PATH", (bin + ":" + oldPath).c_str(), 1);
    auto local = scan(dir + "/content");
    auto remote = remoteScan("testhost:" + dir + "/content", true, &err);
    CHECK(remote && local && sameTotals(remote->root, local->root));
    CHECK(remote && !isLocalTree(remote->root) && remote->root->path() == "testhost:" + dir + "/content");
    CHECK(remote && consistent(remote->root) && child(remote->root, "big.pak") && child(remote->root, "big.pak")->alloc > 0);
    CHECK(!remoteScan("testhost:" + dir + "/does-not-exist", true, &err) && !err.empty());
    setenv("PATH", oldPath.c_str(), 1);
    CHECK(isLocalTree(local->root));

    // PNG: signature, header and pixel data survive a zlib round trip.
    const uint32_t px[4] = {0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFFFF};
    std::string png;
    CHECK(encodePng(px, 2, 2, png) && png.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0);
    CHECK(png.size() > 8 + 25 && png.compare(12, 4, "IHDR") == 0 && uint8_t(png[19]) == 2 && uint8_t(png[23]) == 2);

    // Sunburst: the center is the root, rings are its children; outlines draw.
    TreemapRenderer r;
    r.setShape(MapShape::Rings);
    std::vector<RGBf> colors;
    computeExtColors(computeExtStats(local->root), false, colors, nullptr);
    r.render(local->root, 400, 300, false, colors);
    CHECK(r.hit(200, 150) == local->root);
    Node *ring = r.hit(200, 150 - 70);
    CHECK(ring && ring->parent == local->root);
    CHECK(r.hit(2, 2) == nullptr);  // corner: outside the rings
    std::vector<uint32_t> outPx;
    r.compose(outPx, -1, ring);
    CHECK(outPx.size() == 400u * 300u && std::count(outPx.begin(), outPx.end(), 0xFFFFFFFFu) > 10);

    // gzip snapshot and HTML report
    const std::string gz = dir + "/snap.json.gz";
    CHECK(saveSnapshot(local->root, gz, &err));
    {
        std::ifstream f(gz, std::ios::binary);
        char magic[2] = {};
        f.read(magic, 2);
        CHECK(uint8_t(magic[0]) == 0x1f && uint8_t(magic[1]) == 0x8b);
    }
    auto back = loadSnapshot(gz, &err);
    CHECK(back && sameTotals(back->root, local->root));
    const std::string html = dir + "/report.html";
    CHECK(exportHtml(local->root, html, false, &err));
    {
        std::ifstream f(html);
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(text.rfind("<!DOCTYPE html>", 0) == 0 && text.find("data:image/png;base64,") != std::string::npos);
        CHECK(text.find("big.pak") != std::string::npos && text.find("</html>") != std::string::npos);
    }
    CHECK(exportMapPng(local->root, dir + "/map.png", 320, 200, false, ColorMode::Age, MapShape::Rectangles, &err));

    // Size history in a private XDG data folder
    setenv("XDG_DATA_HOME", (dir + "/xdg-data").c_str(), 1);
    setenv("XDG_CACHE_HOME", (dir + "/xdg-cache").c_str(), 1);
    CHECK(recordHistorySnapshot(local->root, &err));
    auto files = historyFiles(local->root->path());
    CHECK(files.size() == 1);
    if (files.size() == 1) {
        const std::string old = files[0].second.substr(0, files[0].second.rfind('/')) + "/2026-01-05.json.gz";
        CHECK(std::rename(files[0].second.c_str(), old.c_str()) == 0);
    }
    writeFile(dir + "/content/grown.bin", 400000);
    auto now = scan(dir + "/content");
    Job hj("h", [](Job &) {});
    ResultTable h = historyGrowth(now->root, false, hj);
    CHECK(h.rows.size() >= 2 && h.rows[0].node == now->root);
    bool grew = false;
    for (const ResultRow &row : h.rows)
        grew |= row.cells[0] == "grown.bin" && row.cells.back() == "▁█";
    CHECK(grew);
    CHECK(!recordHistorySnapshot(remote ? remote->root : nullptr, &err) || !remote);
    CHECK(saveLastScan(now->root, &err));
    auto last = loadSnapshot(lastScanFile(), &err);
    CHECK(last && sameTotals(last->root, now->root));

    // Languages come from po/*.po
    auto langs = i18n::languages();
    CHECK(langs.size() >= 2 && langs[0].code == "en" &&
          std::any_of(langs.begin(), langs.end(), [](const i18n::Language &l) { return l.code == "tr" && l.name == "Türkçe"; }));
    CHECK(i18n::known("tr") && !i18n::known("xx"));
    CHECK(std::string(i18n::dateFormat()) == "%d.%m.%Y %H:%M");
}

int main()
{
    i18n::init("tr");
    char tmpl[] = "/tmp/tuxdirstat-test-XXXXXX";
    const std::string dir = mkdtemp(tmpl);
    // Oyun klasörüne benzer bir yapı
    ::mkdir((dir + "/content").c_str(), 0755);
    ::mkdir((dir + "/cache").c_str(), 0755);
    ::mkdir((dir + "/bin").c_str(), 0755);
    for (int i = 0; i < 40; ++i)
        writeFile(dir + "/content/tex" + std::to_string(i) + ".dds", 2000);
    for (int i = 0; i < 5; ++i)
        writeFile(dir + "/content/snd" + std::to_string(i) + ".wav", 200000);
    for (int i = 0; i < 20; ++i)  // karışık küçük dosyalar
        writeFile(dir + "/content/misc" + std::to_string(i) + ".e" + std::to_string(i), 10);
    writeFile(dir + "/content/big.pak", 3000000);
    for (int i = 0; i < 150; ++i) {  // çok sayıda küçük klasör (her biri toplamın %1'inden az)
        const std::string d = dir + "/cache/c" + std::to_string(i);
        ::mkdir(d.c_str(), 0755);
        writeFile(d + "/blob", 100);
    }
    writeFile(dir + "/bin/game.exe", 5000000);

    // — tarama ve toplamlar —
    auto tree = scan(dir);
    CHECK(tree != nullptr);
    Node *root = tree->root;
    CHECK(root->files == 40 + 5 + 20 + 1 + 150 + 1);
    CHECK(root->dirs == 3 + 150);
    CHECK(consistent(root));
    Node *content = child(root, "content");
    CHECK(content && content->children.size() == 66);

    // — sıralama —
    sortKids(content->children, SortKey::Size, true, true);
    CHECK(std::string(content->children[0]->name) == "big.pak");
    for (uint32_t i = 0; i < content->children.size(); ++i)
        CHECK(content->children[i]->row == int32_t(i));
    sortKids(root->children, SortKey::Name, false, true);
    CHECK(std::string(root->children[0]->name) == "bin");

    // — gruplama —
    const uint64_t sizeBefore = root->size;
    const uint32_t filesBefore = root->files, dirsBefore = root->dirs;
    groupFiles(root, tree->scratch(), 16);
    CHECK(consistent(root));
    CHECK(root->size == sizeBefore && root->files == filesBefore && root->dirs == dirsBefore);

    const uint32_t dds = [&] {
        for (uint32_t e = 1; e < ExtTable::count(); ++e)
            if (ExtTable::name(e) == "dds")
                return e;
        return 0u;
    }();
    Node *ddsGroup = groupOf(content, dds);
    CHECK(ddsGroup && ddsGroup->files == 40);
    CHECK(ddsGroup && displayName(ddsGroup, false) == "*.dds  (40 dosya)");
    Node *smallGroup = groupOf(content, GROUP_SMALL);
    CHECK(smallGroup && smallGroup->files == 20);  // farklı uzantılı küçük dosyalar
    CHECK(child(content, "big.pak") != nullptr);   // büyük dosya grupta değil
    CHECK(child(content, "snd0.wav") != nullptr);  // eşiğin altında kalan uzantı
    Node *dirGroup = groupOf(child(root, "cache"), GROUP_DIRS);
    CHECK(dirGroup && dirGroup->children.size() == 150);
    CHECK(dirGroup && displayName(dirGroup, false) == "küçük klasörler  (150 klasör)");

    // Gruplar yol hesabında atlanır; silme gerçek yolları kullanır.
    Node *tex = ddsGroup->children[0];
    CHECK(tex->path() == dir + "/content/" + std::string(tex->nameView()));
    CHECK(ddsGroup->path() == dir + "/content");
    CHECK(diskPaths(ddsGroup).size() == 40);
    CHECK(realNode(ddsGroup) == content && realNode(tex) == tex);

    // Gruplamayı kapatınca yapı geri gelir.
    ungroupFiles(root, tree->scratch());
    CHECK(consistent(root));
    CHECK(content->children.size() == 66);
    CHECK(child(root, "cache")->children.size() == 150);
    CHECK(root->files == filesBefore && root->dirs == dirsBefore);

    // — oturum: eşik, boş grup temizliği, silme —
    Session s;
    s.grouping = true;
    s.groupMin = 16;
    s.startScan(dir);
    Session::Result r;
    while ((r = s.poll()) == Session::Result::Running)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(r == Session::Result::NewTree);
    s.commitNewTree();
    Node *sc = child(s.root(), "content");
    CHECK(groupOf(sc, dds) != nullptr);
    s.setGrouping(true, 64);  // eşik yükselince .dds grubu dağılır
    CHECK(groupOf(sc, dds) == nullptr);
    CHECK(consistent(s.root()));
    s.setGrouping(true, 16);
    Node *g = groupOf(sc, dds);
    CHECK(g != nullptr);
    // Grubun son öğesine kadar sil: son silmede grup da kalkmalı.
    while (g->children.size() > 1) {
        Node *f = g->children[0];
        CHECK(s.removalTarget(f) == f);
        CHECK(removePath(f->path(), nullptr));
        s.remove(f);
    }
    Node *last = g->children[0];
    CHECK(s.removalTarget(last) == g);
    CHECK(removePath(last->path(), nullptr));
    s.remove(s.removalTarget(last));
    CHECK(groupOf(sc, dds) == nullptr);
    CHECK(consistent(s.root()));
    CHECK(s.root()->files == filesBefore - 40);

    // Klasör grubunu silmek içindeki klasörleri diskten siler.
    Node *cg = groupOf(child(s.root(), "cache"), GROUP_DIRS);
    CHECK(cg != nullptr);
    for (const std::string &p : diskPaths(cg))
        CHECK(removePath(p, nullptr));
    s.remove(cg);
    struct stat st;
    CHECK(::stat((dir + "/cache/c0").c_str(), &st) != 0);
    CHECK(child(s.root(), "cache")->children.empty());

    // — rapor —
    std::ostringstream out;
    ReportOptions o;
    o.top = 3;
    o.grouping = true;
    writeReport(out, s.root(), o, 0.5);
    const std::string rep = out.str();
    CHECK(rep.find("En büyük dosyalar:") != std::string::npos);
    CHECK(rep.find("bin/game.exe") != std::string::npos);
    CHECK(rep.find("küçük dosyalar  (20 dosya)") != std::string::npos);
    CHECK(rep.find("0,50 sn") != std::string::npos);

    // — tür açıklamaları —
    CHECK(describeExt(0) == "Uzantısız dosyalar");
    CHECK(describeExt(dds, "Sistem açıklaması") == "Sistem açıklaması");  // sistem açıklaması önceliklidir
    CHECK(describeExt(dds) == "DirectDraw doku dosyası");
    CHECK(builtinExtDescription("grp") == "Oyun kaynak grubu (Dagor)");
    CHECK(genericExtDescription("xyz") == "XYZ dosyası");
    CHECK(genericExtDescription("08") == "Numaralı dosya (sürüm, parça ya da kılavuz bölümü)");
    CHECK(rep.find("— ") != std::string::npos);

    // — biçimlendirme —
    CHECK(fmtSize(512) == "512 B");
    CHECK(fmtSize(1536) == "1,5 KB");
    CHECK(fmtInt(1234567) == "1.234.567");
    CHECK(fmtPct(0.125) == "12,5 %");

    toolTests(dir);
    newTests(dir);

    removePath(dir, nullptr);
    if (failures)
        std::fprintf(stderr, "%d kontrol başarısız\n", failures);
    else
        std::printf("Tüm çekirdek testleri geçti.\n");
    return failures ? 1 : 0;
}
