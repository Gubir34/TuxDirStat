#include "resultdialog.h"

#include <unordered_map>

#include <sys/stat.h>

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "mainwindow.h"
#include "session.h"
#include "theme.h"

namespace {

QString qs(const std::string &s)
{
    return QString::fromStdString(s);
}

constexpr int RowRole = Qt::UserRole + 1;

class SortItem : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;
    bool operator<(const QTreeWidgetItem &o) const override
    {
        const int c = treeWidget() ? treeWidget()->sortColumn() : 0;
        const QVariant a = data(c, Qt::UserRole), b = o.data(c, Qt::UserRole);
        if (a.isValid() && b.isValid())
            return a.toULongLong() < b.toULongLong();
        return QTreeWidgetItem::operator<(o);
    }
};

}  // namespace

ResultDialog::ResultDialog(MainWindow *mw, ResultTable table) : QDialog(mw), mw_(mw), t_(std::move(table))
{
    setWindowTitle("TuxDirStat — " + qs(t_.title));
    resize(980, 560);
    gone_.assign(t_.rows.size(), false);
    auto *lay = new QVBoxLayout(this);
    auto *title = new QLabel(qs(t_.title));
    QFont tf = title->font();
    tf.setPointSizeF(tf.pointSizeF() * 1.25);
    tf.setWeight(QFont::DemiBold);
    title->setFont(tf);
    lay->addWidget(title);
    summary_ = new QLabel(qs(t_.summary));
    summary_->setWordWrap(true);
    summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    summary_->setForegroundRole(QPalette::PlaceholderText);
    if (!t_.summary.empty())
        lay->addWidget(summary_);

    if (t_.rows.empty()) {
        auto *empty = new QLabel(qs(t_.emptyText));
        empty->setAlignment(Qt::AlignCenter);
        empty->setWordWrap(true);
        empty->setForegroundRole(QPalette::PlaceholderText);
        lay->addWidget(empty, 1);
    } else {
        list_ = new QTreeWidget;
        QStringList cols;
        for (const std::string &c : t_.columns)
            cols << qs(c);
        list_->setHeaderLabels(cols);
        list_->setRootIsDecorated(t_.grouped);
        list_->setAlternatingRowColors(!t_.grouped);
        list_->setUniformRowHeights(true);
        list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        list_->setContextMenuPolicy(Qt::CustomContextMenu);
        list_->setItemDelegate(new RowDelegate(list_));
        auto numeric = [this](size_t c) { return c < t_.numeric.size() && t_.numeric[c]; };
        for (size_t c = 0; c < t_.columns.size(); ++c)
            if (numeric(c))
                list_->headerItem()->setTextAlignment(int(c), Qt::AlignRight | Qt::AlignVCenter);
        std::unordered_map<int, QTreeWidgetItem *> groups;
        for (size_t i = 0; i < t_.rows.size(); ++i) {
            const ResultRow &r = t_.rows[i];
            QTreeWidgetItem *parent = nullptr;
            if (t_.grouped) {
                auto &g = groups[r.group];
                if (!g) {
                    int n = 0;
                    for (const ResultRow &o : t_.rows)
                        n += o.group == r.group;
                    g = new SortItem(list_, {QTF("Set %1 · %2 × %3", {std::to_string(r.group), std::to_string(n), fmtSize(r.bytes)})});
                    g->setFirstColumnSpanned(true);
                    g->setData(0, RowRole, -1);
                    QFont f = g->font(0);
                    f.setWeight(QFont::DemiBold);
                    g->setFont(0, f);
                    g->setFlags(Qt::ItemIsEnabled);
                    g->setExpanded(true);
                }
                parent = g;
            }
            auto *it = parent ? new SortItem(parent) : new SortItem(list_);
            for (size_t c = 0; c < r.cells.size(); ++c) {
                it->setText(int(c), qs(r.cells[c]));
                if (numeric(c)) {
                    it->setTextAlignment(int(c), Qt::AlignRight | Qt::AlignVCenter);
                    it->setData(int(c), Qt::UserRole, QVariant::fromValue<qulonglong>(r.bytes));
                }
            }
            it->setToolTip(0, qs(r.command.empty() ? r.path : r.command));
            it->setData(0, RowRole, int(i));
        }
        if (t_.grouped)
            list_->expandAll();
        // En uzun metinli (yol) sütun esner ve ortadan kısaltılır; diğerleri içeriğe göre.
        size_t stretch = 0, best = 0;
        for (size_t c = 0; c < t_.columns.size(); ++c) {
            if (numeric(c))
                continue;
            size_t len = 0;
            for (const ResultRow &r : t_.rows)
                len += c < r.cells.size() ? r.cells[c].size() : 0;
            if (len > best) {
                best = len;
                stretch = c;
            }
        }
        list_->setTextElideMode(Qt::ElideMiddle);
        QHeaderView *h = list_->header();
        h->setStretchLastSection(false);
        for (size_t c = 0; c < t_.columns.size(); ++c)
            h->setSectionResizeMode(int(c), c == stretch ? QHeaderView::Stretch : QHeaderView::ResizeToContents);
        if (!t_.grouped) {
            // Satırlar çekirdekte zaten sıralı (çoğunlukla boyuta göre); sıralama göstergesi boş başlar.
            h->setSortIndicator(-1, Qt::AscendingOrder);
            list_->setSortingEnabled(true);
        }
        connect(list_, &QTreeWidget::itemSelectionChanged, this, &ResultDialog::updateButtons);
        connect(list_, &QTreeWidget::itemDoubleClicked, this, [this] { showInTree(); });
        connect(list_, &QWidget::customContextMenuRequested, this, &ResultDialog::contextMenu);
        lay->addWidget(list_, 1);
    }

    auto *btns = new QDialogButtonBox(QDialogButtonBox::Close);
    btns->button(QDialogButtonBox::Close)->setText(QT("Close"));
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    if (list_) {
        showBtn_ = btns->addButton(QT("Show in tree"), QDialogButtonBox::ActionRole);
        revealBtn_ = btns->addButton(QT("Open folder"), QDialogButtonBox::ActionRole);
        copyBtn_ = btns->addButton(QT("Copy"), QDialogButtonBox::ActionRole);
        trashBtn_ = btns->addButton(QT("Move to trash"), QDialogButtonBox::ActionRole);
        showBtn_->setIcon(QIcon::fromTheme("go-jump"));
        revealBtn_->setIcon(QIcon::fromTheme("system-file-manager"));
        copyBtn_->setIcon(QIcon::fromTheme("edit-copy"));
        trashBtn_->setIcon(QIcon::fromTheme("user-trash"));
        connect(showBtn_, &QPushButton::clicked, this, &ResultDialog::showInTree);
        connect(revealBtn_, &QPushButton::clicked, this, &ResultDialog::reveal);
        connect(copyBtn_, &QPushButton::clicked, this, &ResultDialog::copy);
        connect(trashBtn_, &QPushButton::clicked, this, &ResultDialog::trash);
        updateButtons();
    }
    lay->addWidget(btns);
}

