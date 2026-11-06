// Output files: gzip / stdout streams, PNG images of the map and the HTML report.

#include "tools.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <zlib.h>

#include "extinfo.h"
#include "i18n.h"
#include "session.h"
#include "treemap_render.h"

#ifndef TUXDIRSTAT_VERSION
#define TUXDIRSTAT_VERSION "dev"
#endif

// ───────────────────────────── output streams ─────────────────────────────

namespace {

bool endsWith(const std::string &s, const char *suf)
{
    const size_t n = std::strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

ssize_t gzWriteCookie(void *c, const char *buf, size_t n)
{
    const int w = gzwrite(static_cast<gzFile>(c), buf, unsigned(n));
    return w > 0 ? w : (n == 0 ? 0 : -1);
}

int gzCloseCookie(void *c)
{
    return gzclose(static_cast<gzFile>(c)) == Z_OK ? 0 : -1;
}

}  // namespace

std::FILE *openOutput(const std::string &file, std::string *err)
{
    if (file == "-")
        return stdout;
    if (endsWith(file, ".gz")) {
        gzFile g = gzopen(file.c_str(), "wb6");
        if (!g) {
            if (err)
                *err = std::strerror(errno ? errno : ENOMEM);
            return nullptr;
        }
        cookie_io_functions_t io{nullptr, gzWriteCookie, nullptr, gzCloseCookie};
        std::FILE *f = fopencookie(g, "w", io);
        if (!f)
            gzclose(g);
        return f;
    }
    std::FILE *f = std::fopen(file.c_str(), "w");
    if (!f && err)
        *err = std::strerror(errno);
    return f;
}

bool closeOutput(std::FILE *f, std::string *err)
{
    const bool ok = !std::ferror(f);
    const int rc = f == stdout ? std::fflush(f) : std::fclose(f);
    if (rc != 0 || !ok) {
        if (err)
            *err = std::strerror(errno ? errno : EIO);
        return false;
    }
    return true;
}

// ───────────────────────────── PNG ─────────────────────────────

namespace {

void put32(std::string &s, uint32_t v)
{
    s += char(v >> 24);
    s += char(v >> 16);
    s += char(v >> 8);
    s += char(v);
}

void chunk(std::string &png, const char *type, const std::string &data)
{
    put32(png, uint32_t(data.size()));
    std::string body(type, 4);
    body += data;
    png += body;
    put32(png, uint32_t(crc32(0, reinterpret_cast<const Bytef *>(body.data()), uInt(body.size()))));
}

}  // namespace

bool encodePng(const uint32_t *px, int w, int h, std::string &png)
{
    png.assign("\x89PNG\r\n\x1a\n", 8);
    std::string ihdr;
    put32(ihdr, uint32_t(w));
    put32(ihdr, uint32_t(h));
    ihdr += std::string("\x08\x02\x00\x00\x00", 5);  // 8 bit RGB
    chunk(png, "IHDR", ihdr);
    // Rows with the "Sub" filter: each byte minus the same channel of the pixel to its left.
    std::string raw;
    raw.reserve(size_t(h) * (size_t(w) * 3 + 1));
    for (int y = 0; y < h; ++y) {
        raw += char(1);
        uint32_t prev = 0;
        for (int x = 0; x < w; ++x) {
            const uint32_t c = px[size_t(y) * w + x];
            for (int sh : {16, 8, 0})
                raw += char(((c >> sh) - (prev >> sh)) & 255);
            prev = c;
        }
    }
    uLongf len = compressBound(uLong(raw.size()));
    std::string z(len, '\0');
    if (compress2(reinterpret_cast<Bytef *>(z.data()), &len, reinterpret_cast<const Bytef *>(raw.data()), uLong(raw.size()), 6) != Z_OK)
        return false;
    z.resize(len);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    return true;
}

bool writePng(const std::string &file, const uint32_t *px, int w, int h, std::string *err)
{
    std::string png;
    if (!encodePng(px, w, h, png)) {
        if (err)
            *err = T("Could not compress the image.");
        return false;
    }
    std::FILE *f = file == "-" ? stdout : std::fopen(file.c_str(), "wb");
    if (!f) {
        if (err)
            *err = std::strerror(errno);
        return false;
    }
    std::fwrite(png.data(), 1, png.size(), f);
    return closeOutput(f, err);
}

std::vector<uint32_t> renderMap(Node *root, int w, int h, bool apparent, ColorMode mode, MapShape shape)
{
    std::vector<ExtStat> stats = computeExtStats(root);
    std::vector<RGBf> colors;
    computeExtColors(stats, apparent, colors, nullptr);
    TreemapRenderer r;
    r.setColorMode(mode);
    r.setShape(shape);
    r.render(root, w, h, apparent, colors);
    std::vector<uint32_t> px;
    r.compose(px, -1, nullptr);
    if (px.empty())
        px.assign(size_t(w) * h, 0xFF1E1E1E);
    return px;
}

bool exportMapPng(Node *root, const std::string &file, int w, int h, bool apparent, ColorMode mode, MapShape shape,
                  std::string *err)
{
    w = std::clamp(w, 16, 16384);
    h = std::clamp(h, 16, 16384);
    const std::vector<uint32_t> px = renderMap(root, w, h, apparent, mode, shape);
    return writePng(file, px.data(), w, h, err);
}

// ───────────────────────────── HTML report ─────────────────────────────

namespace {

std::string esc(std::string_view s)
{
    std::string o;
    for (char c : s) {
        switch (c) {
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '&': o += "&amp;"; break;
        case '"': o += "&quot;"; break;
        default: o += c;
        }
    }
    return o;
}

std::string base64(const std::string &in)
{
    static const char *tab = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t v = uint32_t(uint8_t(in[i])) << 16 | uint32_t(uint8_t(in[i + 1])) << 8 | uint8_t(in[i + 2]);
        o += tab[v >> 18];
        o += tab[v >> 12 & 63];
        o += tab[v >> 6 & 63];
        o += tab[v & 63];
    }
    if (i < in.size()) {
        uint32_t v = uint32_t(uint8_t(in[i])) << 16;
        if (i + 1 < in.size())
            v |= uint32_t(uint8_t(in[i + 1])) << 8;
        o += tab[v >> 18];
        o += tab[v >> 12 & 63];
        o += i + 1 < in.size() ? tab[v >> 6 & 63] : '=';
        o += '=';
    }
    return o;
}

std::string pngData(Node *root, int w, int h, bool apparent, MapShape shape)
{
    const std::vector<uint32_t> px = renderMap(root, w, h, apparent, ColorMode::Extension, shape);
    std::string png;
    if (!encodePng(px.data(), w, h, png))
        return {};
    return "data:image/png;base64," + base64(png);
}

std::string hexColor(const RGBf &c)
{
    const double k = 0.84 / PALETTE_BRIGHTNESS;  // as bright as the map's cushions
    char b[8];
    std::snprintf(b, sizeof b, "#%02x%02x%02x", std::min(255, int(c.r * k)), std::min(255, int(c.g * k)), std::min(255, int(c.b * k)));
    return b;
}

}  // namespace

