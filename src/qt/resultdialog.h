#pragma once
// Araçların (arama, kopyalar, temizlik…) sonuç tablosunu gösteren pencere ve arka plan
// işleri için ilerleme penceresi.

#include <functional>
#include <memory>

#include <QDialog>

#include "tools.h"

class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class MainWindow;

class ResultDialog : public QDialog {
    Q_OBJECT
public:
    ResultDialog(MainWindow *mw, ResultTable table);

private:
    std::vector<int> selectedRows() const;
    void updateButtons();
    void showInTree();
    void reveal();
    void copy();
    void trash();
    void contextMenu(const QPoint &pos);

    MainWindow *mw_;
    ResultTable t_;
    std::vector<bool> gone_;  // silinen satırlar
    QTreeWidget *list_ = nullptr;
    QLabel *summary_ = nullptr;
    QPushButton *showBtn_ = nullptr, *revealBtn_ = nullptr, *copyBtn_ = nullptr, *trashBtn_ = nullptr;
};

// İşi başlatır, kipli ilerleme penceresi gösterir; iş bitince done(job) çağrılır
// (iptal edildiyse çağrılmaz).
void runJobWithProgress(QWidget *parent, std::shared_ptr<Job> job, std::function<void(Job &)> done);
