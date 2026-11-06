#pragma once
// Veri modeli, tarayıcı ve yardımcılar.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <sys/types.h>

enum NodeFlag : uint8_t {
    F_ERROR = 1,  // okunamadı
    F_MOUNT = 2,  // başka dosya sistemi, girilmedi
    F_LINK = 4,   // sembolik bağ
    F_GROUP = 8,  // sanal grup: bir klasördeki çok sayıda benzer öğe
};

struct Node;

// Arena içinde duran çocuk dizisi. Bellek arenaya ait olduğu için serbest bırakılmaz.
struct Kids {
    Node **ptr = nullptr;
    uint32_t n = 0;

    Node **begin() const { return ptr; }
    Node **end() const { return ptr + n; }
    size_t size() const { return n; }
    bool empty() const { return n == 0; }
    Node *operator[](size_t i) const { return ptr[i]; }
    void erase(size_t i)
    {
        std::memmove(ptr + i, ptr + i + 1, (n - i - 1) * sizeof(Node *));
        --n;
    }
};

// Düğümler ağaçtan ayrıldığında tek tek silinmez; bellekleri ait oldukları
// arena (Tree) yok edilince toplu olarak geri verilir.
struct Node {
    Node *parent;
    Kids children;
    const char *name;
    uint64_t size = 0;   // görünür boyut
    uint64_t alloc = 0;  // diskte kapladığı alan
    int64_t mtime = 0;
    uint32_t files = 0;
    uint32_t dirs = 0;
    uint32_t ext = 0;    // ExtTable kimliği (0 = uzantısız)
    uint32_t uid = 0;    // sahibi
    uint32_t sgen = 0;
    int32_t row = 0;
    std::atomic<int32_t> pend{0};  // tarama sırasında bitmemiş alt klasör sayısı + 1
    uint16_t nameLen;
    uint8_t flags = 0;
    bool isDir;

    Node(Node *p, const char *n, uint16_t len, bool dir) : parent(p), name(n), nameLen(len), isDir(dir) {}

    uint64_t sizeOf(bool apparent) const { return apparent ? size : alloc; }
    std::string_view nameView() const { return {name, nameLen}; }
    std::string path() const;
    int depth() const;
    bool isWithin(const Node *other) const;
};

class Arena {
public:
    void *alloc(size_t n, size_t align);
    const char *str(std::string_view s);
    Node *node(Node *parent, std::string_view name, bool dir);
    Kids kids(const std::vector<Node *> &v);

private:
    std::vector<std::unique_ptr<char[]>> blocks_;
    char *cur_ = nullptr;
    char *end_ = nullptr;
};

// Bir taramanın ürettiği ağaç ve onun belleği.
struct Tree {
    Node *root = nullptr;
    std::vector<std::unique_ptr<Arena>> arenas;

    void adopt(std::unique_ptr<Tree> other);  // other'ın belleğini devralır
    Arena &scratch() { return *arenas.front(); }  // tarama bittikten sonra arayüz iş parçacığı kullanır
};

void updateDir(Node *n);

// Ağaç görünümlerinin ortak sıralaması; satır numaralarını (row) da günceller.
enum class SortKey { Name, Size, Items, Files, Dirs, MTime };
void sortKids(Kids &k, SortKey key, bool descending, bool apparent);

// Yapısal değişiklikler. Görünümler bunları kendi değişiklik bildirimleriyle sarar.
void detachNode(Node *n);                      // ebeveyninden çıkarır
void takeChildren(Node *target, Node *fresh);  // fresh'in çocuklarını ve toplamlarını target'a taşır
void propagateUp(Node *n);                     // n ve üstlerinin toplamlarını yeniden hesaplar

