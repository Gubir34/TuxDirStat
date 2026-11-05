#include "mainwindow.h"

#include <algorithm>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QRadialGradient>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTableWidget>
#include <QToolBar>
#include <QTreeView>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "extinfo.h"
#include "theme.h"
#include "treemap.h"
#include "treemodel.h"

namespace {

QString qs(const std::string &s)
{
    return QString::fromStdString(s);
}

class NumItem : public QTableWidgetItem {
public:
    NumItem(const QString &text, double v) : QTableWidgetItem(text), val(v)
    {
        setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }
    bool operator<(const QTableWidgetItem &o) const override
    {
        return val < static_cast<const NumItem &>(o).val;
    }
    double val;
};

QIcon swatchIcon(const RGBf &c, int size = 16)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QColor col(std::min(255, int(c.r * 1.4f)), std::min(255, int(c.g * 1.4f)), std::min(255, int(c.b * 1.4f)));
    QRadialGradient g(size * 0.35, size * 0.35, size * 0.8);
    g.setColorAt(0, col.lighter(150));
    g.setColorAt(1, col.darker(160));
    p.setBrush(g);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(1, 1, size - 2, size - 2, 3, 3);
    return QIcon(pm);
}

bool moveToTrash(const QString &path)
{
    return QFile::moveToTrash(path) || trashWithCommand(path.toStdString());
}

}  // namespace

QIcon appIcon()
{
    QPixmap pm(64, 64);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    const QColor cols[] = {{40, 90, 255}, {235, 40, 40}, {40, 200, 60}, {240, 220, 40}, {40, 210, 220}};
    const QRect rects[] = {{2, 2, 36, 60}, {40, 2, 22, 30}, {40, 34, 12, 28}, {54, 34, 8, 16}, {54, 52, 8, 10}};
    for (int i = 0; i < 5; ++i) {
        const QRect &r = rects[i];
        QRadialGradient g(r.x() + r.width() * 0.3, r.y() + r.height() * 0.3, std::max(r.width(), r.height()));
        g.setColorAt(0, cols[i].lighter(140));
        g.setColorAt(1, cols[i].darker(170));
        p.fillRect(r, g);
    }
    return QIcon(pm);
}

// ───────────────────────────── Konum seçici ─────────────────────────────

TargetDialog::TargetDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle("TuxDirStat — Taranacak konum");
    resize(720, 400);
    auto *lay = new QVBoxLayout(this);
    lay->addWidget(new QLabel("Taramak istediğiniz konumu seçin:"));
    list_ = new QTreeWidget;
    list_->setHeaderLabels({"Konum", "Tür", "Kullanılan", "Toplam", "Doluluk"});
    list_->setRootIsDecorated(false);
    list_->setAlternatingRowColors(true);
    QStyle *st = style();

    const QString home = QDir::homePath();
    auto *it = new QTreeWidgetItem({QString("Ev klasörü  (%1)").arg(home)});
    it->setIcon(0, QIcon::fromTheme("user-home", st->standardIcon(QStyle::SP_DirHomeIcon)));
    it->setData(0, Qt::UserRole, home);
    list_->addTopLevelItem(it);

    for (const Drive &d : listDrives()) {
        auto *item = new QTreeWidgetItem({qs(d.mp), qs(d.fs), qs(fmtSize(d.used)), qs(fmtSize(d.total)), ""});
        item->setToolTip(0, qs(d.src));
        item->setIcon(0, QIcon::fromTheme("drive-harddisk", st->standardIcon(QStyle::SP_DriveHDIcon)));
        item->setData(0, Qt::UserRole, qs(d.mp));
        item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        item->setTextAlignment(3, Qt::AlignRight | Qt::AlignVCenter);
        list_->addTopLevelItem(item);
        auto *bar = new QProgressBar;
        bar->setRange(0, 100);
        bar->setValue(int(d.used * 100 / d.total));
        bar->setMaximumHeight(18);
        list_->setItemWidget(item, 4, bar);
    }
    list_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    list_->setCurrentItem(list_->topLevelItem(0));
    connect(list_, &QTreeWidget::itemDoubleClicked, this, [this] { accept_(); });
    lay->addWidget(list_);

    auto *btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    btns->button(QDialogButtonBox::Ok)->setText("Tara");
    btns->button(QDialogButtonBox::Cancel)->setText("İptal");
    auto *pick = btns->addButton("Klasör seç…", QDialogButtonBox::ActionRole);
    connect(pick, &QPushButton::clicked, this, [this] {
        QString d = QFileDialog::getExistingDirectory(this, "Klasör seç", QDir::homePath());
        if (!d.isEmpty()) {
            path = d;
            accept();
        }
    });
    connect(btns, &QDialogButtonBox::accepted, this, [this] { accept_(); });
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(btns);
}

