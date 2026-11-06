#include "treemodel.h"
#include "theme.h"

#include <algorithm>

#include <QApplication>
#include <QColor>
#include <QLinearGradient>
#include <QPainter>
#include <QStyle>

static const char *const COLS[] = {N_("Name"), N_("Subtree"), "%", N_("Size"), N_("Items"), N_("Files"), N_("Folders"), N_("Modified")};

TreeModel::TreeModel(QObject *parent) : QAbstractItemModel(parent)
{
    QStyle *st = QApplication::style();
    dirIcon_ = QIcon::fromTheme("folder", st->standardIcon(QStyle::SP_DirIcon));
    fileIcon_ = QIcon::fromTheme("text-x-generic", st->standardIcon(QStyle::SP_FileIcon));
    linkIcon_ = QIcon::fromTheme("emblem-symbolic-link", fileIcon_);
    groupIcon_ = QIcon::fromTheme("package-x-generic", dirIcon_);
}

// ───────────── sıralama ─────────────

void TreeModel::sortList(Kids &v) const
{
    static const SortKey keys[] = {SortKey::Name, SortKey::Size, SortKey::Size, SortKey::Size,
                                   SortKey::Items, SortKey::Files, SortKey::Dirs, SortKey::MTime};
    sortKids(v, keys[sortCol_], sortOrder_ == Qt::DescendingOrder, apparent_);
}

void TreeModel::sortNode(Node *n) const
{
    sortList(n->children);
    n->sgen = gen_;
}

const Kids &TreeModel::kids(Node *n) const
{
    if (n->sgen != gen_) {
        sortNode(n);
        sorted_.insert(n);
    }
    return n->children;
}

void TreeModel::resort()
{
    emit layoutAboutToBeChanged();
    const QModelIndexList old = persistentIndexList();
    std::vector<std::pair<Node *, int>> refs;
    refs.reserve(old.size());
    for (const QModelIndex &i : old)
        refs.emplace_back(nodeAt(i), i.column());
    ++gen_;
    for (Node *n : sorted_)
        sortNode(n);
    QModelIndexList now;
    now.reserve(old.size());
    for (auto &[n, c] : refs)
        now.append(createIndex(n == root_ ? 0 : n->row, c, n));
    changePersistentIndexList(old, now);
    emit layoutChanged();
}

void TreeModel::sort(int column, Qt::SortOrder order)
{
    sortCol_ = column;
    sortOrder_ = order;
    if (root_)
        resort();
}

void TreeModel::setApparent(bool a)
{
    apparent_ = a;
    if (root_)
        resort();
}

void TreeModel::setRoot(Node *root)
{
    beginResetModel();
    root_ = root;
    sorted_.clear();
    ++gen_;
    if (root)
        root->row = 0;
    endResetModel();
}

void TreeModel::forget(Node *subtree)
{
    std::vector<Node *> st{subtree};
    while (!st.empty()) {
        Node *n = st.back();
        st.pop_back();
        sorted_.erase(n);
        for (Node *c : n->children)
            if (c->isDir)
                st.push_back(c);
    }
}

QModelIndex TreeModel::indexFor(Node *node, int col)
{
    if (!node || !root_)
        return {};
    std::vector<Node *> chain;
    Node *n = node;
    while (n && n != root_) {
        chain.push_back(n);
        n = n->parent;
    }
    if (!n)
        return {};
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        kids((*it)->parent);
    return createIndex(node == root_ ? 0 : node->row, col, node);
}

void TreeModel::removeNode(Node *n, const std::function<void()> &detach)
{
    Node *p = n->parent;
    QModelIndex pidx = indexFor(p);
    kids(p);
    forget(n);
    beginRemoveRows(pidx, n->row, n->row);
    detach();
    endRemoveRows();
}

void TreeModel::replaceChildren(Node *node, Node *fresh)
{
    QModelIndex idx = indexFor(node);
    if (!node->children.empty()) {
        beginRemoveRows(idx, 0, int(node->children.size()) - 1);
        for (Node *c : node->children)
            forget(c);
        node->children = Kids{};
        endRemoveRows();
    }
    sortList(fresh->children);
    const int n = int(fresh->children.size());
    if (n)
        beginInsertRows(idx, 0, n - 1);
    takeChildren(node, fresh);
    node->sgen = gen_;
    sorted_.insert(node);
    if (n)
        endInsertRows();
}

// ───────────── Qt arayüzü ─────────────

QModelIndex TreeModel::index(int row, int col, const QModelIndex &parent) const
{
    if (!parent.isValid())
        return (row == 0 && root_) ? createIndex(0, col, root_) : QModelIndex();
    const auto &k = kids(nodeAt(parent));
    if (row >= 0 && row < int(k.size()))
        return createIndex(row, col, k[row]);
    return {};
}

QModelIndex TreeModel::parent(const QModelIndex &index) const
{
    if (!index.isValid())
        return {};
    Node *n = nodeAt(index);
    if (n == root_ || !n->parent)
        return {};
    Node *p = n->parent;
    return createIndex(p == root_ ? 0 : p->row, 0, p);
}

int TreeModel::rowCount(const QModelIndex &parent) const
{
    if (parent.column() > 0)
        return 0;
    if (!parent.isValid())
        return root_ ? 1 : 0;
    return int(nodeAt(parent)->children.size());
}

