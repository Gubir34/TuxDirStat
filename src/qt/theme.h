#pragma once
// Uygulama teması: "system" masaüstünün yerel görünümü; "light"/"dark" Fusion stili
// üzerine TuxDirStat'ın kendi paletleri.

#include <string>

#include <QColor>
#include <QStyledItemDelegate>

void applyTheme(const std::string &theme);
bool isDarkPalette();
QColor accentColor();

// Tüm satırlara biraz nefes payı veren temel temsilci.
class RowDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &index) const override;
};

// Hücrenin arkasına oran çubuğu çizer; oran Qt::UserRole + 10'dan okunur.
class ShareDelegate : public RowDelegate {
    Q_OBJECT
public:
    using RowDelegate::RowDelegate;
    static constexpr int ShareRole = Qt::UserRole + 10;
    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const override;
};
