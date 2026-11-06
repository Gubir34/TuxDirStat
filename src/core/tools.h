#pragma once
// Ağaç üzerinde çalışan araçlar: arama, kopya dosyalar, temizlik önerileri, anlık
// görüntüler (ncdu JSON), karşılaştırma, CSV, sahip kullanımı ve pacman denetimleri.
//
// Uzun süren araçlar bir Job içinde arka planda çalışır; sonuçları arayüzlerin tek bir
// tablo penceresiyle gösterdiği ResultTable'dır. İş sürerken arayüz ağacı değiştirmemeli
// (ilerleme penceresi kipli açılır).

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core.h"

// ───────────── sonuç tablosu ─────────────

struct ResultRow {
    std::vector<std::string> cells;
    std::string path;      // dosya işlemleri için; boşsa yok
    Node *node = nullptr;  // geçerli ağaçtaki düğüm ("ağaçta göster", silme)
    uint64_t bytes = 0;
    std::string command;   // önerilen kabuk komutu (temizlik)
    int group = 0;         // aynı gruptaki satırlar birlikte (kopyalar)
    bool deletable = true;
};

struct ResultTable {
    std::string title, summary;
    std::string emptyText;  // satır yoksa gösterilecek ileti
    std::vector<std::string> columns;
    std::vector<bool> numeric;  // sağa hizalanan sütunlar
    std::vector<ResultRow> rows;
    bool grouped = false;  // satırlar group alanına göre kümelenmiş (kopyalar)
};

// Silinmek üzere seçilen satırlar bir kopya grubunun tüm üyelerini içeriyorsa uyarı metni.
std::string deletionWarning(const ResultTable &t, const std::vector<int> &rows);

// Tablonun düz metin hâli (komut satırı).
void printTable(std::ostream &out, const ResultTable &t);

// ───────────── arka plan işi ─────────────

class Job {
public:
    using Fn = std::function<void(Job &)>;
    Job(std::string title, Fn fn);
    ~Job();
    void start();
    void cancel() { cancelled_ = true; }
    bool cancelled() const { return cancelled_; }
    const std::atomic<bool> *cancelFlag() const { return &cancelled_; }
    bool done() const { return done_; }

    void setStatus(std::string s);
    std::string status() const;
    void setProgress(double f) { progress_ = f; }  // 0…1; < 0: belirsiz
    double progress() const { return progress_; }

    const std::string title;
    ResultTable result;
    std::string error;

private:
    Fn fn_;
    std::thread thread_;
    std::atomic<bool> cancelled_{false}, done_{false};
    std::atomic<double> progress_{-1};
    mutable std::mutex m_;
    std::string status_;
};

// ───────────── araçlar ─────────────

// Sorgu sözdizimi: "*.log >100M older:1y type:f ext:mp4,mkv user:ali path:Steam".
// Sözcükler ad eşleşmesidir (joker yoksa büyük/küçük harf duyarsız alt dize).
// Hatalı sorguda err doldurulur.
ResultTable searchTree(Node *root, const std::string &query, bool apparent, std::string *err, size_t limit = 2000);
const char *searchHelp();

ResultTable findDuplicates(Node *root, Job &job, uint64_t minSize = 1024);
ResultTable findCleanup(Node *root, bool apparent, Job &job);
ResultTable ownerUsage(Node *root, bool apparent);

// pacman (Arch tabanlı dağıtımlar)
bool havePacman();
// Yolun ait olduğu paket; ait değilse boş. Hata olursa err doldurulur.
std::string packageOwner(const std::string &path, std::string *err);
std::string packageOwnerMessage(const std::string &path);
ResultTable findUnowned(Node *root, bool apparent, Job &job);

// ───────────── anlık görüntü (ncdu JSON dışa aktarım biçimi) ─────────────

// file "-" writes to standard output; a name ending in .gz is gzip-compressed.
bool saveSnapshot(const Node *root, const std::string &file, std::string *err);
// .gz / .zst / .xz uzantılı dosyalar açılmadan önce ilgili araçla çözülür.
std::unique_ptr<Tree> loadSnapshot(const std::string &file, std::string *err, int64_t *timestamp = nullptr,
                                   const std::atomic<bool> *cancel = nullptr);
