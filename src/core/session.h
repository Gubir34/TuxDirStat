#pragma once
// Arayüzlerin ortak mantığı: tarama yaşam döngüsü, ağaç sahipliği, uzantı
// tablosu ve sistem işlemleri (silme, açma, sürücü listesi).

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core.h"

struct ExtRow {
    uint32_t ext;
    uint64_t bytes;
    uint64_t count;
    RGBf color;
};

class Session {
public:
    ~Session();

    Node *root() const { return tree_ ? tree_->root : nullptr; }
    bool apparent = false;
    bool oneFs = true;
    bool grouping = false;          // çok sayıda benzer öğeyi grupla
    uint32_t groupMin = GROUP_MIN;  // grup için en az öğe sayısı

    // Gruplamayı açar/kapatır ya da eşiği değiştirir. Grup düğümleri ağaçtan ayrılabilir:
    // görünümler önce realNode() ile seçimi gerçek düğüme taşımalı, sonra kökü yeniden kurmalı.
    void setGrouping(bool on, uint32_t minCount);
    // n silindiğinde görünümden kaldırılacak satır: n tek öğesiyse boşalacak grubu döndürür.
    Node *removalTarget(Node *n) const;

    // — tarama —
    void startScan(const std::string &path, Node *target = nullptr);
    // Anlık görüntüyü (ncdu JSON) arka planda yükler; bitince poll() NewTree döndürür.
    void openSnapshot(const std::string &file);
    // pkexec ile yönetici olarak salt okunur tarama: kök süreç ağacı geçici bir anlık
    // görüntüye yazar, arayüz onu yükler. Silme işlemleri yine kullanıcı yetkisiyle yapılır.
    void startPrivilegedScan(const std::string &path);
    void cancelScan();
    bool scanning() const { return scanner_ != nullptr || loader_ != nullptr; }
    uint64_t progressCount() const;
    uint64_t progressBytes() const;
    std::string progressPath() const;
    std::string progressText() const;  // durum çubuğu için hazır metin

    enum class Result { Idle, Running, NewTree, Refresh, Cancelled, Failed };
    // Tarama bittiğinde sonucu bekletir. Arayüz eski düğümlere işaretçileri
    // bıraktıktan sonra commitNewTree() / commitRefresh() çağırır.
    Result poll();
    Node *pendingRoot() const { return pending_ ? pending_->root : nullptr; }
    Node *refreshTarget() const { return target_; }
    void commitNewTree();  // eski ağacı bırakır, uzantıları hesaplar
    void commitRefresh();  // takeChildren(target, pendingRoot()) sonrası: belleği devralır, toplamları yayar
    const std::string &error() const { return error_; }
    double elapsed() const { return elapsed_; }
    uint64_t lastCount() const { return count_; }

    // — değişiklikler —
    void remove(Node *n);  // diskten silindikten ve görünümden çıkarıldıktan sonra

    // — uzantılar —
    void recomputeExt(const std::vector<ExtStat> *precomputed = nullptr);
    std::vector<ExtRow> ext;   // boyuta göre azalan
    std::vector<RGBf> colors;  // ExtTable kimliğine göre treemap renkleri
    std::string extLabel(uint32_t ext) const;  // ".png" ya da "(yok)"

    struct Loader;

private:
    void startLoader(std::string status, std::function<std::unique_ptr<Tree>(Loader &)> fn);

    std::unique_ptr<Tree> tree_, pending_;
    std::unique_ptr<Scanner> scanner_;
    std::shared_ptr<Loader> loader_;
    std::vector<ExtStat> pendingExt_;
    Node *target_ = nullptr;
    std::string error_;
    double elapsed_ = 0;
    uint64_t count_ = 0;
};

// — sistem işlemleri —
// Programı çalıştırıp standart çıktısını toplar; çıkış kodunu (başlatılamazsa -1) döndürür.
// timeoutMs > 0 ise süre dolunca süreç sonlandırılır ve -2 döner.
int runCapture(const std::vector<std::string> &args, std::string *out, int timeoutMs = 0);
bool haveProgram(const std::string &name);  // PATH'te var mı
std::string selfExe();
bool removePath(const std::string &path, std::string *err);  // kalıcı silme; bağları izlemez
bool trashWithCommand(const std::string &path);              // gio / kioclient ile çöpe taşıma
void openPath(const std::string &path);                      // varsayılan uygulamayla aç
void revealPath(const std::string &path);                    // dosya yöneticisinde göster
uint64_t freeSpace(const std::string &path);
bool diskUsage(const std::string &path, uint64_t &total, uint64_t &avail);
std::string homeDir();
int scanWorkers();

struct Drive {
    std::string src, mp, fs;
    uint64_t total, used;
};
std::vector<Drive> listDrives();
