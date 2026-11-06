#include "treemap_render.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <thread>

namespace {
constexpr double IA = 0.15;  // ortam ışığı
constexpr double IS = 1.0 - IA;
constexpr double CUSHION_H = 0.40;
constexpr double CUSHION_F = 0.90;
constexpr double BRIGHTNESS = 0.84;
const double LLEN = std::sqrt(1.0 + 1.0 + 100.0);
const double LX = -1 / LLEN, LY = -1 / LLEN, LZ = 10 / LLEN;
constexpr uint32_t BG = 0xFF1E1E1E;
constexpr uint32_t WHITE = 0xFFFFFFFF;
constexpr uint32_t BLACK = 0xFF000000;


inline uint32_t rgb(int r, int g, int b)
{
    return 0xFF000000u | uint32_t(r) << 16 | uint32_t(g) << 8 | uint32_t(b);
}

inline int clamp255(double v)
{
    return v <= 0 ? 0 : v >= 255 ? 255 : int(v);
}

struct RectD {
    double x, y, w, h;
};

// Azalan sıralı boyutları (x,y,w,h) dikdörtgenine yerleştirir.
void squarify(const std::vector<double> &sizes, double x, double y, double w, double h,
              std::vector<RectD> &out)
{
    out.clear();
    const size_t n = sizes.size();
    double remaining = 0;
    for (double s : sizes)
        remaining += s;
    size_t i = 0;
    while (i < n) {
        if (remaining <= 0 || w <= 0 || h <= 0) {
            out.resize(n, RectD{x, y, 0, 0});
            break;
        }
        const double scale = w * h / remaining;
        const double shortSide = std::min(w, h);
        const double short2 = shortSide * shortSide;
        const double first = sizes[i] * scale;
        double rsum = 0, best = INFINITY, units = 0;
        size_t j = i;
        while (j < n) {
            const double a = sizes[j] * scale;
            const double ns = rsum + a;
            const double ns2 = ns * ns;
            const double worst = std::max(short2 * first / ns2, ns2 / (short2 * a));
            if (j > i && worst > best)
                break;
            best = worst;
            rsum = ns;
            units += sizes[j];
            ++j;
        }
        if (w >= h) {
            const double thick = j == n ? w : rsum / shortSide;
            double yy = y;
            const double end = y + h;
            for (size_t k = i; k < j; ++k) {
                double hh = k == j - 1 ? end - yy : sizes[k] * scale / thick;
                out.push_back({x, yy, thick, hh});
                yy += hh;
            }
            x += thick;
            w -= thick;
        } else {
            const double thick = j == n ? h : rsum / shortSide;
            double xx = x;
            const double end = x + w;
            for (size_t k = i; k < j; ++k) {
                double ww = k == j - 1 ? end - xx : sizes[k] * scale / thick;
                out.push_back({xx, y, ww, thick});
                xx += ww;
            }
            y += thick;
            h -= thick;
        }
        remaining -= units;
        i = j;
    }
}

int dominantExt(const Node *n, bool apparent)
{
    while (n->isDir && !n->children.empty())
        n = *std::max_element(n->children.begin(), n->children.end(), [apparent](const Node *a, const Node *b) {
            return a->sizeOf(apparent) < b->sizeOf(apparent);
        });
    return n->isDir ? -1 : int(n->ext);
}

void hline(std::vector<uint32_t> &px, int W, int H, int x0, int x1, int y, uint32_t c)
{
    if (y < 0 || y >= H)
        return;
    x0 = std::max(x0, 0);
    x1 = std::min(x1, W);
    if (x0 < x1)
        std::fill(px.begin() + size_t(y) * W + x0, px.begin() + size_t(y) * W + x1, c);
}

void vline(std::vector<uint32_t> &px, int W, int H, int x, int y0, int y1, uint32_t c)
{
    if (x < 0 || x >= W)
        return;
    for (int y = std::max(y0, 0); y < std::min(y1, H); ++y)
        px[size_t(y) * W + x] = c;
}

// Dikdörtgenin içten `inset` piksel uzaklıktaki 1 piksellik çerçevesi.
void frame(std::vector<uint32_t> &px, int W, int H, const IRect &r, int inset, uint32_t c)
{
    const int x0 = r.x0 + inset, y0 = r.y0 + inset, x1 = r.x1 - 1 - inset, y1 = r.y1 - 1 - inset;
    if (x1 < x0 || y1 < y0)
        return;
    hline(px, W, H, x0, x1 + 1, y0, c);
    hline(px, W, H, x0, x1 + 1, y1, c);
    vline(px, W, H, x0, y0, y1 + 1, c);
    vline(px, W, H, x1, y0, y1 + 1, c);
}
}  // namespace