// Snapshot already read into memory (JSON text).
std::unique_ptr<Tree> parseSnapshot(const std::string &data, std::string *err, int64_t *timestamp = nullptr,
                                    const std::atomic<bool> *cancel = nullptr);
ResultTable compareTrees(Node *root, Node *old, int64_t oldTime, bool apparent, size_t limit = 1000);
ResultTable compareWithSnapshot(Node *root, const std::string &file, bool apparent, Job &job);

bool exportCsv(const Node *root, const std::string &file, std::string *err);

// Bir düğümün grupları açılmış gerçek çocukları.
void realChildren(const Node *n, std::vector<Node *> &out);

// ───────────── output files and images ─────────────

// "-" is standard output; names ending in .gz are compressed. Close with closeOutput().
std::FILE *openOutput(const std::string &file, std::string *err);
bool closeOutput(std::FILE *f, std::string *err);
bool encodePng(const uint32_t *pixels, int w, int h, std::string &png);  // 0xFFRRGGBB pixels
bool writePng(const std::string &file, const uint32_t *pixels, int w, int h, std::string *err);
enum class MapShape;
std::vector<uint32_t> renderMap(Node *root, int w, int h, bool apparent, ColorMode mode, MapShape shape);
bool exportMapPng(Node *root, const std::string &file, int w, int h, bool apparent, ColorMode mode, MapShape shape,
                  std::string *err);
// Self-contained HTML page: totals, treemap and sunburst images, largest items, file types.
bool exportHtml(Node *root, const std::string &file, bool apparent, std::string *err);

// ───────────── packages (pacman, Flatpak) ─────────────

struct PackageInfo {
    std::string name, version, description;
    uint64_t size = 0;
    bool explicitlyInstalled = false;
    bool orphan = false;  // installed as a dependency that nothing needs any more
    int requiredBy = 0;
};
std::vector<PackageInfo> pacmanPackages(std::string *err);
// Installed pacman packages and Flatpak apps/runtimes by size; orphans marked.
ResultTable packageSizes(Job &job);

// ───────────── btrfs ─────────────

bool isBtrfs(const std::string &path);
// Shared (snapshots, reflinks) and compressed data under root, per top-level folder and
// per subvolume. Uses FIEMAP, so it works without root; compsize adds real compressed
// sizes when run as root.
ResultTable btrfsUsage(Node *root, Job &job);

// ───────────── remote scans over SSH ─────────────

// "host:/path", "user@host:~/dir" or "ssh://host/path". False for local paths.
bool parseRemote(const std::string &spec, std::string &host, std::string &path);
// Runs TuxDirStat 1.5+ on the remote host, or GNU find when it is missing, through
// ssh with key login. The root is named "host:/path".
std::unique_ptr<Tree> remoteScan(const std::string &spec, bool oneFs, std::string *err,
                                 const std::atomic<bool> *cancel = nullptr);
// True when file operations make sense: the tree's root is a local absolute path.
bool isLocalTree(const Node *root);

// ───────────── size history and the last scan ─────────────

std::vector<std::string> historyPaths();     // folders recorded weekly
bool historyRecording(const std::string &path);
// Adds or removes a folder and installs/enables (or disables) the systemd user timer.
bool setHistoryRecording(const std::string &path, bool on, std::string *err);
// Saves the tree as today's history entry for its root folder.
bool recordHistorySnapshot(const Node *root, std::string *err);
// --history-record: scans every recorded folder and saves an entry (used by the timer).
int recordHistory(std::ostream &log);
std::vector<std::pair<int64_t, std::string>> historyFiles(const std::string &path);  // (time, file), oldest first
// Size of each top-level folder at each recorded date, with a trend line.
ResultTable historyGrowth(Node *root, bool apparent, Job &job);
// Reopened at the next start instead of scanning again.
std::string lastScanFile();
bool saveLastScan(const Node *root, std::string *err);