// Gruplama: bir klasörde en az `minCount` benzer öğe varsa F_GROUP bayraklı sanal bir
// düğümün altına taşınır. Üç tür grup vardır:
//   - aynı uzantılı dosyalar ("*.dds"; ext = uzantı kimliği)
//   - uzantıya göre toplanamayan küçük dosyalar (ext = GROUP_SMALL)
//   - küçük alt klasörler (ext = GROUP_DIRS)
// "Küçük": klasör toplamının %1'inden az. Gruplar yol (path) hesabında atlanır.
constexpr uint32_t GROUP_MIN = 16;
constexpr uint32_t GROUP_SMALL = 0xFFFFFFFD;
constexpr uint32_t GROUP_DIRS = 0xFFFFFFFE;
void groupFiles(Node *root, Arena &arena, uint32_t minCount = GROUP_MIN);
void ungroupFiles(Node *root, Arena &arena);
Node *realNode(Node *n);                                 // n ya da grup olmayan en yakın atası
std::vector<std::string> diskPaths(const Node *n);       // silinecek gerçek yollar (grup için içindekiler)
std::string displayName(const Node *n, bool isRoot);     // ağaçta gösterilecek ad

namespace ExtTable {
uint32_t intern(std::string_view e);
std::string name(uint32_t id);
uint32_t count();
uint32_t forName(std::string_view fileName);  // dosya adının uzantı kimliği
}

struct ExtStat {
    uint64_t size = 0, alloc = 0, count = 0;
};
std::vector<ExtStat> computeExtStats(const Node *root);

struct Mount {
    std::string src, mp, fs;
};
std::vector<Mount> readMounts();
bool isVirtualFs(const std::string &fs);

class Scanner {
public:
    Scanner(std::string path, bool oneFs, int workers);
    ~Scanner();

    void start();
    void cancel() { cancelled_ = true; }
    bool isDone() const { return done_; }
    bool isCancelled() const { return cancelled_; }
    std::unique_ptr<Tree> takeTree();
    const std::string &path() const { return path_; }
    std::string currentPath();

    std::string error;
    double elapsed = 0;
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> bytes{0};
    std::vector<ExtStat> extStats;  // tamamlanınca dolu

private:
    struct Item {
        Node *node;
        std::string path;
        dev_t dev;
    };
    struct DevIno {
        size_t operator()(const std::pair<dev_t, ino_t> &k) const {
            return std::hash<uint64_t>()(uint64_t(k.second) * 1000003u ^ uint64_t(k.first));
        }
    };
    struct Ctx;

    void run();
    void worker(int idx, Arena *arena);
    void scanDir(Item &it, Ctx &ctx);
    static void finish(Node *n);

    std::string path_;
    bool oneFs_;
    int nworkers_;
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> done_{false};
    std::unique_ptr<Tree> tree_;
    std::thread thread_;

    std::mutex qm_;
    std::condition_variable cv_;
    std::vector<Item> stack_;
    size_t pending_ = 0;
    std::vector<std::vector<ExtStat>> workerExt_;

    std::mutex seenM_;
    std::unordered_set<std::pair<dev_t, ino_t>, DevIno> seen_;

    std::mutex curM_;
    std::string current_;

    std::unordered_map<std::string, std::pair<std::string, std::string>> mounts_;  // mp -> (src, fs)
    std::string rootSrc_;
};

std::string fmtSize(uint64_t b);
std::string fmtInt(uint64_t n);
std::string fmtTime(int64_t t);
std::string fmtPct(double frac);
std::string fmtDecimal(double v, int digits);
std::string userName(uint32_t uid);  // bulunamazsa sayı

struct RGBf {
    float r, g, b;
};
constexpr float PALETTE_BRIGHTNESS = 0.6f;
RGBf normalizeColor(int r, int g, int b, float brightness = PALETTE_BRIGHTNESS);
const std::vector<RGBf> &palette();
RGBf dirColor();

// Treemap renklendirmesi: uzantıya ya da son değişiklik yaşına göre.
enum class ColorMode { Extension, Age };
struct AgeBucket {
    int64_t maxAge;     // saniye; son kova için INT64_MAX
    const char *label;  // İngilizce; T() ile çevrilir
    RGBf color;
};
const std::vector<AgeBucket> &ageBuckets();
RGBf ageColor(int64_t mtime, int64_t now);