std::vector<int> ResultDialog::selectedRows() const
{
    std::vector<int> v;
    if (!list_)
        return v;
    for (QTreeWidgetItem *it : list_->selectedItems()) {
        const int r = it->data(0, RowRole).toInt();
        if (r >= 0 && !gone_[size_t(r)])
            v.push_back(r);
    }
    return v;
}

void ResultDialog::updateButtons()
{
    const std::vector<int> rows = selectedRows();
    const ResultRow *one = rows.size() == 1 ? &t_.rows[size_t(rows[0])] : nullptr;
    showBtn_->setEnabled(one && one->node && mw_->rootNode() && one->node->isWithin(mw_->rootNode()));
    revealBtn_->setEnabled(one && !one->path.empty() && one->path[0] == '/');
    copyBtn_->setEnabled(!rows.empty());
    copyBtn_->setText(one && !one->command.empty() ? QT("Copy command") : QT("Copy path"));
    bool del = !rows.empty();
    for (int r : rows)
        del = del && t_.rows[size_t(r)].deletable && !t_.rows[size_t(r)].path.empty();
    trashBtn_->setEnabled(del);
}

void ResultDialog::showInTree()
{
    const std::vector<int> rows = selectedRows();
    if (rows.size() != 1)
        return;
    Node *n = t_.rows[size_t(rows[0])].node;
    if (!n || !mw_->rootNode() || !n->isWithin(mw_->rootNode()))
        return;
    mw_->selectNode(n);
    accept();
}

