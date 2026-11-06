#include "mainwindow.h"

#include <algorithm>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QDate>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
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
#include "resultdialog.h"
#include "theme.h"
#include "treemap_render.h"
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
    const QColor col = displayColor(c);
    QRadialGradient g(size * 0.35, size * 0.35, size * 0.8);
    g.setColorAt(0, col.lighter(150));
    g.setColorAt(1, col.darker(160));
    p.setBrush(g);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(1, 1, size - 2, size - 2, 3, 3);
    return QIcon(pm);
}

const char *const SNAPSHOT_FILTER = N_("Snapshots (*.json *.json.gz *.json.zst *.json.xz);;All files (*)");

}  // namespace

bool moveToTrash(const QString &path)
{
    return QFile::moveToTrash(path) || trashWithCommand(path.toStdString());
}

QIcon appIcon()
{
    QIcon themed = QIcon::fromTheme("tuxdirstat");
    if (!themed.isNull())
        return themed;
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
    setWindowTitle("TuxDirStat — " + QT("Location to scan"));
    resize(900, 460);
    auto *lay = new QVBoxLayout(this);
    lay->addWidget(new QLabel(QT("Choose what to scan:")));
    list_ = new QTreeWidget;
    list_->setHeaderLabels({QT("Location"), QT("Type"), QT("Used"), QT("Free"), QT("Total"), QT("Usage")});
    list_->setRootIsDecorated(false);
    list_->setAlternatingRowColors(true);
    QStyle *st = style();

    const QString home = QDir::homePath();
    auto *it = new QTreeWidgetItem({QTF("Home folder  (%1)", {home.toStdString()})});
    it->setIcon(0, QIcon::fromTheme("user-home", st->standardIcon(QStyle::SP_DirHomeIcon)));
    it->setData(0, Qt::UserRole, home);
    list_->addTopLevelItem(it);

    for (const Drive &d : listDrives()) {
        auto *item = new QTreeWidgetItem({qs(d.mp), qs(d.fs), qs(fmtSize(d.used)), qs(fmtSize(d.avail)), qs(fmtSize(d.total)), ""});
        item->setToolTip(0, qs(d.src));
        item->setIcon(0, QIcon::fromTheme("drive-harddisk", st->standardIcon(QStyle::SP_DriveHDIcon)));
        item->setData(0, Qt::UserRole, qs(d.mp));
        for (int c : {2, 3, 4})
            item->setTextAlignment(c, Qt::AlignRight | Qt::AlignVCenter);
        list_->addTopLevelItem(item);
        auto *bar = new QProgressBar;
        bar->setRange(0, 100);
        bar->setValue(int(d.used * 100 / d.total));
        bar->setMaximumHeight(18);
        list_->setItemWidget(item, 5, bar);
    }
    list_->header()->setStretchLastSection(false);
    list_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int c : {1, 2, 3, 4})
        list_->header()->resizeSection(c, c == 1 ? 80 : 90);
    list_->header()->resizeSection(5, 130);
    list_->setCurrentItem(list_->topLevelItem(0));
    connect(list_, &QTreeWidget::itemDoubleClicked, this, [this] { accept_(); });
    lay->addWidget(list_);
    auto *rl = new QHBoxLayout;
    rl->addWidget(new QLabel(QT("Other computer (SSH):")));
    remote_ = new QLineEdit;
    remote_->setPlaceholderText(QT("host:/path, e.g. pi:/home"));
    remote_->setToolTip(QT("Scans a folder on another computer over SSH. Needs key login without a password prompt."));
    rl->addWidget(remote_, 1);
    lay->addLayout(rl);
    admin_ = new QCheckBox(QT("Scan as administrator (read-only, asks for your password)"));
    admin_->setToolTip(QT("Reads folders your user cannot open, such as /root or /var/lib. Deleting still uses your own rights."));
    lay->addWidget(admin_);

    auto *btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    btns->button(QDialogButtonBox::Ok)->setText(QT("Scan"));
    btns->button(QDialogButtonBox::Cancel)->setText(QT("Cancel"));
    auto *pick = btns->addButton(QT("Choose folder…"), QDialogButtonBox::ActionRole);
    connect(pick, &QPushButton::clicked, this, [this] {
        QString d = QFileDialog::getExistingDirectory(this, QT("Choose folder"), QDir::homePath());
        if (!d.isEmpty()) {
            path = d;
            admin = admin_->isChecked();
            accept();
        }
    });
    connect(btns, &QDialogButtonBox::accepted, this, [this] { accept_(); });
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(btns);
}

