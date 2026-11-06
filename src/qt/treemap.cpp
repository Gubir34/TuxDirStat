#include "treemap.h"

#include "theme.h"

#include <QMouseEvent>
#include <QPainter>

Treemap::Treemap(QWidget *parent) : QWidget(parent)
{
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, &Treemap::rebuild);
    setMouseTracking(true);
    setMinimumHeight(80);
}

void Treemap::setRoot(Node *n)
{
    invalidate();
    root_ = n;
    schedule();
}

void Treemap::setApparent(bool a)
{
    apparent_ = a;
    schedule();
}

void Treemap::setColors(std::vector<RGBf> colors)
{
    colors_ = std::move(colors);
    schedule();
}

void Treemap::setSelected(Node *n)
{
    selected_ = n;
    compose();
}

void Treemap::setColorMode(ColorMode m)
{
    renderer_.setColorMode(m);
    schedule();
}

void Treemap::setShape(MapShape s)
{
    renderer_.setShape(s);
    schedule();
}

void Treemap::setHighlight(int ext)
{
    highlight_ = ext;
    compose();
}

void Treemap::invalidate()
{
    renderer_.forgetNodes();
    hover_ = nullptr;
}

void Treemap::rebuild()
{
    const double dpr = devicePixelRatioF();
    renderer_.render(root_, int(width() * dpr), int(height() * dpr), apparent_, colors_);
    compose();
}

void Treemap::compose()
{
    renderer_.compose(pixels_, highlight_, selected_);
    if (pixels_.empty()) {
        view_ = QImage();
    } else {
        view_ = QImage(reinterpret_cast<const uchar *>(pixels_.data()), renderer_.width(), renderer_.height(),
                       renderer_.width() * 4, QImage::Format_RGB32);
        view_.setDevicePixelRatio(devicePixelRatioF());
    }
    update();
}

void Treemap::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    if (!view_.isNull()) {
        p.drawImage(QRectF(rect()), view_);
    } else {
        p.fillRect(rect(), palette().window());
        if (!root_) {
            p.setPen(palette().placeholderText().color());
            p.drawText(rect(), Qt::AlignCenter, QT("The treemap will appear here"));
        }
    }
}

void Treemap::resizeEvent(QResizeEvent *e)
{
    schedule(150);
    QWidget::resizeEvent(e);
}

Node *Treemap::nodeAt(const QPointF &pos) const
{
    if (width() <= 0 || height() <= 0)
        return nullptr;
    return renderer_.hit(int(pos.x() / width() * renderer_.width()), int(pos.y() / height() * renderer_.height()));
}

void Treemap::mouseMoveEvent(QMouseEvent *e)
{
    Node *n = nodeAt(e->position());
    if (n != hover_) {
        hover_ = n;
        emit nodeHovered(n);
    }
}

void Treemap::leaveEvent(QEvent *)
{
    hover_ = nullptr;
    emit nodeHovered(nullptr);
}

void Treemap::mousePressEvent(QMouseEvent *e)
{
    Node *n = nodeAt(e->position());
    if (!n)
        return;
    emit nodeClicked(n);
    if (e->button() == Qt::RightButton)
        emit contextRequested(n, e->globalPosition().toPoint());
}

void Treemap::mouseDoubleClickEvent(QMouseEvent *e)
{
    Node *n = nodeAt(e->position());
    if (!n || !root_)
        return;
    while (n && n->parent != root_)
        n = n->parent;
    if (n && n->isDir)
        emit zoomRequested(n);
}