void TargetDialog::accept_()
{
    if (auto *it = list_->currentItem()) {
        path = it->data(0, Qt::UserRole).toString();
        accept();
    }
}

// ───────────────────────────── Ana pencere ─────────────────────────────

MainWindow::MainWindow(const QString &path)
{
    setWindowTitle("TuxDirStat");
    setWindowIcon(appIcon());
    resize(1280, 860);

    // Ağaç
    model_ = new TreeModel(this);
    treeView_ = new QTreeView;
    treeView_->setModel(model_);
    treeView_->setUniformRowHeights(true);
    treeView_->setAlternatingRowColors(true);
    treeView_->setSortingEnabled(true);
    treeView_->setSelectionMode(QAbstractItemView::SingleSelection);
    treeView_->setContextMenuPolicy(Qt::CustomContextMenu);
    treeView_->setItemDelegate(new RowDelegate(treeView_));
    treeView_->setItemDelegateForColumn(TreeModel::Bar, new BarDelegate(treeView_));
    treeView_->setIconSize(QSize(18, 18));
    QHeaderView *hdr = treeView_->header();
    hdr->setSortIndicator(TreeModel::Size, Qt::DescendingOrder);
    hdr->setStretchLastSection(false);
    hdr->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    const int widths[] = {340, 130, 70, 90, 80, 80, 70, 145};
    for (int c = 0; c < TreeModel::NCols; ++c)
        hdr->resizeSection(c, widths[c]);
    connect(treeView_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex &cur) {
        Node *n = cur.isValid() ? TreeModel::nodeAt(cur) : nullptr;
        treemap_->setSelected(n);
        if (n)
            showNodeStatus(n);
    });
    connect(treeView_, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QModelIndex i = treeView_->indexAt(pos);
        if (i.isValid())
            showMenu(TreeModel::nodeAt(i), treeView_->viewport()->mapToGlobal(pos));
    });
    connect(treeView_, &QTreeView::doubleClicked, this, [](const QModelIndex &i) {
        Node *n = TreeModel::nodeAt(i);
        if (!n->isDir)
            QDesktopServices::openUrl(QUrl::fromLocalFile(qs(n->path())));
    });

    // Uzantılar
    extTable_ = new QTableWidget(0, 5);
    extTable_->setHorizontalHeaderLabels({"Uzantı", "Tür", "Boyut", "%", "Dosya"});
    extTable_->verticalHeader()->hide();
    extTable_->verticalHeader()->setDefaultSectionSize(26);
    extTable_->setItemDelegate(new RowDelegate(extTable_));
    extTable_->setItemDelegateForColumn(3, new ShareDelegate(extTable_));
    extTable_->setIconSize(QSize(14, 14));
    extTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    extTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    extTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    extTable_->setShowGrid(false);
    extTable_->setAlternatingRowColors(true);
    QHeaderView *eh = extTable_->horizontalHeader();
    eh->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    for (int c : {2, 3, 4})
        extTable_->horizontalHeaderItem(c)->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    eh->setSectionResizeMode(1, QHeaderView::Stretch);
    eh->resizeSection(0, 90);
    eh->resizeSection(2, 80);
    eh->resizeSection(3, 60);
    eh->resizeSection(4, 70);
    connect(extTable_, &QTableWidget::itemSelectionChanged, this, [this] {
        const auto rows = extTable_->selectionModel()->selectedRows();
        treemap_->setHighlight(rows.isEmpty() ? -1 : extTable_->item(rows[0].row(), 0)->data(Qt::UserRole).toInt());
    });

    // Treemap
    treemap_ = new Treemap;
    connect(treemap_, &Treemap::nodeClicked, this, &MainWindow::selectNode);
    connect(treemap_, &Treemap::nodeHovered, this, &MainWindow::onHover);
    connect(treemap_, &Treemap::zoomRequested, this, &MainWindow::zoomTo);
    connect(treemap_, &Treemap::contextRequested, this, &MainWindow::showMenu);
    crumbs_ = new QWidget;
    auto *cl = new QHBoxLayout(crumbs_);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(0);
    tmBox_ = new QWidget;
    auto *tl = new QVBoxLayout(tmBox_);
    tl->setContentsMargins(0, 4, 0, 0);
    tl->setSpacing(4);
    tl->addWidget(crumbs_);
    tl->addWidget(treemap_, 1);

    auto *top = new QSplitter(Qt::Horizontal);
    top->addWidget(treeView_);
    top->addWidget(extTable_);
    top->setStretchFactor(0, 3);
    top->setStretchFactor(1, 1);
    top->setSizes({820, 460});
    auto *mainSplit = new QSplitter(Qt::Vertical);
    mainSplit->addWidget(top);
    mainSplit->addWidget(tmBox_);
    mainSplit->setSizes({450, 410});
    mainSplit->setHandleWidth(8);
    top->setHandleWidth(8);
    auto *central = new QWidget;
    auto *vl = new QVBoxLayout(central);
    vl->setContentsMargins(8, 6, 8, 4);
    vl->setSpacing(8);
    vl->addWidget(buildHeader());
    vl->addWidget(mainSplit, 1);
    setCentralWidget(central);

    // Durum çubuğu
    statusLeft_ = new QLabel;
    statusLeft_->setMinimumWidth(100);
    progress_ = new QProgressBar;
    progress_->setRange(0, 0);
    progress_->setMaximumWidth(160);
    progress_->setMaximumHeight(16);
    progress_->hide();
    statusRight_ = new QLabel;
    statusRight_->setForegroundRole(QPalette::PlaceholderText);
    statusBar()->addWidget(statusLeft_, 1);
    statusBar()->addPermanentWidget(progress_);
    statusBar()->addPermanentWidget(statusRight_);

    settings_ = Settings::load();
    buildActions();
    poll_.setInterval(100);
    connect(&poll_, &QTimer::timeout, this, &MainWindow::poll);
    updateZoomLabel();
    updateTotals();

    if (!path.isEmpty())
        QTimer::singleShot(0, this, [this, path] { startScan(path); });
    else
        QTimer::singleShot(0, this, &MainWindow::chooseTarget);
}

