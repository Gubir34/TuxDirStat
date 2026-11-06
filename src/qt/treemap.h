#pragma once

#include <vector>

#include <QImage>
#include <QTimer>
#include <QWidget>

#include "core.h"
#include "treemap_render.h"

// TreemapRenderer'ı gösteren Qt bileşeni.
class Treemap : public QWidget {
    Q_OBJECT
public:
    explicit Treemap(QWidget *parent = nullptr);

    void setRoot(Node *n);
    Node *root() const { return root_; }
    void setApparent(bool a);
    void setColors(std::vector<RGBf> colors);
    void setSelected(Node *n);
    Node *selected() const { return selected_; }
    void setHighlight(int ext);  // -1: yok
    void setColorMode(ColorMode m);
    void setShape(MapShape s);
    void schedule(int ms = 0) { timer_.start(ms); }
    // Düğümler silinmeden önce çağrılır: eski düğümlere işaretçileri bırakır.
    void invalidate();

    QSize sizeHint() const override { return {800, 320}; }

signals:
    void nodeClicked(Node *n);
    void nodeHovered(Node *n);
    void zoomRequested(Node *n);
    void contextRequested(Node *n, const QPoint &globalPos);

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    void rebuild();
    void compose();
    Node *nodeAt(const QPointF &pos) const;

    TreemapRenderer renderer_;
    std::vector<uint32_t> pixels_;
    QImage view_;
    Node *root_ = nullptr;
    Node *selected_ = nullptr;
    Node *hover_ = nullptr;
    bool apparent_ = false;
    int highlight_ = -1;
    std::vector<RGBf> colors_;
    QTimer timer_;
};