void TreemapRenderer::clear()
{
    forgetNodes();
    pixels_.clear();
    w_ = h_ = 0;
}

void TreemapRenderer::forgetNodes()
{
    leaves_.clear();
    rects_.clear();
    idx_.clear();
    root_ = nullptr;
}

void TreemapRenderer::render(Node *root, int width, int height, bool apparent, const std::vector<RGBf> &colors)
{
    clear();
    if (!root || width < 2 || height < 2 || root->sizeOf(apparent) == 0)
        return;
    w_ = width;
    h_ = height;
    root_ = root;
    if (shape_ == MapShape::Rings) {
        layoutRings(root, apparent, colors);
        paintRings();
        return;
    }
    layout(root, apparent, colors);
    paint();
}

// ───────────── rings (sunburst) ─────────────
// Leaf fields in ring mode: s0/s1 = start/end angle (radians, clockwise from the top),
// s2 = level (0 = center disc). x0…y1 are unused.

void TreemapRenderer::layoutRings(Node *root, bool ap, const std::vector<RGBf> &colors)
{
    const int64_t now = std::time(nullptr);
    const bool byAge = mode_ == ColorMode::Age;
    const double R = std::min(w_, h_) / 2.0 - 3;
    if (R < 8)
        return;
    // Rings share the radius by the depth actually drawn, so shallow trees fill the area too.
    int depth = 1;
    {
        std::vector<std::pair<const Node *, int>> dst{{root, 0}};
        while (!dst.empty() && depth < RING_LEVELS) {
            auto [n, l] = dst.back();
            dst.pop_back();
            for (const Node *c : n->children) {
                depth = std::max(depth, l + 1);
                if (c->isDir && !(c->flags & F_GROUP) && l + 1 < RING_LEVELS)
                    dst.push_back({c, l + 1});
            }
        }
    }
    r0_ = R * (depth <= 3 ? 0.24 : 0.17);
    ringW_ = (R - r0_) / depth;
    auto colorFor = [&colors](uint32_t ext) { return ext < colors.size() ? colors[ext] : dirColor(); };
    auto dirTint = [](RGBf c) {  // folders: the dominant color, softened toward gray
        const float g = 255 * PALETTE_BRIGHTNESS * 0.9f;
        return RGBf{c.r * 0.6f + g * 0.4f, c.g * 0.6f + g * 0.4f, c.b * 0.6f + g * 0.4f};
    };
    auto colorOf = [&](const Node *n) -> RGBf {
        if (byAge)
            return ageColor(n->mtime, now);
        if (!n->isDir || (n->flags & F_GROUP))
            return colorFor(n->ext);
        const int e = dominantExt(n, ap);
        return e >= 0 ? dirTint(colorFor(uint32_t(e))) : dirColor();
    };
    struct Item {
        Node *n;
        double a0, a1;
        int level;
    };
    std::vector<Item> st{{root, 0, 2 * M_PI, 0}};
    std::vector<Node *> kids;
    while (!st.empty()) {
        const Item it = st.back();
        st.pop_back();
        Leaf leaf{0, 0, 0, 0, it.a0, it.a1, double(it.level), 0, it.level == 0 ? dirColor() : colorOf(it.n), it.n};
        leaves_.push_back(leaf);
        if (!it.n->isDir || ((it.n->flags & F_GROUP) && it.n != root) || r0_ + (it.level + 1) * ringW_ > R + 0.5)
            continue;
        const double outer = r0_ + (it.level + 1) * ringW_;
        kids.clear();
        for (Node *c : it.n->children)
            if (c->sizeOf(ap) > 0)
                kids.push_back(c);
        std::sort(kids.begin(), kids.end(), [ap](const Node *a, const Node *b) { return a->sizeOf(ap) > b->sizeOf(ap); });
        const double total = double(it.n->sizeOf(ap));
        double a = it.a0;
        for (Node *c : kids) {
            const double span = (it.a1 - it.a0) * double(c->sizeOf(ap)) / total;
            if (span * outer < 1.2)  // thinner than a pixel: the rest is smaller still
                break;
            st.push_back({c, a, a + span, it.level + 1});
            a += span;
        }
    }
}