void TargetDialog::accept_()
{
    const QString remote = remote_->text().trimmed();
    if (!remote.isEmpty()) {
        std::string host, rpath;
        if (!parseRemote(remote.toStdString(), host, rpath)) {
            QMessageBox::warning(this, "TuxDirStat", QT("Write the remote location as host:/path, e.g. pi:/home"));
            return;
        }
        path = remote;
        admin = false;
        accept();
        return;
    }
    if (auto *it = list_->currentItem()) {
        path = it->data(0, Qt::UserRole).toString();
        admin = admin_->isChecked();
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
    connect(treeView_, &QTreeView::doubleClicked, this, [this](const QModelIndex &i) {
        Node *n = TreeModel::nodeAt(i);
        if (!n->isDir && localTree())
            QDesktopServices::openUrl(QUrl::fromLocalFile(qs(n->path())));
    });

    // Uzantılar
    extTable_ = new QTableWidget(0, 5);
    extTable_->setHorizontalHeaderLabels({QT("Extension"), QT("Type"), QT("Size"), "%", QT("Files")});
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

    if (!path.isEmpty()) {
        if (QFileInfo(path).isFile())  // anlık görüntü dosyası
            QTimer::singleShot(0, this, [this, path] { openSnapshot(path); });
        else
            QTimer::singleShot(0, this, [this, path] { startScan(path); });
    } else if (settings_.openLast && QFileInfo::exists(qs(lastScanFile()))) {
        openedLast_ = true;
        QTimer::singleShot(0, this, [this] { openSnapshot(qs(lastScanFile())); });
    } else {
        QTimer::singleShot(0, this, &MainWindow::chooseTarget);
    }
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
    QAction *actOpen = act(QT("Open…"), "document-open", QKeySequence("Ctrl+O"));
    connect(actOpen, &QAction::triggered, this, &MainWindow::chooseTarget);
    QAction *actRescan = act(QT("Rescan"), "view-refresh", QKeySequence("F5"));
    connect(actRescan, &QAction::triggered, this, &MainWindow::rescan);
    actStop_ = act(QT("Stop"), "process-stop", QKeySequence("Esc"));
    actStop_->setEnabled(false);
    connect(actStop_, &QAction::triggered, this, [this] { session_.cancelScan(); });
    QAction *actOpenSnap = act(QT("Open snapshot…"), "document-import");
    connect(actOpenSnap, &QAction::triggered, this, &MainWindow::openSnapshotDialog);
    QAction *actSaveSnap = act(QT("Save snapshot…"), "document-save-as", QKeySequence("Ctrl+S"));
    connect(actSaveSnap, &QAction::triggered, this, &MainWindow::saveSnapshotDialog);
    QAction *actCsv = act(QT("Export as CSV…"), "document-export");
    connect(actCsv, &QAction::triggered, this, &MainWindow::exportCsvDialog);
    QAction *actHtml = act(QT("Export HTML report…"), "text-html");
    connect(actHtml, &QAction::triggered, this, &MainWindow::exportHtmlDialog);
    QAction *actPng = act(QT("Save map as image…"), "image-x-generic");
    connect(actPng, &QAction::triggered, this, &MainWindow::exportPngDialog);

    QAction *actZoomIn = act(QT("Zoom in"), "zoom-in", QKeySequence("Ctrl++"));
    connect(actZoomIn, &QAction::triggered, this, &MainWindow::zoomInSelected);
    actZoomOut_ = act(QT("Zoom out"), "zoom-out", QKeySequence("Backspace"));
    connect(actZoomOut_, &QAction::triggered, this, &MainWindow::zoomOut);
    QAction *actZoomReset = act(QT("Root"), "zoom-original", QKeySequence("Ctrl+0"));
    connect(actZoomReset, &QAction::triggered, this, [this] { zoomTo(rootNode()); });

    actApparent_ = act(QT("Apparent size"), "view-statistics");
    actApparent_->setCheckable(true);
    actApparent_->setToolTip(QT("On: the apparent size of files (ls)\nOff: the space they take on disk (du)"));
    connect(actApparent_, &QAction::toggled, this, &MainWindow::setApparent);
    actOneFs_ = act(QT("One file system"), "drive-harddisk");
    actOneFs_->setCheckable(true);
    actOneFs_->setChecked(true);
    actOneFs_->setToolTip(QT("Do not cross into other disks or mount points\n(btrfs subvolumes on the same device are included)"));
    connect(actOneFs_, &QAction::toggled, this, [this](bool on) {
        session_.oneFs = on;
        saveSettings();
    });
    actGroup_ = act(QT("Group files"), "view-group", QKeySequence("Ctrl+G"));
    actGroup_->setCheckable(true);
    actGroup_->setToolTip(QT("Collect many similar items in a folder (like game assets) into one box:\n"
                             "files with the same extension, small files and small subfolders"));
    connect(actGroup_, &QAction::toggled, this, &MainWindow::setGrouping);
    actTreemap_ = act(QT("Treemap"), "view-split-top-bottom", QKeySequence("F9"));
    actTreemap_->setCheckable(true);
    actTreemap_->setChecked(true);
    connect(actTreemap_, &QAction::toggled, this, [this](bool on) {
        tmBox_->setVisible(on);
        saveSettings();
    });
    actAge_ = act(QT("Color by age"), "chronometer", QKeySequence("F8"));
    actAge_->setCheckable(true);
    actAge_->setToolTip(QT("Color the treemap by when files last changed instead of by type"));
    connect(actAge_, &QAction::toggled, this, &MainWindow::setColorByAge);
    actSunburst_ = act(QT("Sunburst"), "chart-pie", QKeySequence("F7"));
    actSunburst_->setCheckable(true);
    actSunburst_->setToolTip(QT("Show the map as rings around the folder instead of rectangles"));
    connect(actSunburst_, &QAction::toggled, this, &MainWindow::setSunburst);
    actOpenLast_ = act(QT("Reopen the last scan at start"), "document-open-recent");
    actOpenLast_->setCheckable(true);
    connect(actOpenLast_, &QAction::toggled, this, [this](bool on) {
        settings_.openLast = on;
        saveSettings();
        if (!on)
            QFile::remove(qs(lastScanFile()));
    });

    QAction *actCopy = act(QT("Copy path"), "edit-copy", QKeySequence("Ctrl+C"));
    connect(actCopy, &QAction::triggered, this, [this] {
        if (Node *n = selectedNode())
            copyPath(n);
    });
    QAction *actTrash = act(QT("Move to trash"), "user-trash", QKeySequence("Delete"));
    connect(actTrash, &QAction::triggered, this, [this] {
        if (Node *n = selectedNode())
            deleteNode(n, false);
    });
    QAction *actDelete = act(QT("Delete permanently…"), "edit-delete", QKeySequence("Shift+Delete"));
    connect(actDelete, &QAction::triggered, this, [this] {
        if (Node *n = selectedNode())
            deleteNode(n, true);
    });
    QAction *actQuit = act(QT("Quit"), "application-exit", QKeySequence("Ctrl+Q"));
    connect(actQuit, &QAction::triggered, this, &QWidget::close);

    // Araçlar
    QAction *actFind = act(QT("Find files…"), "edit-find", QKeySequence("Ctrl+F"));
    connect(actFind, &QAction::triggered, this, &MainWindow::findFiles);
    QAction *actDup = act(QT("Duplicate files…"), "edit-copy");
    connect(actDup, &QAction::triggered, this, [this] {
        Node *root = rootNode();
        runTool(T("Duplicate files"), [root](Job &j) { j.result = findDuplicates(root, j); });
    });
    QAction *actClean = act(QT("Cleanup suggestions…"), "edit-clear-all");
    connect(actClean, &QAction::triggered, this, [this] {
        Node *root = rootNode();
        const bool ap = session_.apparent;
        runTool(T("Cleanup suggestions"), [root, ap](Job &j) { j.result = findCleanup(root, ap, j); });
    });
    QAction *actOwners = act(QT("Usage by owner"), "user-identity");
    connect(actOwners, &QAction::triggered, this, [this] {
        if (toolReady())
            showTable(ownerUsage(rootNode(), session_.apparent));
    });
    QAction *actUnowned = act(QT("Files no package owns…"), "package-x-generic");
    actUnowned->setToolTip(QT("Arch Linux: files under /usr, /etc and /opt that no pacman package owns"));
    actUnowned->setEnabled(havePacman());
    connect(actUnowned, &QAction::triggered, this, [this] {
        Node *root = rootNode();
        const bool ap = session_.apparent;
        runTool(T("Files no package owns"), [root, ap](Job &j) { j.result = findUnowned(root, ap, j); });
    });
    QAction *actCompare = act(QT("Compare with snapshot…"), "document-compare");
    connect(actCompare, &QAction::triggered, this, &MainWindow::compareDialog);
    QAction *actPackages = act(QT("Installed packages…"), "package-x-generic");
    actPackages->setToolTip(QT("pacman and Flatpak packages by size, with orphans"));
    actPackages->setEnabled(havePacman() || haveProgram("flatpak"));
    connect(actPackages, &QAction::triggered, this, [this] {
        runTool(T("Installed packages"), [](Job &j) { j.result = packageSizes(j); }, false);
    });
    QAction *actBtrfs = act(QT("Btrfs: shared and compressed data…"), "drive-multidisk");
    actBtrfs->setToolTip(QT("How much is shared with snapshots or reflinks (and frees nothing when deleted), and how much is compressed"));
    connect(actBtrfs, &QAction::triggered, this, [this] {
        Node *root = rootNode();
        runTool(T("Btrfs: shared and compressed data"), [root](Job &j) { j.result = btrfsUsage(root, j); });
    });
    QAction *actHistory = act(QT("Size history…"), "office-chart-line");
    connect(actHistory, &QAction::triggered, this, [this] {
        Node *root = rootNode();
        const bool ap = session_.apparent;
        runTool(T("Size history"), [root, ap](Job &j) { j.result = historyGrowth(root, ap, j); });
    });
    actRecord_ = act(QT("Record size history weekly"), "media-record");
    actRecord_->setCheckable(true);
    actRecord_->setToolTip(QT("Saves the size of this folder every week (systemd user timer) for the size history"));
    connect(actRecord_, &QAction::toggled, this, &MainWindow::setRecording);

    QToolBar *tb = addToolBar(QT("Tools"));
    tb->setObjectName("main_toolbar");
    tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    tb->setIconSize(QSize(18, 18));
    tb->setMovable(false);
    for (QAction *a : {actZoomIn, actZoomOut_, actZoomReset, actFind})
        a->setIconText(QString());  // araç çubuğunda yalnızca simge; ipucu kalır
    actFind->setToolTip(QT("Find files…"));
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
              actApparent_, actOneFs_, actGroup_, actTreemap_, actSunburst_, actAge_, nullptr, actFind});
    fill(menuBar()->addMenu(QT("&File")), {actOpen, actRescan, actStop_, nullptr, actOpenSnap, actSaveSnap, nullptr,
                                           actHtml, actPng, actCsv, nullptr, actQuit});
    fill(menuBar()->addMenu(QT("&Edit")), {actCopy, nullptr, actTrash, actDelete});
    QMenu *view = menuBar()->addMenu(QT("&View"));
    fill(view, {actZoomIn, actZoomOut_, actZoomReset, nullptr, actApparent_, actOneFs_, actGroup_});
    QMenu *thr = view->addMenu(QT("Grouping threshold"));
    auto *thrGroup = new QActionGroup(this);
    for (uint32_t n : GROUP_MIN_CHOICES) {
        QAction *a = thr->addAction(QTF("At least %1 items", {std::to_string(n)}));
        a->setCheckable(true);
        a->setChecked(n == settings_.groupMin);
        thrGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, n] { setGroupMin(n); });
    }
    fill(view, {nullptr, actTreemap_, actSunburst_, actAge_, nullptr, actOpenLast_});
    QMenu *themeMenu = view->addMenu(QT("Theme"));
    auto *themeGroup = new QActionGroup(this);
    for (auto [id, label] : {std::pair{"system", N_("System")}, {"light", N_("Light")}, {"dark", N_("Dark")}}) {
        QAction *a = themeMenu->addAction(QT(label));
        a->setCheckable(true);
        a->setChecked(settings_.theme == id);
        themeGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, t = std::string(id)] { setTheme(t); });
    }
    QMenu *langMenu = view->addMenu(QT("Language"));
    auto *langGroup = new QActionGroup(this);
    std::vector<std::pair<std::string, QString>> langs{{"system", QT("System")}};
    for (const i18n::Language &l : i18n::languages())
        langs.emplace_back(l.code, QString::fromStdString(l.name));
    for (const auto &[id, label] : langs) {
        QAction *a = langMenu->addAction(label);
        a->setCheckable(true);
        a->setChecked(settings_.language == id);
        langGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, l = id] { setLanguage(l); });
    }
    fill(menuBar()->addMenu(QT("&Tools")), {actFind, actDup, actClean, nullptr, actOwners, actUnowned, actPackages, actBtrfs,
                                            nullptr, actCompare, actHistory, actRecord_});

    // Kayıtlı ayarları uygula (kök yokken yalnızca durumları kurar).
    applyingSettings_ = true;
    session_.groupMin = settings_.groupMin;
    actApparent_->setChecked(settings_.apparent);
    actOneFs_->setChecked(settings_.oneFs);
    actGroup_->setChecked(settings_.grouping);
    actTreemap_->setChecked(settings_.showTreemap);
    actAge_->setChecked(settings_.colorByAge);
    actSunburst_->setChecked(settings_.sunburst);
    actOpenLast_->setChecked(settings_.openLast);
    applyingSettings_ = false;
    updateRecordAction();

    QMenu *help = menuBar()->addMenu(QT("&Help"));
    help->addAction(QT("About"), this, [this] {
        QMessageBox::about(this, QT("About TuxDirStat"),
                           "<b>TuxDirStat " TUXDIRSTAT_VERSION "</b><br>" + QT("Disk usage analyzer for Linux.") + "<br><br>" +
                               QT("Click an item in the tree or the treemap. Double-click in the treemap to zoom into a "
                                  "folder, Backspace goes back. Selecting a row in the extension list marks those files "
                                  "in the treemap.") +
                               "<br><br>" + QT("License: GPL-3.0-or-later") +
                               "<br><a href=\"https://github.com/Gubir34/TuxDirStat\">github.com/Gubir34/TuxDirStat</a>");
    });
}

