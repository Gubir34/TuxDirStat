// Çekirdek birim testleri: tarama, toplamlar, sıralama, gruplama, silme ve rapor.
// Çalıştırma: cmake -DTUXDIRSTAT_TESTS=ON … && ctest --test-dir build

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include <sys/stat.h>
#include <unistd.h>

#include "extinfo.h"
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

int main()
{
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

    removePath(dir, nullptr);
    if (failures)
        std::fprintf(stderr, "%d kontrol başarısız\n", failures);
    else
        std::printf("Tüm çekirdek testleri geçti.\n");
    return failures ? 1 : 0;
}
