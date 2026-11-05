#pragma once

#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <QAbstractItemModel>
#include <QIcon>
#include <QMimeDatabase>
#include <QStyledItemDelegate>

#include "theme.h"

#include "core.h"

// Node ağacını QTreeView'a sunar. Çocuklar ilk istendiklerinde sıralanır.
class TreeModel : public QAbstractItemModel {
    Q_OBJECT
public:
    enum Col { Name, Bar, Pct, Size, Items, Files, Dirs, MTime, NCols };
    enum Role { FracRole = Qt::UserRole, DepthRole };

    explicit TreeModel(QObject *parent = nullptr);

    void setRoot(Node *root);  // sahiplik almaz
    Node *root() const { return root_; }
    bool apparent() const { return apparent_; }
    void setApparent(bool a);

    QModelIndex indexFor(Node *n, int col = 0);
    static Node *nodeAt(const QModelIndex &i) { return static_cast<Node *>(i.internalPointer()); }

    // Yapısal değişiklikler
    void removeNode(Node *n, const std::function<void()> &detach);  // detach satır bildirimleri arasında çalışır
    void replaceChildren(Node *node, Node *fresh);                   // fresh'in çocuklarını ve toplamlarını devralır

    QModelIndex index(int row, int col, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &index) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    bool hasChildren(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation o, int role = Qt::DisplayRole) const override;
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

private:
    const Kids &kids(Node *n) const;
    void sortList(Kids &v) const;
    void sortNode(Node *n) const;
    void resort();
    void forget(Node *subtree);
    double frac(const Node *n) const;
    QIcon icon(const Node *n) const;

    Node *root_ = nullptr;
    bool apparent_ = false;
    int sortCol_ = Size;
    Qt::SortOrder sortOrder_ = Qt::DescendingOrder;
    uint32_t gen_ = 1;
    mutable std::unordered_set<Node *> sorted_;
    mutable std::unordered_map<uint32_t, QIcon> icons_;
    QMimeDatabase mime_;
    QIcon dirIcon_, fileIcon_, linkIcon_, groupIcon_;
};

class BarDelegate : public RowDelegate {
    Q_OBJECT
public:
    using RowDelegate::RowDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const override;
};