MainWindow::~MainWindow()
{
    session_.cancelScan();
    treemap_->setRoot(nullptr);
    model_->setRoot(nullptr);
}

// ───────────── eylemler ─────────────

QAction *MainWindow::act(const QString &text, const char *icon, const QKeySequence &key)
{
    auto *a = new QAction(QIcon::fromTheme(icon), text, this);
    if (!key.isEmpty())
        a->setShortcut(key);
    addAction(a);
    return a;
}

void MainWindow::buildActions()
{
    QAction *actOpen = act("Aç…", "document-open", QKeySequence("Ctrl+O"));
    connect(actOpen, &QAction::triggered, this, &MainWindow::chooseTarget);
    QAction *actRescan = act("Yeniden tara", "view-refresh", QKeySequence("F5"));
    connect(actRescan, &QAction::triggered, this, &MainWindow::rescan);
    actStop_ = act("Durdur", "process-stop", QKeySequence("Esc"));
    actStop_->setEnabled(false);
    connect(actStop_, &QAction::triggered, this, [this] { session_.cancelScan(); });

    QAction *actZoomIn = act("Yakınlaştır", "zoom-in", QKeySequence("Ctrl++"));
    connect(actZoomIn, &QAction::triggered, this, &MainWindow::zoomInSelected);
    actZoomOut_ = act("Uzaklaştır", "zoom-out", QKeySequence("Backspace"));
    connect(actZoomOut_, &QAction::triggered, this, &MainWindow::zoomOut);
    QAction *actZoomReset = act("Kök", "zoom-original", QKeySequence("Ctrl+0"));
    connect(actZoomReset, &QAction::triggered, this, [this] { zoomTo(rootNode()); });

    actApparent_ = act("Görünür boyut", "view-statistics");
    actApparent_->setCheckable(true);
    actApparent_->setToolTip("Açık: dosyaların görünür boyutu (ls)\nKapalı: diskte kapladığı alan (du)");
    connect(actApparent_, &QAction::toggled, this, &MainWindow::setApparent);
    actOneFs_ = act("Tek dosya sistemi", "drive-harddisk");
    actOneFs_->setCheckable(true);
    actOneFs_->setChecked(true);
    actOneFs_->setToolTip("Başka disklere/bağlama noktalarına geçme\n(aynı aygıttaki btrfs alt birimleri dahil edilir)");
    connect(actOneFs_, &QAction::toggled, this, [this](bool on) {
        session_.oneFs = on;
        saveSettings();
    });
    actGroup_ = act("Dosyaları grupla", "view-group", QKeySequence("Ctrl+G"));
    actGroup_->setCheckable(true);
    actGroup_->setToolTip("Bir klasördeki çok sayıda benzer öğeyi (oyun asset'leri gibi) tek kutuda topla:\n"
                          "aynı uzantılı dosyalar, küçük dosyalar ve küçük alt klasörler");
    connect(actGroup_, &QAction::toggled, this, &MainWindow::setGrouping);
    actTreemap_ = act("Treemap", "view-split-top-bottom", QKeySequence("F9"));
    QAction *actTreemap = actTreemap_;
    actTreemap->setCheckable(true);
    actTreemap->setChecked(true);
    connect(actTreemap, &QAction::toggled, this, [this](bool on) {
        tmBox_->setVisible(on);
        saveSettings();
    });

    QAction *actCopy = act("Yolu kopyala", "edit-copy", QKeySequence("Ctrl+C"));
    connect(actCopy, &QAction::triggered, this, [this] {
        if (Node *n = selectedNode())
            copyPath(n);
    });
    QAction *actTrash = act("Çöp kutusuna taşı", "user-trash", QKeySequence("Delete"));
    connect(actTrash, &QAction::triggered, this, [this] {
        if (Node *n = selectedNode())
            deleteNode(n, false);
    });
    QAction *actDelete = act("Kalıcı olarak sil…", "edit-delete", QKeySequence("Shift+Delete"));
    connect(actDelete, &QAction::triggered, this, [this] {
        if (Node *n = selectedNode())
            deleteNode(n, true);
    });
    QAction *actQuit = act("Çıkış", "application-exit", QKeySequence("Ctrl+Q"));
    connect(actQuit, &QAction::triggered, this, &QWidget::close);

    QToolBar *tb = addToolBar("Araçlar");
    tb->setObjectName("main_toolbar");
    tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    tb->setIconSize(QSize(18, 18));
    tb->setMovable(false);
    for (QAction *a : {actZoomIn, actZoomOut_, actZoomReset})
        a->setIconText(QString());  // araç çubuğunda yalnızca simge; ipucu kalır
    auto fill = [](QWidget *w, std::initializer_list<QAction *> acts) {
        for (QAction *a : acts) {
            if (!a) {
                auto *sep = new QAction(w);
                sep->setSeparator(true);
                w->addAction(sep);
            } else {
                w->addAction(a);
            }
        }
    };
    fill(tb, {actOpen, actRescan, actStop_, nullptr, actZoomIn, actZoomOut_, actZoomReset, nullptr,
              actApparent_, actOneFs_, actGroup_, actTreemap});
    fill(menuBar()->addMenu("&Dosya"), {actOpen, actRescan, actStop_, nullptr, actQuit});
    fill(menuBar()->addMenu("Dü&zen"), {actCopy, nullptr, actTrash, actDelete});
    QMenu *view = menuBar()->addMenu("&Görünüm");
    fill(view, {actZoomIn, actZoomOut_, actZoomReset, nullptr, actApparent_, actOneFs_, actGroup_});
    QMenu *thr = view->addMenu("Gruplama eşiği");
    auto *thrGroup = new QActionGroup(this);
    for (uint32_t n : GROUP_MIN_CHOICES) {
        QAction *a = thr->addAction(QString("En az %1 öğe").arg(n));
        a->setCheckable(true);
        a->setChecked(n == settings_.groupMin);
        thrGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, n] { setGroupMin(n); });
    }
    fill(view, {nullptr, actTreemap});
    QMenu *themeMenu = view->addMenu("Tema");
    auto *themeGroup = new QActionGroup(this);
    for (auto [id, label] : {std::pair{"system", "Sistem"}, {"light", "Açık"}, {"dark", "Koyu"}}) {
        QAction *a = themeMenu->addAction(label);
        a->setCheckable(true);
        a->setChecked(settings_.theme == id);
        themeGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, t = std::string(id)] { setTheme(t); });
    }
    // Kayıtlı ayarları uygula (kök yokken yalnızca durumları kurar).
    applyingSettings_ = true;
    session_.groupMin = settings_.groupMin;
    actApparent_->setChecked(settings_.apparent);
    actOneFs_->setChecked(settings_.oneFs);
    actGroup_->setChecked(settings_.grouping);
    actTreemap->setChecked(settings_.showTreemap);
    applyingSettings_ = false;

    QMenu *help = menuBar()->addMenu("&Yardım");
    help->addAction("Hakkında", this, [this] {
        QMessageBox::about(this, "TuxDirStat hakkında",
                           "<b>TuxDirStat</b><br>Linux için disk kullanım analizörü.<br><br>"
                           "Ağaçta veya treemap'te bir öğeye tıklayın; treemap'te çift tıklayarak "
                           "klasöre yakınlaşın, Backspace ile geri çıkın. Uzantı listesinde bir satır "
                           "seçmek o türdeki dosyaları treemap'te işaretler.");
    });
}

