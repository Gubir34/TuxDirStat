#include "theme.h"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QStyleFactory>

namespace {

struct Original {
    QString style;
    QPalette palette;
    bool saved = false;
} original;

QPalette makePalette(bool dark)
{
    struct C {
        const char *window, *text, *base, *altBase, *button, *mid, *midlight, *dark, *shadow, *highlight, *placeholder,
            *disabled, *tooltip, *link;
    };
    const C c = dark ? C{"#1f2227", "#e3e5e8", "#181a1e", "#1d2025", "#2a2e35", "#3a3f47", "#30343b", "#141619",
                         "#0b0c0e", "#3d7eff", "#868c96", "#666c75", "#2a2e35", "#7aa7ff"}
                     : C{"#f2f3f5", "#1f2328", "#ffffff", "#f6f7f9", "#e8eaee", "#c4c8cf", "#dcdfe4", "#9da3ab",
                         "#6b7078", "#2f6fed", "#8a9099", "#9da3ab", "#ffffff", "#2f6fed"};
    QPalette p;
    p.setColor(QPalette::Window, QColor(c.window));
    p.setColor(QPalette::WindowText, QColor(c.text));
    p.setColor(QPalette::Base, QColor(c.base));
    p.setColor(QPalette::AlternateBase, QColor(c.altBase));
    p.setColor(QPalette::ToolTipBase, QColor(c.tooltip));
    p.setColor(QPalette::ToolTipText, QColor(c.text));
    p.setColor(QPalette::PlaceholderText, QColor(c.placeholder));
    p.setColor(QPalette::Text, QColor(c.text));
    p.setColor(QPalette::Button, QColor(c.button));
    p.setColor(QPalette::ButtonText, QColor(c.text));
    p.setColor(QPalette::BrightText, QColor("#ff6b6b"));
    p.setColor(QPalette::Light, QColor(c.midlight).lighter(110));
    p.setColor(QPalette::Midlight, QColor(c.midlight));
    p.setColor(QPalette::Mid, QColor(c.mid));
    p.setColor(QPalette::Dark, QColor(c.dark));
    p.setColor(QPalette::Shadow, QColor(c.shadow));
    p.setColor(QPalette::Highlight, QColor(c.highlight));
    p.setColor(QPalette::HighlightedText, QColor("#ffffff"));
    p.setColor(QPalette::Link, QColor(c.link));
    for (auto role : {QPalette::Text, QPalette::WindowText, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, QColor(c.disabled));
    p.setColor(QPalette::Disabled, QPalette::Highlight, QColor(c.mid));
    return p;
}

// Fusion ile kullanılan küçük rötuşlar: ferah başlıklar, sade kaydırma çubukları.
const char *const FUSION_STYLE = R"CSS(
QToolBar { spacing: 2px; padding: 3px 4px; border: none; }
QToolButton { padding: 4px 6px; border-radius: 5px; }
QHeaderView::section { padding: 5px 6px; border: none; border-right: 1px solid palette(midlight);
                       border-bottom: 1px solid palette(midlight); background: palette(window); }
QTreeView, QTableView { border: 1px solid palette(midlight); border-radius: 6px; }
QSplitter::handle { background: transparent; }
QStatusBar { border-top: 1px solid palette(midlight); }
QScrollBar:vertical { width: 10px; background: transparent; margin: 2px; }
QScrollBar:horizontal { height: 10px; background: transparent; margin: 2px; }
QScrollBar::handle { background: palette(mid); border-radius: 3px; min-height: 24px; min-width: 24px; }
QScrollBar::handle:hover { background: palette(dark); }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QMenu { padding: 4px; border: 1px solid palette(mid); }
QMenu::item { padding: 5px 24px 5px 22px; border-radius: 4px; }
QMenu::item:selected { background: palette(highlight); color: palette(highlighted-text); }
)CSS";

}  // namespace

void applyTheme(const std::string &theme)
{
    auto *app = qApp;
    if (!original.saved) {
        original.style = app->style()->name();
        original.palette = QGuiApplication::palette();
        original.saved = true;
    }
    if (theme == "light" || theme == "dark") {
        app->setStyle(QStyleFactory::create("Fusion"));
        app->setPalette(makePalette(theme == "dark"));
        app->setStyleSheet(FUSION_STYLE);
    } else {
        app->setStyleSheet(QString());
        app->setStyle(QStyleFactory::create(original.style));
        app->setPalette(original.palette);
    }
}

bool isDarkPalette()
{
    return QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
}

QColor accentColor()
{
    // Bazı koyu temalarda (ör. Breeze Koyu) vurgu rengi çubuklar için fazla koyudur.
    QColor c = QGuiApplication::palette().color(QPalette::Highlight);
    if (isDarkPalette() && c.lightness() < 120)
        c = c.lighter(160);
    return c;
}

QSize RowDelegate::sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &index) const
{
    QSize s = QStyledItemDelegate::sizeHint(opt, index);
    s.setHeight(std::max(s.height() + 6, 26));
    return s;
}

void ShareDelegate::paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const
{
    const double frac = std::clamp(index.data(ShareRole).toDouble(), 0.0, 1.0);
    if (frac > 0 && !(opt.state & QStyle::State_Selected)) {
        QColor c = accentColor();
        c.setAlphaF(isDarkPalette() ? 0.28f : 0.18f);
        QRectF r = QRectF(opt.rect).adjusted(2, 4, -2, -4);
        r.setWidth(std::max(2.0, r.width() * frac));
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(Qt::NoPen);
        p->setBrush(c);
        p->drawRoundedRect(r, 3, 3);
        p->restore();
    }
    RowDelegate::paint(p, opt, index);
}