void ResultDialog::reveal()
{
    const std::vector<int> rows = selectedRows();
    if (rows.size() == 1)
        revealPath(t_.rows[size_t(rows[0])].path);
}

void ResultDialog::copy()
{
    QStringList lines;
    for (int r : selectedRows()) {
        const ResultRow &row = t_.rows[size_t(r)];
        lines << qs(row.command.empty() ? row.path : row.command);
    }
    QApplication::clipboard()->setText(lines.join('\n'));
}

void ResultDialog::trash()
{
    const std::vector<int> rows = selectedRows();
    if (rows.empty())
        return;
    uint64_t bytes = 0;
    for (int r : rows)
        bytes += t_.rows[size_t(r)].bytes;
    QString q = QTF("Move %1 items (%2) to the trash?", {fmtInt(rows.size()), fmtSize(bytes)});
    const std::string warn = deletionWarning(t_, rows);
    if (!warn.empty())
        q = "<b>" + qs(warn).toHtmlEscaped() + "</b><br><br>" + q;
    if (QMessageBox::question(this, "TuxDirStat", q, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    int failed = 0;
    QString firstErr;
    for (int r : rows) {
        const ResultRow &row = t_.rows[size_t(r)];
        struct stat st;
        bool ok;
        QString err;
        if (::lstat(row.path.c_str(), &st) != 0) {
            ok = true;  // daha önce silinen bir klasörün içindeydi
        } else if (row.node && mw_->rootNode() && row.node->isWithin(mw_->rootNode())) {
            ok = mw_->removeQuietly(row.node, &err);
        } else {
            ok = moveToTrash(qs(row.path));
            if (!ok)
                err = qs(row.path);
        }
        if (ok)
            gone_[size_t(r)] = true;
        else if (!failed++)
            firstErr = err;
    }
    // Silinen satırları kaldır; boşalan kopya kümelerini de.
    for (QTreeWidgetItemIterator it(list_); *it;) {
        QTreeWidgetItem *item = *it;
        ++it;
        const int r = item->data(0, RowRole).toInt();
        if (r >= 0 && gone_[size_t(r)])
            delete item;
    }
    for (int i = list_->topLevelItemCount(); i-- > 0;) {
        QTreeWidgetItem *g = list_->topLevelItem(i);
        if (g->data(0, RowRole).toInt() < 0 && g->childCount() < 2)
            delete g;
    }
    if (failed)
        QMessageBox::warning(this, "TuxDirStat", QTF("%1 items could not be moved to the trash.", {std::to_string(failed)}) + "\n\n" + firstErr);
    updateButtons();
}

void ResultDialog::contextMenu(const QPoint &pos)
{
    if (!list_->itemAt(pos))
        return;
    updateButtons();
    QMenu m(this);
    for (QPushButton *b : {showBtn_, revealBtn_, copyBtn_, trashBtn_}) {
        QAction *a = m.addAction(b->icon(), b->text(), b, &QPushButton::click);
        a->setEnabled(b->isEnabled());
    }
    m.exec(list_->viewport()->mapToGlobal(pos));
}

void runJobWithProgress(QWidget *parent, std::shared_ptr<Job> job, std::function<void(Job &)> done)
{
    auto *dlg = new QProgressDialog(QString::fromStdString(job->title), QT("Cancel"), 0, 1000, parent);
    dlg->setWindowTitle("TuxDirStat — " + QString::fromStdString(job->title));
    dlg->setWindowModality(Qt::WindowModal);
    dlg->setMinimumDuration(0);
    dlg->setMinimumWidth(520);
    dlg->setAutoClose(false);
    dlg->setAutoReset(false);
    dlg->setValue(0);
    auto *timer = new QTimer(dlg);
    QObject::connect(dlg, &QProgressDialog::canceled, dlg, [job] { job->cancel(); });
    QObject::connect(timer, &QTimer::timeout, dlg, [dlg, timer, job, done] {
        const double p = job->progress();
        if (p < 0) {
            dlg->setMaximum(0);
        } else {
            dlg->setMaximum(1000);
            dlg->setValue(int(p * 1000));
        }
        dlg->setLabelText(QString::fromStdString(job->status()));
        if (!job->done())
            return;
        timer->stop();
        dlg->deleteLater();
        if (!job->cancelled())
            done(*job);
    });
    timer->start(100);
    dlg->show();
    job->start();
}