// ───────────── tarama ─────────────

void MainWindow::chooseTarget()
{
    TargetDialog dlg(this);
    if (dlg.exec() == QDialog::Accepted && !dlg.path.isEmpty())
        startScan(dlg.path);
}

void MainWindow::startScan(const QString &path, Node *target)
{
    const QString abs = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    session_.startScan(abs.toStdString(), target);
    progress_->show();
    actStop_->setEnabled(true);
    poll_.start();
    if (!target)
        setWindowTitle("TuxDirStat — " + abs);
}

void MainWindow::rescan()
{
    if (rootNode())
        startScan(qs(rootNode()->path()));
    else
        chooseTarget();
}

void MainWindow::poll()
{
    const Session::Result res = session_.poll();
    if (res == Session::Result::Running) {
        const QString cur = statusLeft_->fontMetrics().elidedText(qs(session_.progressPath()), Qt::ElideMiddle, 600);
        statusLeft_->setText(QString("Taranıyor… %1 öğe · %2 · %3")
                                 .arg(qs(fmtInt(session_.progressCount())), qs(fmtSize(session_.progressBytes())), cur));
        return;
    }
    poll_.stop();
    progress_->hide();
    actStop_->setEnabled(false);
    switch (res) {
    case Session::Result::Failed:
        QMessageBox::warning(this, "TuxDirStat", "Tarama başarısız:\n" + qs(session_.error()));
        statusLeft_->clear();
        return;
    case Session::Result::Cancelled:
        statusLeft_->setText("Tarama durduruldu.");
        return;
    case Session::Result::NewTree: {
        Node *root = session_.pendingRoot();
        treemap_->setSelected(nullptr);
        treemap_->setRoot(root);
        model_->setRoot(root);
        session_.commitNewTree();
        treeView_->expand(model_->index(0, 0));
        treeView_->setCurrentIndex(model_->index(0, 0));
        break;
    }
    case Session::Result::Refresh: {
        Node *target = session_.refreshTarget();
        treemap_->invalidate();
        if (treemap_->root() && treemap_->root() != target && treemap_->root()->isWithin(target))
            treemap_->setRoot(target);
        if (treemap_->selected() && treemap_->selected() != target && treemap_->selected()->isWithin(target))
            treemap_->setSelected(target);
        model_->replaceChildren(target, session_.pendingRoot());
        session_.commitRefresh();
        treeView_->expand(model_->indexFor(target));
        treeView_->viewport()->update();
        break;
    }
    default:
        return;
    }
    fillExtTable();
    updateTotals();
    updateZoomLabel();
    statusLeft_->setText(QString("Tarama tamamlandı: %1 öğe, %2 sn.")
                             .arg(qs(fmtInt(session_.lastCount())), QString::number(session_.elapsed(), 'f', 2).replace('.', ',')));
}