void TreemapRenderer::paintRings()
{
    const int W = w_, H = h_;
    idx_.assign(size_t(W) * H, -1);
    pixels_.assign(size_t(W) * H, BG);
    if (leaves_.empty())
        return;
    // Per level: segments sorted by start angle for binary search.
    std::vector<std::vector<std::pair<double, int32_t>>> levels(RING_LEVELS + 1);
    for (size_t i = 0; i < leaves_.size(); ++i)
        levels[size_t(leaves_[i].s2)].push_back({leaves_[i].s0, int32_t(i)});
    for (auto &l : levels)
        std::sort(l.begin(), l.end());
    const double cx = W / 2.0, cy = H / 2.0;
    const double k = BRIGHTNESS / PALETTE_BRIGHTNESS;
    auto work = [&](int ya, int yb) {
        for (int y = ya; y < yb; ++y) {
            for (int x = 0; x < W; ++x) {
                const double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
                const double r = std::sqrt(dx * dx + dy * dy);
                int level;
                double radial;
                if (r < r0_) {
                    level = 0;
                    radial = 1 - r / r0_;
                } else {
                    level = 1 + int((r - r0_) / ringW_);
                    if (level > RING_LEVELS)
                        continue;
                    radial = std::sin(M_PI * std::fmod(r - r0_, ringW_) / ringW_);
                }
                double ang = std::atan2(dx, -dy);
                if (ang < 0)
                    ang += 2 * M_PI;
                const auto &segs = levels[size_t(level)];
                auto p = std::upper_bound(segs.begin(), segs.end(), std::pair{ang, INT32_MAX});
                if (p == segs.begin())
                    continue;
                --p;
                const Leaf &l = leaves_[size_t(p->second)];
                if (ang >= l.s1)
                    continue;
                idx_[size_t(y) * W + x] = p->second;
                const double along = level ? std::sin(M_PI * (ang - l.s0) / std::max(l.s1 - l.s0, 1e-9)) : 1.0;
                const double pix = (IA + IS * (0.55 + 0.30 * radial + 0.15 * along)) * k;
                pixels_[size_t(y) * W + x] = rgb(clamp255(l.color.r * pix), clamp255(l.color.g * pix), clamp255(l.color.b * pix));
            }
        }
    };
    const int nt = std::clamp(int(std::thread::hardware_concurrency()), 1, 16);
    std::vector<std::thread> ts;
    const int chunk = (H + nt - 1) / nt;
    for (int t = 0; t < nt; ++t) {
        int ya = t * chunk, yb = std::min(H, ya + chunk);
        if (ya < yb)
            ts.emplace_back(work, ya, yb);
    }
    for (auto &t : ts)
        t.join();
    // Thin dark seams between neighboring segments.
    for (int y = 1; y < H; ++y)
        for (int x = 1; x < W; ++x) {
            const size_t i = size_t(y) * W + x;
            const int32_t v = idx_[i];
            if (v >= 0 && (idx_[i - 1] != v || idx_[i - W] != v)) {
                const uint32_t c = pixels_[i];
                pixels_[i] = rgb(int((c >> 16 & 255) * 0.55), int((c >> 8 & 255) * 0.55), int((c & 255) * 0.55));
            }
        }
}

// Draws the border of the pixel region whose leaves are marked in `in`: white inside,
// and with `contrast` also black just outside so it reads on every color.
void TreemapRenderer::outline(std::vector<uint32_t> &out, const std::vector<char> &in, bool contrast) const
{
    const int W = w_, H = h_;
    auto inside = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= W || y >= H)
            return false;
        const int32_t i = idx_[size_t(y) * W + x];
        return i >= 0 && in[size_t(i)];
    };
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const bool me = inside(x, y);
            const bool edge = inside(x - 1, y) != me || inside(x + 1, y) != me || inside(x, y - 1) != me || inside(x, y + 1) != me;
            if (!edge)
                continue;
            if (me)
                out[size_t(y) * W + x] = WHITE;
            else if (contrast)
                out[size_t(y) * W + x] = BLACK;
        }
}

