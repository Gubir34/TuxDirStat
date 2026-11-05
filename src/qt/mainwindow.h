#pragma once

#include <unordered_map>

#include <QDialog>
#include <QMainWindow>
#include <QMimeDatabase>
#include <QTimer>

#include "report.h"
#include "session.h"

class QAction;
class QLabel;
class QProgressBar;
class QTableWidget;
class QTreeView;
class QTreeWidget;
class TreeModel;
class Treemap;

QIcon appIcon();

class TargetDialog : public QDialog {
    Q_OBJECT
public:
    explicit TargetDialog(QWidget *parent = nullptr);
    QString path;

private:
    void accept_();
    QTreeWidget *list_;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const QString &path = {});
    ~MainWindow() override;

    // Testler için
    Node *rootNode() const { return session_.root(); }
    bool scanning() const { return poll_.isActive(); }
    TreeModel *model() const { return model_; }
    Treemap *treemap() const { return treemap_; }
    void startScan(const QString &path, Node *target = nullptr);
    void selectNode(Node *n);
    void deleteNode(Node *n, bool permanent, bool confirm = true);
    void refreshNode(Node *n);
    void zoomTo(Node *n);
    void setApparent(bool a);
    void setGrouping(bool on);
    void setGroupMin(uint32_t n);
    Node *selectedNode() const;

protected:
    void closeEvent(QCloseEvent *e) override;

private:
    QAction *act(const QString &text, const char *icon, const QKeySequence &key = {});
    void buildActions();
    void chooseTarget();
    void rescan();
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
    QWidget *buildHeader();
    void saveSettings();

    Session session_;
    Settings settings_;
    bool applyingSettings_ = false;  // ayarlar uygulanırken kaydetme
    QTimer poll_;

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

    QMimeDatabase mime_;
    std::unordered_map<uint32_t, QString> extDesc_;
};