void MainWindow::refreshStats()
{
    session_.recomputeExt();
    fillExtTable();
    updateTotals();
}

QWidget *MainWindow::buildHeader()
{
    auto *box = new QWidget;
    auto *hl = new QHBoxLayout(box);
    hl->setContentsMargins(4, 2, 4, 2);
    hl->setSpacing(16);
    auto *icon = new QLabel;
    icon->setPixmap(appIcon().pixmap(36, 36));
    hl->addWidget(icon);
    auto *left = new QVBoxLayout;
    left->setSpacing(1);
    title_ = new QLabel;
    QFont tf = title_->font();
    tf.setPointSizeF(tf.pointSizeF() * 1.35);
    tf.setWeight(QFont::DemiBold);
    title_->setFont(tf);
    title_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    subtitle_ = new QLabel;
    subtitle_->setForegroundRole(QPalette::PlaceholderText);
    left->addWidget(title_);
    left->addWidget(subtitle_);
    hl->addLayout(left, 1);
    auto *right = new QVBoxLayout;
    right->setSpacing(4);
    diskLabel_ = new QLabel;
    diskLabel_->setAlignment(Qt::AlignRight);
    diskLabel_->setForegroundRole(QPalette::PlaceholderText);
    diskBar_ = new QProgressBar;
    diskBar_->setRange(0, 1000);
    diskBar_->setTextVisible(false);
    diskBar_->setFixedSize(260, 8);
    right->addWidget(diskLabel_);
    right->addWidget(diskBar_, 0, Qt::AlignRight);
    hl->addLayout(right);
    return box;
}

