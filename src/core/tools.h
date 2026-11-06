#pragma once
// Ağaç üzerinde çalışan araçlar: arama, kopya dosyalar, temizlik önerileri, anlık
// görüntüler (ncdu JSON), karşılaştırma, CSV, sahip kullanımı ve pacman denetimleri.
//
// Uzun süren araçlar bir Job içinde arka planda çalışır; sonuçları arayüzlerin tek bir
// tablo penceresiyle gösterdiği ResultTable'dır. İş sürerken arayüz ağacı değiştirmemeli
// (ilerleme penceresi kipli açılır).

#include <atomic>
#include <cstdint>
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

bool saveSnapshot(const Node *root, const std::string &file, std::string *err);
// .gz / .zst / .xz uzantılı dosyalar açılmadan önce ilgili araçla çözülür.
std::unique_ptr<Tree> loadSnapshot(const std::string &file, std::string *err, int64_t *timestamp = nullptr,
                                   const std::atomic<bool> *cancel = nullptr);
ResultTable compareTrees(Node *root, Node *old, int64_t oldTime, bool apparent, size_t limit = 1000);
ResultTable compareWithSnapshot(Node *root, const std::string &file, bool apparent, Job &job);

bool exportCsv(const Node *root, const std::string &file, std::string *err);

// Bir düğümün grupları açılmış gerçek çocukları.
void realChildren(const Node *n, std::vector<Node *> &out);