// ───────────── tarama ─────────────

void MainWindow::chooseTarget()
{
    TargetDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted || dlg.path.isEmpty())
        return;
    if (dlg.admin)
        startPrivilegedScan(dlg.path);
    else
        startScan(dlg.path);
}

void MainWindow::beginBusy(const QString &title)
{
    progress_->show();
    actStop_->setEnabled(true);
    poll_.start();
    if (!title.isEmpty())
        setWindowTitle("TuxDirStat — " + title);
}

void MainWindow::startScan(const QString &path, Node *target)
{
    openedLast_ = false;
    std::string host, rpath;
    const QString abs = parseRemote(path.toStdString(), host, rpath) ? path : QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    session_.startScan(abs.toStdString(), target);
    beginBusy(target ? QString() : abs);
}

void MainWindow::startPrivilegedScan(const QString &path)
{
    const QString abs = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    session_.startPrivilegedScan(abs.toStdString());
    beginBusy(abs);
}

void MainWindow::openSnapshot(const QString &file)
{
    session_.openSnapshot(QFileInfo(file).absoluteFilePath().toStdString());
    beginBusy(QFileInfo(file).fileName());
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
        statusLeft_->setText(statusLeft_->fontMetrics().elidedText(qs(session_.progressText()), Qt::ElideMiddle, 900));
        return;
    }
    poll_.stop();
    progress_->hide();
    actStop_->setEnabled(false);
    switch (res) {
    case Session::Result::Failed:
        QMessageBox::warning(this, "TuxDirStat", QT("The scan failed:") + "\n" + qs(session_.error()));
        statusLeft_->clear();
        if (rootNode())
            setWindowTitle("TuxDirStat — " + qs(rootNode()->path()));
        return;
    case Session::Result::Cancelled:
        statusLeft_->setText(QT("Scan stopped."));
        return;
    case Session::Result::NewTree: {
        Node *root = session_.pendingRoot();
        treemap_->setSelected(nullptr);
        treemap_->setRoot(root);
        model_->setRoot(root);
        session_.commitNewTree();
        treeView_->expand(model_->index(0, 0));
        treeView_->setCurrentIndex(model_->index(0, 0));
        setWindowTitle("TuxDirStat — " + qs(root->path()));
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
    updateRecordAction();
    if (openedLast_) {
        openedLast_ = false;
        statusLeft_->setText(QTF("Showing the saved scan from %1. Press F5 to scan again.",
                                 {fmtTime(QFileInfo(qs(lastScanFile())).lastModified().toSecsSinceEpoch())}));
        return;
    }
    statusLeft_->setText(QTF("Done: %1 items in %2 s.", {fmtInt(session_.lastCount()), fmtDecimal(session_.elapsed(), 2)}));
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
        title_->setText(QT("Nothing scanned yet"));
        subtitle_->setText(QT("Open… (Ctrl+O) to choose a location"));
        diskLabel_->clear();
        diskBar_->hide();
        return;
    }
    const std::string path = root->path();
    title_->setText(title_->fontMetrics().elidedText(qs(path), Qt::ElideMiddle, 700));
    title_->setToolTip(qs(path));
    subtitle_->setText(QTF("%1 · %2 files · %3 folders · %4",
                           {fmtSize(root->sizeOf(session_.apparent)), fmtInt(root->files), fmtInt(root->dirs),
                            session_.apparent ? T("apparent size") : T("size on disk")}));
    uint64_t total = 0, avail = 0;
    if (diskUsage(path, total, avail)) {
        const double used = double(total - avail) / double(total);
        diskLabel_->setText(QTF("Disk: %1 free of %2 (%3 used)", {fmtSize(avail), fmtSize(total), fmtPct(used)}));
        diskBar_->setValue(int(used * 1000));
        diskBar_->show();
    } else {
        diskLabel_->clear();
        diskBar_->hide();
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

// Treemap'in üstündeki tıklanabilir konum çubuğu: kökten yakınlaşılan klasöre kadar;
// yaşa göre renklendirmede sağda renk açıklaması.
void MainWindow::updateZoomLabel()
{
    Node *r = treemap_->root();
    actZoomOut_->setEnabled(r && r->parent);
    auto *lay = static_cast<QHBoxLayout *>(crumbs_->layout());
    while (QLayoutItem *it = lay->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    auto *lbl = new QLabel(QT("Treemap"));
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
            if (i == chain.size() - 2)
                lay->addWidget(new QLabel("…"));
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
    if (actAge_ && actAge_->isChecked()) {
        QString html = QT("Last change:") + " ";
        for (const AgeBucket &b : ageBuckets())
            html += QString("<span style=\"color:%1\">■</span>&nbsp;%2&nbsp;&nbsp;")
                        .arg(displayColor(b.color).name(), QT(b.label).toHtmlEscaped());
        auto *legend = new QLabel(html);
        legend->setForegroundRole(QPalette::PlaceholderText);
        lay->addWidget(legend);
    }
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

void MainWindow::setLanguage(const std::string &lang)
{
    settings_.language = lang;
    saveSettings();
    QMessageBox::information(this, "TuxDirStat", QT("The new language is used after TuxDirStat is restarted."));
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

void MainWindow::setColorByAge(bool on)
{
    if (actAge_->isChecked() != on) {
        actAge_->setChecked(on);
        return;
    }
    treemap_->setColorMode(on ? ColorMode::Age : ColorMode::Extension);
    updateZoomLabel();
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
    settings_.showTreemap = !actTreemap_ || actTreemap_->isChecked();  // tema ve dil settings_ içinde güncel
    settings_.colorByAge = actAge_ && actAge_->isChecked();
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
    const bool local = localTree();
    if (local) {
        m.addAction(QIcon::fromTheme("document-open"), QT("Open"), this,
                    [n] { QDesktopServices::openUrl(QUrl::fromLocalFile(qs(n->path()))); });
        m.addAction(QIcon::fromTheme("system-file-manager"), QT("Show in file manager"), this,
                    [n] { revealPath(n->path()); });
    }
    m.addAction(QIcon::fromTheme("edit-copy"), QT("Copy path"), this, [this, n] { copyPath(n); });
    if (local && havePacman() && !(n->flags & F_GROUP))
        m.addAction(QIcon::fromTheme("package-x-generic"), QT("Which package owns this?"), this, [this, n] {
            QApplication::setOverrideCursor(Qt::WaitCursor);
            const std::string msg = packageOwnerMessage(n->path());
            QApplication::restoreOverrideCursor();
            QMessageBox::information(this, "TuxDirStat", qs(msg));
        });
    if (n->isDir) {
        m.addSeparator();
        m.addAction(QIcon::fromTheme("zoom-in"), QT("Zoom into in treemap"), this, [this, n] { zoomTo(n); });
        m.addAction(QIcon::fromTheme("view-refresh"),
                    (n->flags & F_GROUP)   ? QT("Rescan its folder")
                    : (n->flags & F_MOUNT) ? QT("Scan this file system too")
                                           : QT("Rescan this folder"),
                    this, [this, n] { refreshNode(n); });
    }
    if (n->parent && local) {
        m.addSeparator();
        m.addAction(QIcon::fromTheme("user-trash"), QT("Move to trash"), this, [this, n] { deleteNode(n, false); });
        m.addAction(QIcon::fromTheme("edit-delete"), QT("Delete permanently…"), this, [this, n] { deleteNode(n, true); });
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

bool MainWindow::removeFromDisk(Node *n, bool permanent, std::string *err, std::string *firstFailed)
{
    int failed = 0;
    for (const std::string &p : diskPaths(n)) {
        if (!(permanent ? removePath(p, err) : moveToTrash(qs(p))) && !failed++)
            *firstFailed = p;
    }
    return failed == 0;
}

void MainWindow::dropNode(Node *n)
{
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

void MainWindow::deleteNode(Node *n, bool permanent, bool confirm)
{
    if (!n->parent || session_.scanning())
        return;
    if (!localTree()) {
        QMessageBox::information(this, "TuxDirStat", QT("This scan is from another computer or a snapshot of one; its files cannot be deleted from here."));
        return;
    }
    const QString path = qs(n->path());
    const QString kind = (n->flags & F_GROUP) ? QTF("the %1 files in this group", {fmtInt(n->files)})
                         : n->isDir           ? QT("this folder")
                                              : QT("this file");
    const QString size = qs(fmtSize(n->sizeOf(session_.apparent)));
    if (confirm) {
        const QString q = permanent
            ? QTF("Delete %1 <b>permanently</b>?", {kind.toStdString()}) + "<br><br><code>" + path.toHtmlEscaped() +
                  "</code><br>(" + size + ")<br><br>" + QT("This cannot be undone.")
            : QTF("Move %1 to the trash?", {kind.toStdString()}) + "<br><br><code>" + path.toHtmlEscaped() + "</code><br>(" +
                  size + ")";
        if (QMessageBox::question(this, "TuxDirStat", q, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
    }
    std::string err, firstFailed;
    if (!removeFromDisk(n, permanent, &err, &firstFailed)) {
        QMessageBox::warning(this, "TuxDirStat", QT("Could not delete (or only partly deleted):") + "\n" + qs(firstFailed) +
                                                      (err.empty() ? QString() : "\n\n" + qs(err)));
        refreshNode(n->parent);  // ağacı diskle eşitle
        return;
    }
    dropNode(n);
}

bool MainWindow::removeQuietly(Node *n, QString *err)
{
    if (!n->parent || session_.scanning() || !localTree())
        return false;
    std::string e, firstFailed;
    if (!removeFromDisk(n, false, &e, &firstFailed)) {
        if (err)
            *err = qs(firstFailed);
        refreshNode(n->parent);
        return false;
    }
    dropNode(n);
    return true;
}

// ───────────── araçlar ─────────────

bool MainWindow::toolReady()
{
    if (session_.scanning()) {
        QMessageBox::information(this, "TuxDirStat", QT("Wait until the scan has finished."));
        return false;
    }
    if (!rootNode()) {
        QMessageBox::information(this, "TuxDirStat", QT("Scan a folder first."));
        return false;
    }
    return true;
}

void MainWindow::showTable(ResultTable t)
{
    ResultDialog dlg(this, std::move(t));
    dlg.exec();
}

void MainWindow::runTool(const char *title, std::function<void(Job &)> fn, bool needsTree)
{
    if (needsTree ? !toolReady() : session_.scanning())
        return;
    auto job = std::make_shared<Job>(title, std::move(fn));
    runJobWithProgress(this, job, [this](Job &j) {
        if (!j.error.empty() && j.result.rows.empty())
            QMessageBox::warning(this, "TuxDirStat", qs(j.error));
        else
            showTable(std::move(j.result));
    });
}

void MainWindow::findFiles()
{
    if (!toolReady())
        return;
    QDialog dlg(this);
    dlg.setWindowTitle("TuxDirStat — " + QT("Find files"));
    auto *lay = new QVBoxLayout(&dlg);
    auto *edit = new QLineEdit(lastQuery_);
    edit->setPlaceholderText("*.iso >1G older:1y");
    edit->setMinimumWidth(520);
    lay->addWidget(edit);
    auto *help = new QLabel(QT(searchHelp()));
    help->setForegroundRole(QPalette::PlaceholderText);
    QFont mono("monospace");
    mono.setStyleHint(QFont::Monospace);
    help->setFont(mono);
    lay->addWidget(help);
    auto *btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    btns->button(QDialogButtonBox::Ok)->setText(QT("Find"));
    btns->button(QDialogButtonBox::Cancel)->setText(QT("Cancel"));
    connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(btns);
    for (;;) {
        if (dlg.exec() != QDialog::Accepted)
            return;
        lastQuery_ = edit->text();
        std::string err;
        ResultTable t = searchTree(rootNode(), lastQuery_.toStdString(), session_.apparent, &err);
        if (!err.empty()) {
            QMessageBox::warning(this, "TuxDirStat", qs(err));
            continue;
        }
        t.title += ": " + lastQuery_.toStdString();
        showTable(std::move(t));
        return;
    }
}

void MainWindow::openSnapshotDialog()
{
    if (session_.scanning())
        return;
    const QString f = QFileDialog::getOpenFileName(this, QT("Open snapshot"), QDir::homePath(), QT(SNAPSHOT_FILTER));
    if (!f.isEmpty())
        openSnapshot(f);
}

void MainWindow::saveSnapshotDialog()
{
    if (!toolReady())
        return;
    QString name = QFileInfo(qs(rootNode()->path())).fileName();
    if (name.isEmpty())
        name = "root";
    const QString suggestion = QDir::homePath() + "/" + name + "-" + QDate::currentDate().toString(Qt::ISODate) + ".json";
    QString f = QFileDialog::getSaveFileName(this, QT("Save snapshot"), suggestion, QT(SNAPSHOT_FILTER));
    if (f.isEmpty())
        return;
    if (QFileInfo(f).suffix().isEmpty())
        f += ".json";
    QApplication::setOverrideCursor(Qt::WaitCursor);
    std::string err;
    const bool ok = saveSnapshot(rootNode(), f.toStdString(), &err);
    QApplication::restoreOverrideCursor();
    if (!ok)
        QMessageBox::warning(this, "TuxDirStat", QT("Could not save the snapshot:") + "\n" + qs(err));
    else
        statusLeft_->setText(QTF("Snapshot saved: %1", {f.toStdString()}));
}

void MainWindow::exportCsvDialog()
{
    if (!toolReady())
        return;
    QString f = QFileDialog::getSaveFileName(this, QT("Export as CSV"), QDir::homePath() + "/tuxdirstat.csv",
                                             QT("CSV files (*.csv);;All files (*)"));
    if (f.isEmpty())
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    std::string err;
    const bool ok = exportCsv(rootNode(), f.toStdString(), &err);
    QApplication::restoreOverrideCursor();
    if (!ok)
        QMessageBox::warning(this, "TuxDirStat", QT("Could not write the file:") + "\n" + qs(err));
    else
        statusLeft_->setText(QTF("Exported: %1", {f.toStdString()}));
}

void MainWindow::compareDialog()
{
    if (!toolReady())
        return;
    const QString f = QFileDialog::getOpenFileName(this, QT("Compare with snapshot"), QDir::homePath(), QT(SNAPSHOT_FILTER));
    if (f.isEmpty())
        return;
    Node *root = rootNode();
    const bool ap = session_.apparent;
    const std::string file = f.toStdString();
    runTool(T("Compare with snapshot"), [root, ap, file](Job &j) { j.result = compareWithSnapshot(root, file, ap, j); });
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    const bool keep = settings_.openLast && !session_.scanning() && localTree();
    session_.cancelScan();
    if (keep) {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        statusLeft_->setText(QT("Saving the scan for next time…"));
        QApplication::processEvents();
        std::string err;
        saveLastScan(rootNode(), &err);
        QApplication::restoreOverrideCursor();
    }
    QMainWindow::closeEvent(e);
}

void MainWindow::exportHtmlDialog()
{
    if (!toolReady())
        return;
    QString name = QFileInfo(qs(rootNode()->path())).fileName();
    if (name.isEmpty())
        name = "root";
    QString f = QFileDialog::getSaveFileName(this, QT("Export HTML report"), QDir::homePath() + "/" + name + "-report.html",
                                             QT("HTML files (*.html);;All files (*)"));
    if (f.isEmpty())
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    std::string err;
    const bool ok = exportHtml(rootNode(), f.toStdString(), session_.apparent, &err);
    QApplication::restoreOverrideCursor();
    if (!ok)
        QMessageBox::warning(this, "TuxDirStat", QT("Could not write the file:") + "\n" + qs(err));
    else
        statusLeft_->setText(QTF("Exported: %1", {f.toStdString()}));
}

void MainWindow::exportPngDialog()
{
    if (!toolReady())
        return;
    QString f = QFileDialog::getSaveFileName(this, QT("Save map as image"), QDir::homePath() + "/tuxdirstat.png",
                                             QT("PNG images (*.png);;All files (*)"));
    if (f.isEmpty())
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    std::string err;
    // The image follows what is shown: zoomed folder, colors and shape, at twice the widget size.
    Node *root = treemap_->root() ? treemap_->root() : rootNode();
    const bool ring = settings_.sunburst;
    const int w = ring ? 1600 : std::max(1600, treemap_->width() * 2), h = ring ? 1600 : std::max(1000, treemap_->height() * 2);
    const bool ok = exportMapPng(root, f.toStdString(), w, h, session_.apparent,
                                 settings_.colorByAge ? ColorMode::Age : ColorMode::Extension,
                                 ring ? MapShape::Rings : MapShape::Rectangles, &err);
    QApplication::restoreOverrideCursor();
    if (!ok)
        QMessageBox::warning(this, "TuxDirStat", QT("Could not write the file:") + "\n" + qs(err));
    else
        statusLeft_->setText(QTF("Exported: %1", {f.toStdString()}));
}

void MainWindow::updateRecordAction()
{
    if (!actRecord_)
        return;
    const bool ok = localTree();
    actRecord_->setEnabled(ok);
    const QSignalBlocker block(actRecord_);
    actRecord_->setChecked(ok && historyRecording(rootNode()->path()));
}

void MainWindow::setRecording(bool on)
{
    if (!localTree())
        return;
    const std::string path = rootNode()->path();
    std::string err;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    bool ok = setHistoryRecording(path, on, &err);
    if (ok && on)
        ok = recordHistorySnapshot(rootNode(), &err);
    QApplication::restoreOverrideCursor();
    if (!ok)
        QMessageBox::warning(this, "TuxDirStat", qs(err));
    else
        statusLeft_->setText(on ? QTF("The size of %1 is now recorded every week.", {path}) : QTF("Stopped recording %1.", {path}));
    updateRecordAction();
}

void MainWindow::setSunburst(bool on)
{
    treemap_->setShape(on ? MapShape::Rings : MapShape::Rectangles);
    if (!applyingSettings_) {
        settings_.sunburst = on;
        saveSettings();
    }
}