bool exportHtml(Node *root, const std::string &file, bool apparent, std::string *err)
{
    const bool ap = apparent;
    const uint64_t total = std::max<uint64_t>(1, root->sizeOf(ap));
    const std::string base = root->path();
    auto rel = [&](const Node *n) {
        const std::string p = n->path();
        return p.size() > base.size() ? p.substr(base.size() + (base == "/" ? 0 : 1)) : std::string(".");
    };
    std::vector<Node *> top, files;
    realChildren(root, top);
    std::vector<Node *> st{root};
    while (!st.empty()) {
        Node *d = st.back();
        st.pop_back();
        for (Node *c : d->children) {
            if (c->isDir)
                st.push_back(c);
            else if (!(c->flags & F_GROUP))
                files.push_back(c);
        }
    }
    auto bySize = [ap](const Node *a, const Node *b) { return a->sizeOf(ap) > b->sizeOf(ap); };
    std::sort(top.begin(), top.end(), bySize);
    if (top.size() > 25)
        top.resize(25);
    const size_t nf = std::min<size_t>(25, files.size());
    std::partial_sort(files.begin(), files.begin() + long(nf), files.end(), bySize);
    files.resize(nf);
    const std::vector<ExtStat> stats = computeExtStats(root);
    std::vector<RGBf> colors;
    std::vector<ExtRow> types;
    computeExtColors(stats, ap, colors, &types);
    if (types.size() > 20)
        types.resize(20);

    std::FILE *f = openOutput(file, err);
    if (!f)
        return false;
    char date[64];
    const std::time_t now = std::time(nullptr);
    std::strftime(date, sizeof date, i18n::dateFormat(), std::localtime(&now));
    std::string h;
    h += "<!DOCTYPE html>\n<html lang=\"" + std::string(i18n::code()) + "\">\n<head>\n<meta charset=\"utf-8\">\n";
    h += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
    h += "<meta name=\"generator\" content=\"TuxDirStat " TUXDIRSTAT_VERSION "\">\n";
    h += "<title>" + esc(TF("TuxDirStat report: %1", {base})) + "</title>\n<style>\n"
         ":root{color-scheme:light dark;--fg:#1d1d1f;--bg:#fafafa;--card:#fff;--line:#e3e3e6;--muted:#6e6e73;--bar:#3d7bd9}\n"
         "@media (prefers-color-scheme:dark){:root{--fg:#ececf0;--bg:#18181b;--card:#222226;--line:#34343a;--muted:#a0a0a8;--bar:#5b93e8}}\n"
         "body{font:15px/1.45 system-ui,sans-serif;color:var(--fg);background:var(--bg);margin:0;padding:24px;max-width:1240px;margin:auto}\n"
         "h1{font-size:22px;margin:0 0 4px}h2{font-size:17px;margin:28px 0 10px}.muted{color:var(--muted)}\n"
         ".stats{display:flex;gap:12px;flex-wrap:wrap;margin:16px 0}.stat{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:10px 14px}\n"
         ".stat b{display:block;font-size:20px}.maps{display:flex;gap:12px;flex-wrap:wrap}.maps img{border-radius:6px;max-width:100%;height:auto}\n"
         "table{border-collapse:collapse;width:100%;background:var(--card);border:1px solid var(--line);border-radius:8px;overflow:hidden}\n"
         "td,th{padding:6px 10px;border-bottom:1px solid var(--line);text-align:left;vertical-align:middle}th{font-weight:600;color:var(--muted)}\n"
         "td.n{text-align:right;white-space:nowrap;font-variant-numeric:tabular-nums}td.p{word-break:break-all}\n"
         ".bar{height:8px;border-radius:4px;background:var(--bar);min-width:1px}.sw{display:inline-block;width:12px;height:12px;border-radius:3px;margin-right:6px;vertical-align:-1px}\n"
         "footer{margin-top:28px;font-size:13px}\n</style>\n</head>\n<body>\n";
    h += "<h1>" + esc(base) + "</h1>\n<div class=\"muted\">" +
         esc(TF("Created by TuxDirStat %1 on %2", {TUXDIRSTAT_VERSION, date})) + " · " +
         esc(ap ? T("apparent size") : T("size on disk")) + "</div>\n";
    h += "<div class=\"stats\">";
    auto stat = [&](const std::string &v, const char *label) { h += "<div class=\"stat\"><b>" + esc(v) + "</b>" + esc(T(label)) + "</div>"; };
    stat(fmtSize(root->sizeOf(ap)), N_("Total size"));
    stat(fmtInt(root->files), N_("Files"));
    stat(fmtInt(root->dirs), N_("Folders"));
    uint64_t dt = 0, da = 0;
    if (base.rfind('/', 0) == 0 && diskUsage(base, dt, da))
        stat(fmtSize(da), N_("Free on this disk"));
    h += "</div>\n";
    h += "<h2>" + esc(T("Map")) + "</h2>\n<div class=\"maps\">";
    h += "<img alt=\"treemap\" width=\"680\" height=\"500\" src=\"" + pngData(root, 680, 500, ap, MapShape::Rectangles) + "\">";
    h += "<img alt=\"sunburst\" width=\"500\" height=\"500\" src=\"" + pngData(root, 500, 500, ap, MapShape::Rings) + "\">";
    h += "</div>\n";

    auto sizeTable = [&](const char *title, const std::vector<Node *> &rows, bool folders) {
        h += "<h2>" + esc(T(title)) + "</h2>\n<table><tr><th>" + esc(T(folders ? "Folder" : "File")) + "</th><th>" + esc(T("Size")) +
             "</th><th>%</th><th style=\"width:28%\"></th>";
        if (folders)
            h += "<th>" + esc(T("Files")) + "</th>";
        h += "</tr>\n";
        for (const Node *n : rows) {
            const double frac = double(n->sizeOf(ap)) / total;
            h += "<tr><td class=\"p\">" + esc(rel(n)) + (n->isDir ? "/" : "") + "</td><td class=\"n\">" + esc(fmtSize(n->sizeOf(ap))) +
                 "</td><td class=\"n\">" + esc(fmtPct(frac)) + "</td><td><div class=\"bar\" style=\"width:" +
                 std::to_string(std::max(0.2, frac * 100)) + "%\"></div></td>";
            if (folders)
                h += "<td class=\"n\">" + esc(n->isDir ? fmtInt(n->files) : "1") + "</td>";
            h += "</tr>\n";
        }
        h += "</table>\n";
    };
    sizeTable(N_("Contents"), top, true);
    sizeTable(N_("Largest files"), files, false);

    h += "<h2>" + esc(T("File types")) + "</h2>\n<table><tr><th>" + esc(T("Extension")) + "</th><th>" + esc(T("Type")) + "</th><th>" +
         esc(T("Size")) + "</th><th>%</th><th>" + esc(T("Files")) + "</th></tr>\n";
    for (const ExtRow &r : types) {
        const std::string label = r.ext ? "." + ExtTable::name(r.ext) : std::string(T("(none)"));
        h += "<tr><td><span class=\"sw\" style=\"background:" + hexColor(r.color) + "\"></span>" + esc(label) + "</td><td>" +
             esc(describeExt(r.ext)) + "</td><td class=\"n\">" + esc(fmtSize(r.bytes)) + "</td><td class=\"n\">" +
             esc(fmtPct(double(r.bytes) / total)) + "</td><td class=\"n\">" + esc(fmtInt(r.count)) + "</td></tr>\n";
    }
    h += "</table>\n<footer class=\"muted\">" + esc(T("Made with TuxDirStat")) +
         " · <a href=\"https://github.com/Gubir34/TuxDirStat\">github.com/Gubir34/TuxDirStat</a></footer>\n</body>\n</html>\n";
    std::fwrite(h.data(), 1, h.size(), f);
    return closeOutput(f, err);
}