void MainWindow::updateTotals()
{
    Node *root = rootNode();
    if (!root) {
        title_->setText("Henüz tarama yapılmadı");
        subtitle_->setText("Bir konum seçmek için Aç… (Ctrl+O)");
        diskLabel_->clear();
        diskBar_->hide();
        return;
    }
    const std::string path = root->path();
    title_->setText(title_->fontMetrics().elidedText(qs(path), Qt::ElideMiddle, 700));
    title_->setToolTip(qs(path));
    subtitle_->setText(QString("%1 · %2 dosya · %3 klasör%4")
                           .arg(qs(fmtSize(root->sizeOf(session_.apparent))), qs(fmtInt(root->files)),
                                qs(fmtInt(root->dirs)),
                                session_.apparent ? QString(" · görünür boyut") : QString(" · diskte kapladığı alan")));
    uint64_t total = 0, avail = 0;
    if (diskUsage(path, total, avail)) {
        const double used = double(total - avail) / double(total);
        diskLabel_->setText(QString("Disk: %1 boş / %2 (%3 dolu)")
                                .arg(qs(fmtSize(avail)), qs(fmtSize(total)), qs(fmtPct(used))));
        diskBar_->setValue(int(used * 1000));
        diskBar_->show();
    }
}

// ───────────── uzantılar ─────────────

QString MainWindow::extDescription(uint32_t ext)
{
    auto it = extDesc_.find(ext);
    if (it != extDesc_.end())
        return it->second;
    std::string sys;
    if (ext) {
        QMimeType mt = mime_.mimeTypeForFile("x." + qs(ExtTable::name(ext)), QMimeDatabase::MatchExtension);
        if (!mt.isDefault())
            sys = mt.comment().toStdString();
    }
    const QString d = qs(describeExt(ext, sys));
    extDesc_.emplace(ext, d);
    return d;
}

void MainWindow::fillExtTable()
{
    Node *root = rootNode();
    const double total = root ? std::max<uint64_t>(1, root->sizeOf(session_.apparent)) : 1;
    QTableWidget *t = extTable_;
    t->blockSignals(true);
    t->setSortingEnabled(false);
    t->clearContents();
    t->setRowCount(int(session_.ext.size()));
    for (int r = 0; r < int(session_.ext.size()); ++r) {
        const ExtRow &e = session_.ext[r];
        auto *it = new QTableWidgetItem(qs(session_.extLabel(e.ext)));
        it->setIcon(swatchIcon(e.color));
        it->setData(Qt::UserRole, int(e.ext));
        t->setItem(r, 0, it);
        const QString desc = extDescription(e.ext);
        it->setToolTip(desc);
        auto *descItem = new QTableWidgetItem(desc);
        descItem->setToolTip(desc);
        t->setItem(r, 1, descItem);
        t->setItem(r, 2, new NumItem(qs(fmtSize(e.bytes)), double(e.bytes)));
        auto *pct = new NumItem(qs(fmtPct(e.bytes / total)), double(e.bytes));
        pct->setData(ShareDelegate::ShareRole, e.bytes / total);
        t->setItem(r, 3, pct);
        t->setItem(r, 4, new NumItem(qs(fmtInt(e.count)), double(e.count)));
    }
    t->setSortingEnabled(true);
    t->sortItems(2, Qt::DescendingOrder);
    t->blockSignals(false);
    treemap_->setHighlight(-1);
    treemap_->setColors(session_.colors);
}

// ───────────── seçim ve yakınlaştırma ─────────────

Node *MainWindow::selectedNode() const
{
    QModelIndex i = treeView_->currentIndex();
    return i.isValid() ? TreeModel::nodeAt(i) : nullptr;
}

void MainWindow::selectNode(Node *n)
{
    QModelIndex i = model_->indexFor(n);
    if (i.isValid()) {
        treeView_->setCurrentIndex(i);
        treeView_->scrollTo(i);
    }
}

