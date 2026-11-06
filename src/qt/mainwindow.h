#pragma once

#include <memory>
#include <unordered_map>

#include <QDialog>
#include <QMainWindow>
#include <QMimeDatabase>
#include <QTimer>

#include "report.h"
#include "session.h"
#include "tools.h"

class QAction;
class QCheckBox;
class QLabel;
class QProgressBar;
class QTableWidget;
class QTreeView;
class QTreeWidget;
class TreeModel;
class Treemap;

QIcon appIcon();
bool moveToTrash(const QString &path);

class TargetDialog : public QDialog {
    Q_OBJECT
public:
    explicit TargetDialog(QWidget *parent = nullptr);
    QString path;
    bool admin = false;  // yönetici olarak tara

private:
    void accept_();
    QTreeWidget *list_;
    QCheckBox *admin_;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const QString &path = {});
    ~MainWindow() override;

    // Testler ve sonuç penceresi için
    Node *rootNode() const { return session_.root(); }
    bool scanning() const { return poll_.isActive(); }
    TreeModel *model() const { return model_; }
    Treemap *treemap() const { return treemap_; }
    void startScan(const QString &path, Node *target = nullptr);
    void startPrivilegedScan(const QString &path);
    void openSnapshot(const QString &file);
    void selectNode(Node *n);
    void deleteNode(Node *n, bool permanent, bool confirm = true);
    // Onay sormadan çöpe taşır ve görünümlerden kaldırır; başarısızsa err doldurulur.
    bool removeQuietly(Node *n, QString *err);
    void refreshNode(Node *n);
    void zoomTo(Node *n);
    void setApparent(bool a);
    void setGrouping(bool on);
    void setGroupMin(uint32_t n);
    void setColorByAge(bool on);
    Node *selectedNode() const;
    void showTable(ResultTable t);

protected:
    void closeEvent(QCloseEvent *e) override;

private:
    QAction *act(const QString &text, const char *icon, const QKeySequence &key = {});
    void buildActions();
    void chooseTarget();
    void rescan();
    void beginBusy(const QString &title);
    void poll();
    void refreshStats();
    void updateTotals();
    void fillExtTable();
    QString extDescription(uint32_t ext);
    void showNodeStatus(Node *n);
    void onHover(Node *n);
    void zoomOut();
    void zoomInSelected();
    void updateZoomLabel();
    void showMenu(Node *n, const QPoint &globalPos);
    void copyPath(Node *n);
    void regroup(bool on, uint32_t minCount);
    void setTheme(const std::string &theme);
    void setLanguage(const std::string &lang);
    QWidget *buildHeader();
    void saveSettings();
    bool removeFromDisk(Node *n, bool permanent, std::string *err, std::string *firstFailed);
    void dropNode(Node *n);

    // araçlar
    bool toolReady();
    void runTool(const char *title, std::function<void(Job &)> fn);
    void findFiles();
    void openSnapshotDialog();
    void saveSnapshotDialog();
    void exportCsvDialog();
    void compareDialog();

    Session session_;
    Settings settings_;
    bool applyingSettings_ = false;  // ayarlar uygulanırken kaydetme
    QTimer poll_;
    QString lastQuery_;

    TreeModel *model_;
    QTreeView *treeView_;
    QTableWidget *extTable_;
    Treemap *treemap_;
    QWidget *tmBox_;
    QWidget *crumbs_;
    QLabel *title_, *subtitle_, *diskLabel_;
    QProgressBar *diskBar_;
    QLabel *statusLeft_;
    QLabel *statusRight_;
    QProgressBar *progress_;

    QAction *actStop_ = nullptr;
    QAction *actZoomOut_ = nullptr;
    QAction *actApparent_ = nullptr;
    QAction *actOneFs_ = nullptr;
    QAction *actGroup_ = nullptr;
    QAction *actTreemap_ = nullptr;
    QAction *actAge_ = nullptr;

    QMimeDatabase mime_;
    std::unordered_map<uint32_t, QString> extDesc_;
};