void TreemapRenderer::layout(Node *root, bool ap, const std::vector<RGBf> &colors)
{
    struct Item {
        Node *n;
        double x, y, w, h, s0, s1, s2, s3, ht;
    };
    const int64_t now = std::time(nullptr);
    const bool byAge = mode_ == ColorMode::Age;
    auto colorFor = [&colors](uint32_t ext) { return ext < colors.size() ? colors[ext] : dirColor(); };
    auto nodeColor = [&](const Node *n) { return byAge ? ageColor(n->mtime, now) : colorFor(n->ext); };
    std::vector<Item> stack{{root, 0, 0, double(w_), double(h_), 0, 0, 0, 0, CUSHION_H}};
    std::vector<Node *> kids;
    std::vector<double> sizes;
    std::vector<RectD> rects;
    while (!stack.empty()) {
        Item it = stack.back();
        stack.pop_back();
        const int x0 = int(std::lround(it.x)), y0 = int(std::lround(it.y));
        const int x1 = int(std::lround(it.x + it.w)), y1 = int(std::lround(it.y + it.h));
        if (x1 <= x0 || y1 <= y0)
            continue;
        rects_[it.n] = IRect{x0, y0, x1, y1};
        double f = 4 * it.ht / it.w;
        it.s2 += f * (2 * it.x + it.w);
        it.s0 -= f;
        f = 4 * it.ht / it.h;
        it.s3 += f * (2 * it.y + it.h);
        it.s1 -= f;
        Leaf leaf{x0, y0, x1, y1, it.s0, it.s1, it.s2, it.s3, dirColor(), it.n};
        // Grup tek blok olarak çizilir; içine yakınlaşıldığında dosyaları görünür.
        if (!it.n->isDir || ((it.n->flags & F_GROUP) && it.n != root)) {
            leaf.color = nodeColor(it.n);
            leaves_.push_back(leaf);
            continue;
        }
        if ((x1 - x0) * (y1 - y0) <= 4) {
            if (byAge) {
                leaf.color = nodeColor(it.n);
            } else if (int e = dominantExt(it.n, ap); e >= 0) {
                leaf.color = colorFor(uint32_t(e));
            }
            leaves_.push_back(leaf);
            continue;
        }
        leaves_.push_back(leaf);  // boşlukları dolduran klasör zemini
        kids.clear();
        for (Node *c : it.n->children)
            if (c->sizeOf(ap) > 0)
                kids.push_back(c);
        if (kids.empty())
            continue;
        std::sort(kids.begin(), kids.end(), [ap](const Node *a, const Node *b) {
            return a->sizeOf(ap) > b->sizeOf(ap);
        });
        sizes.resize(kids.size());
        for (size_t i = 0; i < kids.size(); ++i)
            sizes[i] = double(kids[i]->sizeOf(ap));
        squarify(sizes, it.x, it.y, it.w, it.h, rects);
        const double nh = it.ht * CUSHION_F;
        for (size_t i = 0; i < kids.size(); ++i) {
            const RectD &r = rects[i];
            stack.push_back({kids[i], r.x, r.y, r.w, r.h, it.s0, it.s1, it.s2, it.s3, nh});
        }
    }
}