int TreeModel::columnCount(const QModelIndex &) const
{
    return NCols;
}

bool TreeModel::hasChildren(const QModelIndex &parent) const
{
    if (!parent.isValid())
        return root_ != nullptr;
    if (parent.column() > 0)
        return false;
    return !nodeAt(parent)->children.empty();
}

QVariant TreeModel::headerData(int section, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal)
        return {};
    if (role == Qt::DisplayRole)
        return QT(COLS[section]);
    if (role == Qt::TextAlignmentRole && section >= Pct)
        return int(Qt::AlignRight | Qt::AlignVCenter);
    return {};
}

double TreeModel::frac(const Node *n) const
{
    if (n == root_ || !n->parent)
        return 1.0;
    uint64_t tot = n->parent->sizeOf(apparent_);
    return tot ? double(n->sizeOf(apparent_)) / double(tot) : 0.0;
}

QIcon TreeModel::icon(const Node *n) const
{
    if (n->flags & F_GROUP)
        return groupIcon_;
    if (n->isDir)
        return dirIcon_;
    if (n->flags & F_LINK)
        return linkIcon_;
    auto it = icons_.find(n->ext);
    if (it != icons_.end())
        return it->second;
    QIcon ic = fileIcon_;
    if (n->ext) {
        QMimeType mt = mime_.mimeTypeForFile(
            "x." + QString::fromStdString(ExtTable::name(n->ext)), QMimeDatabase::MatchExtension);
        ic = QIcon::fromTheme(mt.iconName(), QIcon::fromTheme(mt.genericIconName(), fileIcon_));
    }
    icons_.emplace(n->ext, ic);
    return ic;
}

QVariant TreeModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return {};
    const Node *n = nodeAt(index);
    const int col = index.column();
    switch (role) {
    case Qt::DisplayRole:
        switch (col) {
        case Name:
            return QString::fromStdString(displayName(n, n == root_));
        case Pct:
            return QString::fromStdString(fmtPct(frac(n)));
        case Size:
            return QString::fromStdString(fmtSize(n->sizeOf(apparent_)));
        case MTime:
            return QString::fromStdString(fmtTime(n->mtime));
        case Items:
            return n->isDir ? QString::fromStdString(fmtInt(uint64_t(n->files) + n->dirs)) : QVariant();
        case Files:
            return n->isDir ? QString::fromStdString(fmtInt(n->files)) : QVariant();
        case Dirs:
            return n->isDir ? QString::fromStdString(fmtInt(n->dirs)) : QVariant();
        }
        return {};
    case Qt::DecorationRole:
        return col == Name ? QVariant(icon(n)) : QVariant();
    case Qt::TextAlignmentRole:
        return col >= Pct ? QVariant(int(Qt::AlignRight | Qt::AlignVCenter)) : QVariant();
    case Qt::ForegroundRole:
        if (n->flags & F_ERROR)
            return QColor(220, 70, 70);
        if (n->flags & F_MOUNT)
            return QColor(140, 140, 140);
        return {};
    case Qt::ToolTipRole:
        return col == Name ? QVariant(QString::fromStdString(n->path())) : QVariant();
    case FracRole:
        return frac(n);
    case DepthRole:
        return n->depth();
    }
    return {};
}

// ───────────── Alt ağaç çubuğu ─────────────

void BarDelegate::paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const
{
    RowDelegate::paint(p, opt, index);
    const double frac = std::clamp(index.data(TreeModel::FracRole).toDouble(), 0.0, 1.0);
    const int depth = index.data(TreeModel::DepthRole).toInt();
    const QRectF r = QRectF(opt.rect).adjusted(6, 0, -6, 0);
    const double h = std::min(10.0, r.height() - 8);
    if (r.width() <= 4 || h <= 2)
        return;
    const QRectF bar(r.left(), r.center().y() - h / 2, r.width(), h);
    const bool selected = opt.state & QStyle::State_Selected;
    const bool dark = isDarkPalette();

    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setPen(Qt::NoPen);
    QColor trough = selected ? opt.palette.color(QPalette::HighlightedText) : opt.palette.color(QPalette::Text);
    trough.setAlphaF(selected ? 0.25f : (dark ? 0.08f : 0.07f));
    p->setBrush(trough);
    p->drawRoundedRect(bar, h / 2, h / 2);
    if (frac > 0) {
        // Vurgu renginin tonları: derinlik arttıkça biraz daha soluk.
        QColor c = selected ? opt.palette.color(QPalette::HighlightedText) : accentColor();
        float hue, sat, val, alpha;
        c.getHsvF(&hue, &sat, &val, &alpha);
        if (!selected)
            c = QColor::fromHsvF(hue, std::max(0.25f, sat - 0.12f * float(std::min(depth, 5))), val);
        QRectF fill = bar;
        fill.setWidth(std::max(h, bar.width() * frac));
        QLinearGradient g(fill.topLeft(), fill.bottomLeft());
        g.setColorAt(0, c.lighter(dark ? 115 : 108));
        g.setColorAt(1, c.darker(dark ? 105 : 112));
        p->setBrush(g);
        p->drawRoundedRect(fill, h / 2, h / 2);
    }
    p->restore();
}