void MainWindow::showNodeStatus(Node *n)
{
    statusLeft_->setText(qs(n->path()) + "  —  " + qs(fmtSize(n->sizeOf(session_.apparent))));
}

void MainWindow::onHover(Node *n)
{
    if (session_.scanning())
        return;
    if (!n)
        n = selectedNode();
    if (n)
        showNodeStatus(n);
}

void MainWindow::zoomTo(Node *n)
{
    if (!n || !n->isDir)
        return;
    treemap_->setRoot(n);
    updateZoomLabel();
}

void MainWindow::zoomInSelected()
{
    if (Node *n = selectedNode())
        zoomTo(n->isDir ? n : n->parent);
}

void MainWindow::zoomOut()
{
    Node *r = treemap_->root();
    if (r && r->parent)
        zoomTo(r->parent);
}

// Treemap'in üstündeki tıklanabilir konum çubuğu: kökten yakınlaşılan klasöre kadar.
void MainWindow::updateZoomLabel()
{
    Node *r = treemap_->root();
    actZoomOut_->setEnabled(r && r->parent);
    auto *lay = static_cast<QHBoxLayout *>(crumbs_->layout());
    while (QLayoutItem *it = lay->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    auto *lbl = new QLabel("Treemap");
    lbl->setForegroundRole(QPalette::PlaceholderText);
    lbl->setContentsMargins(2, 0, 8, 0);
    lay->addWidget(lbl);
    std::vector<Node *> chain;
    for (Node *n = r; n; n = n->parent)
        chain.push_back(n);
    const size_t maxShown = 7;
    for (size_t i = chain.size(); i-- > 0;) {
        Node *n = chain[i];
        const bool last = i == 0;
        if (chain.size() > maxShown && i < chain.size() - 1 && i >= maxShown - 1) {
            if (i == chain.size() - 2) {
                auto *dots = new QLabel("…");
                lay->addWidget(dots);
            }
            continue;
        }
        auto *b = new QToolButton;
        b->setAutoRaise(true);
        const QString name = n->parent ? QString::fromStdString(displayName(n, false)) : qs(n->path());
        b->setText(name);
        b->setToolTip(qs(n->path()));
        if (last) {
            QFont f = b->font();
            f.setWeight(QFont::DemiBold);
            b->setFont(f);
        }
        connect(b, &QToolButton::clicked, this, [this, n] { zoomTo(n); });
        lay->addWidget(b);
        if (!last) {
            auto *sep = new QLabel("›");
            sep->setForegroundRole(QPalette::PlaceholderText);
            lay->addWidget(sep);
        }
    }
    lay->addStretch(1);
}

void MainWindow::setTheme(const std::string &theme)
{
    applyTheme(theme);
    settings_.theme = theme;
    saveSettings();
    treeView_->viewport()->update();
    extTable_->viewport()->update();
    fillExtTable();
}

void MainWindow::setApparent(bool a)
{
    if (actApparent_->isChecked() != a) {
        actApparent_->setChecked(a);  // toggled sinyali buraya geri döner
        return;
    }
    session_.apparent = a;
    model_->setApparent(a);
    treemap_->setApparent(a);
    refreshStats();
    saveSettings();
}

void MainWindow::saveSettings()
{
    if (applyingSettings_)
        return;
    settings_.apparent = session_.apparent;
    settings_.oneFs = session_.oneFs;
    settings_.grouping = session_.grouping;
    settings_.groupMin = session_.groupMin;
    settings_.showTreemap = !actTreemap_ || actTreemap_->isChecked();  // tema settings_ içinde güncel
    settings_.save();
}

void MainWindow::setGrouping(bool on)
{
    if (actGroup_->isChecked() != on) {
        actGroup_->setChecked(on);  // toggled sinyali buraya geri döner
        return;
    }
    regroup(on, session_.groupMin);
}

void MainWindow::setGroupMin(uint32_t n)
{
    regroup(session_.grouping, n);
}

void MainWindow::regroup(bool on, uint32_t minCount)
{
    // Eski gruplar ağaçtan ayrılır; seçimi ve yakınlaştırmayı gerçek düğüme taşı.
    Node *sel = realNode(selectedNode());
    Node *zoom = realNode(treemap_->root());
    treemap_->setSelected(nullptr);
    treemap_->invalidate();
    session_.setGrouping(on, minCount);
    saveSettings();
    if (!rootNode())
        return;
    model_->setRoot(rootNode());
    treemap_->setRoot(zoom ? zoom : rootNode());
    treeView_->expand(model_->index(0, 0));
    selectNode(sel ? sel : rootNode());
    updateZoomLabel();
}

// ───────────── bağlam menüsü ─────────────

void MainWindow::showMenu(Node *n, const QPoint &gpos)
{
    QMenu m(this);
    m.addAction(QIcon::fromTheme("document-open"), "Aç", this,
                [n] { QDesktopServices::openUrl(QUrl::fromLocalFile(qs(n->path()))); });
    m.addAction(QIcon::fromTheme("system-file-manager"), "Dosya yöneticisinde göster", this,
                [n] { revealPath(n->path()); });
    m.addAction(QIcon::fromTheme("edit-copy"), "Yolu kopyala", this, [this, n] { copyPath(n); });
    if (n->isDir) {
        m.addSeparator();
        m.addAction(QIcon::fromTheme("zoom-in"), "Treemap'te yakınlaştır", this, [this, n] { zoomTo(n); });
        m.addAction(QIcon::fromTheme("view-refresh"),
                    (n->flags & F_GROUP)   ? "Bulunduğu klasörü yeniden tara"
                    : (n->flags & F_MOUNT) ? "Bu dosya sistemini de tara"
                                           : "Bu klasörü yeniden tara",
                    this,
                    [this, n] { refreshNode(n); });
    }
    if (n->parent) {
        m.addSeparator();
        m.addAction(QIcon::fromTheme("user-trash"), "Çöp kutusuna taşı", this, [this, n] { deleteNode(n, false); });
        m.addAction(QIcon::fromTheme("edit-delete"), "Kalıcı olarak sil…", this, [this, n] { deleteNode(n, true); });
    }
    m.exec(gpos);
}

void MainWindow::copyPath(Node *n)
{
    QApplication::clipboard()->setText(qs(n->path()));
}

// ───────────── değişiklikler ─────────────

void MainWindow::refreshNode(Node *n)
{
    n = realNode(n);  // grup için bulunduğu klasör
    if (session_.scanning() || !n)
        return;
    startScan(qs(n->path()), n);
}

void MainWindow::deleteNode(Node *n, bool permanent, bool confirm)
{
    if (!n->parent || session_.scanning())
        return;
    const QString path = qs(n->path());
    const QString kind = (n->flags & F_GROUP) ? QString("gruptaki %1 dosyayı").arg(qs(fmtInt(n->files)))
                         : n->isDir           ? QString("klasörü")
                                              : QString("dosyayı");
    const QString size = qs(fmtSize(n->sizeOf(session_.apparent)));
    if (confirm) {
        const QString q = permanent
            ? QString("Bu %1 <b>kalıcı olarak</b> silmek istediğinize emin misiniz?<br><br><code>%2</code><br>(%3)"
                      "<br><br>Bu işlem geri alınamaz.").arg(kind, path.toHtmlEscaped(), size)
            : QString("Bu %1 çöp kutusuna taşınsın mı?<br><br><code>%2</code><br>(%3)").arg(kind, path.toHtmlEscaped(), size);
        if (QMessageBox::question(this, "TuxDirStat", q, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
    }
    std::string err;
    int failed = 0;
    std::string firstFailed;
    for (const std::string &p : diskPaths(n)) {
        if (!(permanent ? removePath(p, &err) : moveToTrash(qs(p))) && !failed++)
            firstFailed = p;
    }
    if (failed) {
        QMessageBox::warning(this, "TuxDirStat", "Silinemedi (ya da yalnızca kısmen silindi):\n" + qs(firstFailed) +
                                                  (err.empty() ? QString() : "\n\n" + qs(err)));
        refreshNode(n->parent);  // ağacı diskle eşitle
        return;
    }
    n = session_.removalTarget(n);  // grubun son öğesiyse grup da kalkar
    Node *p = n->parent;
    treemap_->invalidate();
    if (treemap_->root() && treemap_->root()->isWithin(n))
        treemap_->setRoot(p);
    if (treemap_->selected() && treemap_->selected()->isWithin(n))
        treemap_->setSelected(nullptr);
    model_->removeNode(n, [this, n] { session_.remove(n); });
    treeView_->viewport()->update();
    fillExtTable();
    updateTotals();
    updateZoomLabel();
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    session_.cancelScan();
    QMainWindow::closeEvent(e);
}