void TreemapRenderer::paint()
{
    const int W = w_, H = h_;
    idx_.assign(size_t(W) * H, -1);
    for (size_t i = 0; i < leaves_.size(); ++i) {
        const Leaf &l = leaves_[i];
        for (int y = l.y0; y < l.y1; ++y) {
            int32_t *row = idx_.data() + size_t(y) * W;
            std::fill(row + l.x0, row + l.x1, int32_t(i));
        }
    }
    pixels_.resize(size_t(W) * H);
    const double k = BRIGHTNESS / PALETTE_BRIGHTNESS;
    auto work = [&](int ya, int yb) {
        for (int y = ya; y < yb; ++y) {
            uint32_t *line = pixels_.data() + size_t(y) * W;
            const int32_t *row = idx_.data() + size_t(y) * W;
            const double fy = y + 0.5;
            for (int x = 0; x < W; ++x) {
                const int32_t i = row[x];
                if (i < 0) {
                    line[x] = BG;
                    continue;
                }
                const Leaf &l = leaves_[i];
                const double nx = -(2 * l.s0 * (x + 0.5) + l.s2);
                const double ny = -(2 * l.s1 * fy + l.s3);
                double cosa = (nx * LX + ny * LY + LZ) / std::sqrt(nx * nx + ny * ny + 1.0);
                if (cosa > 1)
                    cosa = 1;
                double pix = IS * cosa;
                if (pix < 0)
                    pix = 0;
                pix = (pix + IA) * k;
                line[x] = rgb(clamp255(l.color.r * pix), clamp255(l.color.g * pix), clamp255(l.color.b * pix));
            }
        }
    };
    const int nt = std::clamp(int(std::thread::hardware_concurrency()), 1, 16);
    std::vector<std::thread> ts;
    const int chunk = (H + nt - 1) / nt;
    for (int t = 0; t < nt; ++t) {
        int ya = t * chunk, yb = std::min(H, ya + chunk);
        if (ya < yb)
            ts.emplace_back(work, ya, yb);
    }
    for (auto &t : ts)
        t.join();
}

Node *TreemapRenderer::hit(int x, int y) const
{
    if (idx_.empty() || x < 0 || y < 0 || x >= w_ || y >= h_)
        return nullptr;
    const int32_t i = idx_[size_t(y) * w_ + x];
    return i >= 0 ? leaves_[i].node : nullptr;
}

bool TreemapRenderer::rectOf(const Node *n, IRect &r) const
{
    for (; n; n = n->parent) {
        auto it = rects_.find(n);
        if (it != rects_.end()) {
            r = it->second;
            return true;
        }
    }
    return false;
}

void TreemapRenderer::compose(std::vector<uint32_t> &out, int highlightExt, const Node *selected) const
{
    out = pixels_;
    if (out.empty())
        return;
    const int W = w_, H = h_;
    if (shape_ == MapShape::Rings) {
        if (highlightExt >= 0) {
            std::vector<char> in(leaves_.size());
            for (size_t i = 0; i < leaves_.size(); ++i) {
                const Node *n = leaves_[i].node;
                in[i] = (!n->isDir || (n->flags & F_GROUP)) && int(n->ext) == highlightExt;
            }
            outline(out, in, false);
        }
        if (selected && selected != root_) {
            std::vector<char> in(leaves_.size());
            bool any = false;
            for (size_t i = 0; i < leaves_.size(); ++i)
                any |= (in[i] = leaves_[i].node == selected || leaves_[i].node->isWithin(selected));
            if (!any)  // not drawn (too small or too deep): mark its nearest drawn ancestor
                for (const Node *a = selected->parent; a && !any; a = a->parent)
                    for (size_t i = 0; i < leaves_.size(); ++i)
                        if (leaves_[i].node == a && a != root_) {
                            in[i] = 1;
                            any = true;
                        }
            outline(out, in, true);
        }
        return;
    }
    if (highlightExt >= 0) {
        for (const Leaf &l : leaves_) {
            // Dosyalar ve aynı uzantıdaki grup blokları işaretlenir.
            if ((l.node->isDir && !(l.node->flags & F_GROUP)) || int(l.node->ext) != highlightExt)
                continue;
            const IRect r{l.x0, l.y0, l.x1, l.y1};
            if (r.w() > 2 && r.h() > 2) {
                frame(out, W, H, r, 0, WHITE);
            } else {
                for (int y = r.y0; y < r.y1; ++y)
                    hline(out, W, H, r.x0, r.x1, y, WHITE);
            }
        }
    }
    // Seçim: siyah dış ve beyaz iç çerçeve her renk üzerinde okunur. Tüm alanı
    // kaplayan kök (ya da yakınlaşılan klasör) seçiliyken çerçeve gereksizdir.
    IRect r;
    if (selected && selected != root_ && rectOf(selected, r) && !(r.w() >= W && r.h() >= H)) {
        frame(out, W, H, r, 0, BLACK);
        frame(out, W, H, r, 1, WHITE);
        frame(out, W, H, r, 2, BLACK);
    }
}
