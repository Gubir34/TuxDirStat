#pragma once
// Araç takımından bağımsız gölgeli (cushion) squarified treemap:
// yerleşim, piksel çizimi, isabet testi ve vurgu/seçim katmanı.

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "core.h"

struct IRect {
    int x0, y0, x1, y1;
    int w() const { return x1 - x0; }
    int h() const { return y1 - y0; }
};

// Map shape: squarified rectangles (treemap) or concentric rings (sunburst), where the
// center is the root and each ring one level deeper.
enum class MapShape { Rectangles, Rings };

class TreemapRenderer {
public:
    static constexpr int RING_LEVELS = 7;  // rings drawn around the center
    // Pikseller 0xFFRRGGBB biçimindedir (QImage::Format_RGB32 / CAIRO_FORMAT_RGB24 ile aynı).
    void render(Node *root, int width, int height, bool apparent, const std::vector<RGBf> &colors);
    // Sonraki render() için renklendirme ölçütü (varsayılan: uzantı).
    void setColorMode(ColorMode m) { mode_ = m; }
    ColorMode colorMode() const { return mode_; }
    void setShape(MapShape s) { shape_ = s; }
    MapShape shape() const { return shape_; }
    // Düğümler silinmeden önce çağrılır: eski düğüm işaretçilerini bırakır, görüntü kalır.
    void forgetNodes();
    void clear();

    int width() const { return w_; }
    int height() const { return h_; }
    bool empty() const { return pixels_.empty(); }
    Node *hit(int x, int y) const;
    bool rectOf(const Node *n, IRect &r) const;  // n ya da çizilmiş en yakın atası
    void compose(std::vector<uint32_t> &out, int highlightExt, const Node *selected) const;

private:
    struct Leaf {
        int x0, y0, x1, y1;
        double s0, s1, s2, s3;
        RGBf color;
        Node *node;
    };

    void layout(Node *root, bool apparent, const std::vector<RGBf> &colors);
    void paint();
    void layoutRings(Node *root, bool apparent, const std::vector<RGBf> &colors);
    void paintRings();
    void outline(std::vector<uint32_t> &out, const std::vector<char> &in, bool contrast) const;

    std::vector<Leaf> leaves_;
    std::unordered_map<const Node *, IRect> rects_;
    std::vector<int32_t> idx_;
    std::vector<uint32_t> pixels_;
    const Node *root_ = nullptr;
    ColorMode mode_ = ColorMode::Extension;
    MapShape shape_ = MapShape::Rectangles;
    double r0_ = 0, ringW_ = 0;
    int w_ = 0, h_ = 0;
};
