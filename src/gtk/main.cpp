// TuxDirStat — GTK arayüzü. Aynı kaynak hem GTK 3 hem GTK 4 için derlenir.
// Kullanım: tuxdirstat-gtk3 [klasör] / tuxdirstat-gtk4 [klasör]

// GtkTreeView, GTK 4.10'dan beri "eski" sayılıyor ama GTK 4 boyunca destekleniyor;
// iki sürümü tek kodla sürmek için onu kullanıyoruz.
#define GDK_DISABLE_DEPRECATION_WARNINGS
#include <gtk/gtk.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "extinfo.h"
#include "report.h"
#include "session.h"
#include "treemap_render.h"

#define DT_GTK4 GTK_CHECK_VERSION(4, 0, 0)
// Lambda içindeki virgüller makro argümanlarını bölmesin diye ek parantez.
#define DT_CB(...) G_CALLBACK((__VA_ARGS__))

namespace {

enum Col { C_NAME, C_BAR, C_PCT, C_SIZE, C_ITEMS, C_FILES, C_DIRS, C_MTIME, NCOLS };
const char *const COL_TITLES[NCOLS] = {"Ad", "Alt ağaç", "%", "Boyut", "Öğe", "Dosya", "Klasör", "Son değişiklik"};
const int COL_WIDTHS[NCOLS] = {340, 130, 70, 90, 80, 80, 70, 145};
const SortKey COL_KEYS[NCOLS] = {SortKey::Name, SortKey::Size, SortKey::Size, SortKey::Size,
                                 SortKey::Items, SortKey::Files, SortKey::Dirs, SortKey::MTime};

}  // namespace

// ───────────────────────────── Ağaç modeli ─────────────────────────────
// Node ağacını GtkTreeView'a sunar; çocuklar ilk istendiklerinde sıralanır.

struct DtModel {
    GObject parent;
    Node *root;
    uint32_t gen;
    int stamp;
    SortKey key;
    bool desc;
    bool apparent;
    std::unordered_set<Node *> *sorted;
};
struct DtModelClass {
    GObjectClass parent_class;
};

static void dt_model_iface_init(GtkTreeModelIface *iface);
G_DEFINE_TYPE_WITH_CODE(DtModel, dt_model, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GTK_TYPE_TREE_MODEL, dt_model_iface_init))

static void dt_model_init(DtModel *m)
{
    m->root = nullptr;
    m->gen = 1;
    m->stamp = g_random_int();
    m->key = SortKey::Size;
    m->desc = true;
    m->apparent = false;
    m->sorted = new std::unordered_set<Node *>;
}

static void dt_model_finalize(GObject *o)
{
    delete reinterpret_cast<DtModel *>(o)->sorted;
    G_OBJECT_CLASS(dt_model_parent_class)->finalize(o);
}

static void dt_model_class_init(DtModelClass *k)
{
    G_OBJECT_CLASS(k)->finalize = dt_model_finalize;
}

static const Kids &dtKids(DtModel *m, Node *n)
{
    if (n->sgen != m->gen) {
        sortKids(n->children, m->key, m->desc, m->apparent);
        n->sgen = m->gen;
        m->sorted->insert(n);
    }
    return n->children;
}

static bool dtSet(DtModel *m, GtkTreeIter *it, Node *n)
{
    it->stamp = m->stamp;
    it->user_data = n;
    it->user_data2 = it->user_data3 = nullptr;
    return n != nullptr;
}

static Node *dtNode(const GtkTreeIter *it)
{
    return static_cast<Node *>(it->user_data);
}

static GtkTreeModelFlags dt_get_flags(GtkTreeModel *)
{
    return GTK_TREE_MODEL_ITERS_PERSIST;
}

static gint dt_get_n_columns(GtkTreeModel *)
{
    return 1;
}

static GType dt_get_column_type(GtkTreeModel *, gint)
{
    return G_TYPE_POINTER;
}

static gboolean dt_get_iter(GtkTreeModel *tm, GtkTreeIter *it, GtkTreePath *path)
{
    auto *m = reinterpret_cast<DtModel *>(tm);
    int depth = 0;
    int *idx = gtk_tree_path_get_indices_with_depth(path, &depth);
    if (!m->root || depth < 1 || idx[0] != 0)
        return FALSE;
    Node *n = m->root;
    for (int d = 1; d < depth; ++d) {
        const Kids &k = dtKids(m, n);
        if (idx[d] < 0 || idx[d] >= int(k.size()))
            return FALSE;
        n = k[idx[d]];
    }
    return dtSet(m, it, n);
}

static GtkTreePath *dt_get_path(GtkTreeModel *tm, GtkTreeIter *it)
{
    auto *m = reinterpret_cast<DtModel *>(tm);
    std::vector<int> rows;
    for (Node *n = dtNode(it); n && n != m->root; n = n->parent)
        rows.push_back(n->row);
    rows.push_back(0);
    GtkTreePath *p = gtk_tree_path_new();
    for (auto r = rows.rbegin(); r != rows.rend(); ++r)
        gtk_tree_path_append_index(p, *r);
    return p;
}

static void dt_get_value(GtkTreeModel *, GtkTreeIter *it, gint, GValue *v)
{
    g_value_init(v, G_TYPE_POINTER);
    g_value_set_pointer(v, it->user_data);
}

static gboolean dt_iter_next(GtkTreeModel *tm, GtkTreeIter *it)
{
    auto *m = reinterpret_cast<DtModel *>(tm);
    Node *n = dtNode(it);
    if (n == m->root || !n->parent)
        return FALSE;
    const Kids &k = dtKids(m, n->parent);
    return size_t(n->row + 1) < k.size() ? dtSet(m, it, k[n->row + 1]) : FALSE;
}

static gboolean dt_iter_previous(GtkTreeModel *tm, GtkTreeIter *it)
{
    auto *m = reinterpret_cast<DtModel *>(tm);
    Node *n = dtNode(it);
    if (n == m->root || !n->parent || n->row == 0)
        return FALSE;
    return dtSet(m, it, dtKids(m, n->parent)[n->row - 1]);
}

static gboolean dt_iter_nth_child(GtkTreeModel *tm, GtkTreeIter *it, GtkTreeIter *parent, gint n)
{
    auto *m = reinterpret_cast<DtModel *>(tm);
    if (!parent)
        return n == 0 && m->root ? dtSet(m, it, m->root) : FALSE;
    if (!m->root || !dtNode(parent))
        return FALSE;
    const Kids &k = dtKids(m, dtNode(parent));
    return n >= 0 && n < int(k.size()) ? dtSet(m, it, k[n]) : FALSE;
}

static gboolean dt_iter_children(GtkTreeModel *tm, GtkTreeIter *it, GtkTreeIter *parent)
{
    return dt_iter_nth_child(tm, it, parent, 0);
}

static gboolean dt_iter_has_child(GtkTreeModel *, GtkTreeIter *it)
{
    return !dtNode(it)->children.empty();
}

static gint dt_iter_n_children(GtkTreeModel *tm, GtkTreeIter *it)
{
    auto *m = reinterpret_cast<DtModel *>(tm);
    if (!it)
        return m->root ? 1 : 0;
    return int(dtNode(it)->children.size());
}

static gboolean dt_iter_parent(GtkTreeModel *tm, GtkTreeIter *it, GtkTreeIter *child)
{
    auto *m = reinterpret_cast<DtModel *>(tm);
    Node *n = dtNode(child);
    if (n == m->root || !n->parent)
        return FALSE;
    return dtSet(m, it, n->parent);
}

static void dt_model_iface_init(GtkTreeModelIface *iface)
{
    iface->get_flags = dt_get_flags;
    iface->get_n_columns = dt_get_n_columns;
    iface->get_column_type = dt_get_column_type;
    iface->get_iter = dt_get_iter;
    iface->get_path = dt_get_path;
    iface->get_value = dt_get_value;
    iface->iter_next = dt_iter_next;
    iface->iter_previous = dt_iter_previous;
    iface->iter_children = dt_iter_children;
    iface->iter_has_child = dt_iter_has_child;
    iface->iter_n_children = dt_iter_n_children;
    iface->iter_nth_child = dt_iter_nth_child;
    iface->iter_parent = dt_iter_parent;
}

// Düğümün yolunu üretir; yol boyunca çocukların sıralı olmasını sağlar.
static GtkTreePath *dtPathFor(DtModel *m, Node *node)
{
    std::vector<Node *> chain;
    for (Node *n = node; n && n != m->root; n = n->parent)
        chain.push_back(n);
    GtkTreePath *p = gtk_tree_path_new_first();
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        dtKids(m, (*it)->parent);
        gtk_tree_path_append_index(p, (*it)->row);
    }
    return p;
}

// ───────────────────────────── GTK 3/4 yardımcıları ─────────────────────────────

namespace {

void boxAdd(GtkWidget *box, GtkWidget *child, bool expand = false)
{
#if DT_GTK4
    if (expand) {
        if (gtk_orientable_get_orientation(GTK_ORIENTABLE(box)) == GTK_ORIENTATION_HORIZONTAL)
            gtk_widget_set_hexpand(child, TRUE);
        else
            gtk_widget_set_vexpand(child, TRUE);
    }
    gtk_box_append(GTK_BOX(box), child);
#else
    gtk_box_pack_start(GTK_BOX(box), child, expand, expand, 0);
#endif
}

GtkWidget *scrolled(GtkWidget *child)
{
#if DT_GTK4
    GtkWidget *s = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(s), child);
#else
    GtkWidget *s = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_container_add(GTK_CONTAINER(s), child);
#endif
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    return s;
}

GtkWidget *paned(GtkOrientation o, GtkWidget *a, GtkWidget *b, int pos)
{
    GtkWidget *p = gtk_paned_new(o);
#if DT_GTK4
    gtk_paned_set_start_child(GTK_PANED(p), a);
    gtk_paned_set_end_child(GTK_PANED(p), b);
    gtk_paned_set_shrink_start_child(GTK_PANED(p), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(p), FALSE);
#else
    gtk_paned_pack1(GTK_PANED(p), a, TRUE, FALSE);
    gtk_paned_pack2(GTK_PANED(p), b, TRUE, FALSE);
#endif
    gtk_paned_set_position(GTK_PANED(p), pos);
    return p;
}

void windowSetChild(GtkWidget *win, GtkWidget *child)
{
#if DT_GTK4
    gtk_window_set_child(GTK_WINDOW(win), child);
#else
    gtk_container_add(GTK_CONTAINER(win), child);
#endif
}

void destroyWindow(GtkWidget *w)
{
#if DT_GTK4
    gtk_window_destroy(GTK_WINDOW(w));
#else
    gtk_widget_destroy(w);
#endif
}

GtkWidget *toolButton(const char *icon, const char *label, const char *action, bool toggle, const char *tip = nullptr)
{
    GtkWidget *b = toggle ? gtk_toggle_button_new() : gtk_button_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
#if DT_GTK4
    boxAdd(box, gtk_image_new_from_icon_name(icon));
    boxAdd(box, gtk_label_new(label));
    gtk_button_set_child(GTK_BUTTON(b), box);
    gtk_button_set_has_frame(GTK_BUTTON(b), toggle);
#else
    boxAdd(box, gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_SMALL_TOOLBAR));
    boxAdd(box, gtk_label_new(label));
    gtk_container_add(GTK_CONTAINER(b), box);
    gtk_button_set_relief(GTK_BUTTON(b), toggle ? GTK_RELIEF_NORMAL : GTK_RELIEF_NONE);
#endif
    gtk_actionable_set_action_name(GTK_ACTIONABLE(b), action);
    if (tip)
        gtk_widget_set_tooltip_text(b, tip);
    return b;
}

void setClipboard(GtkWidget *w, const std::string &text)
{
#if DT_GTK4
    gdk_clipboard_set_text(gtk_widget_get_clipboard(w), text.c_str());
#else
    (void)w;
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), text.c_str(), -1);
#endif
}

std::string markupEscape(const std::string &s)
{
    gchar *e = g_markup_escape_text(s.c_str(), -1);
    std::string r = e;
    g_free(e);
    return r;
}

// Evet/hayır sorusu; cevap geldiğinde cb(true/false) çağrılır.
void confirm(GtkWindow *parent, const std::string &title, const std::string &detail, const char *yes,
             std::function<void(bool)> cb)
{
    auto *fn = new std::function<void(bool)>(std::move(cb));
#if GTK_CHECK_VERSION(4, 10, 0)
    GtkAlertDialog *d = gtk_alert_dialog_new("%s", title.c_str());
    gtk_alert_dialog_set_detail(d, detail.c_str());
    const char *buttons[] = {"İptal", yes, nullptr};
    gtk_alert_dialog_set_buttons(d, buttons);
    gtk_alert_dialog_set_cancel_button(d, 0);
    gtk_alert_dialog_set_default_button(d, 0);
    gtk_alert_dialog_choose(
        d, parent, nullptr,
        [](GObject *src, GAsyncResult *res, gpointer data) {
            auto *f = static_cast<std::function<void(bool)> *>(data);
            int r = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, nullptr);
            (*f)(r == 1);
            delete f;
        },
        fn);
    g_object_unref(d);
#else
    GtkWidget *d = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s",
                                          title.c_str());
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", detail.c_str());
    gtk_dialog_add_buttons(GTK_DIALOG(d), "İptal", GTK_RESPONSE_CANCEL, yes, GTK_RESPONSE_ACCEPT, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL);
    g_signal_connect(d, "response", DT_CB(+[](GtkDialog *dlg, int resp, gpointer data) {
                         auto *f = static_cast<std::function<void(bool)> *>(data);
                         destroyWindow(GTK_WIDGET(dlg));
                         (*f)(resp == GTK_RESPONSE_ACCEPT);
                         delete f;
                     }),
                     fn);
    gtk_widget_show_all(d);
#endif
}

void showError(GtkWindow *parent, const std::string &title, const std::string &detail)
{
#if GTK_CHECK_VERSION(4, 10, 0)
    GtkAlertDialog *d = gtk_alert_dialog_new("%s", title.c_str());
    gtk_alert_dialog_set_detail(d, detail.c_str());
    gtk_alert_dialog_show(d, parent);
    g_object_unref(d);
#else
    GtkWidget *d = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "%s",
                                          title.c_str());
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", detail.c_str());
    g_signal_connect(d, "response", G_CALLBACK(destroyWindow), nullptr);
    gtk_widget_show_all(d);
#endif
}

void pickFolder(GtkWindow *parent, std::function<void(const std::string &)> cb)
{
    auto *fn = new std::function<void(const std::string &)>(std::move(cb));
#if GTK_CHECK_VERSION(4, 10, 0)
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Klasör seç");
    GFile *home = g_file_new_for_path(homeDir().c_str());
    gtk_file_dialog_set_initial_folder(d, home);
    g_object_unref(home);
    gtk_file_dialog_select_folder(
        d, parent, nullptr,
        [](GObject *src, GAsyncResult *res, gpointer data) {
            auto *f = static_cast<std::function<void(const std::string &)> *>(data);
            if (GFile *file = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, nullptr)) {
                if (char *p = g_file_get_path(file)) {
                    (*f)(p);
                    g_free(p);
                }
                g_object_unref(file);
            }
            delete f;
        },
        fn);
    g_object_unref(d);
#else
    GtkFileChooserNative *d = gtk_file_chooser_native_new("Klasör seç", parent, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                                          "Seç", "İptal");
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(d), homeDir().c_str());
    g_signal_connect(d, "response", DT_CB(+[](GtkNativeDialog *dlg, int resp, gpointer data) {
                         auto *f = static_cast<std::function<void(const std::string &)> *>(data);
                         if (resp == GTK_RESPONSE_ACCEPT) {
                             if (char *p = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg))) {
                                 (*f)(p);
                                 g_free(p);
                             }
                         }
                         delete f;
                         g_object_unref(dlg);
                     }),
                     fn);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(d));
#endif
}

// Yuvarlak uçlu oran çubuğu: yarı saydam zemin her iki temada da okunur; derinlik
// arttıkça dolgu biraz solar. Görüntüler önbelleğe alınır.
GdkPixbuf *barPixbuf(int pct, int depth)
{
    static std::unordered_map<int, GdkPixbuf *> cache;
    pct = std::clamp(pct, 0, 100);
    depth = std::min(depth, 5);
    const int key = pct * 8 + depth;
    if (auto it = cache.find(key); it != cache.end())
        return it->second;
    const int W = 112, H = 10;
    const double r = H / 2.0;
    GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, W, H);
    guchar *px = gdk_pixbuf_get_pixels(pb);
    const int stride = gdk_pixbuf_get_rowstride(pb);
    const double fade = 0.13 * depth;
    const double ar = 61 + (120 - 61) * fade, ag = 126 + (150 - 126) * fade, ab = 255 + (205 - 255) * fade;
    const double fillW = pct ? std::max(double(H), W * pct / 100.0) : 0;
    auto coverage = [r](double x, double y, double len) {
        if (len <= 0)
            return 0.0;
        const double cx = std::clamp(x, r, std::max(r, len - r));
        return std::clamp(r - std::hypot(x - cx, y - r) + 0.5, 0.0, 1.0);
    };
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const double fx = x + 0.5, fy = y + 0.5;
            const double ta = 0.22 * coverage(fx, fy, W), fa = coverage(fx, fy, fillW);
            const double a = fa + ta * (1 - fa);
            guchar *p = px + y * stride + x * 4;
            if (a <= 0) {
                p[0] = p[1] = p[2] = p[3] = 0;
                continue;
            }
            p[0] = guchar((ar * fa + 128 * ta * (1 - fa)) / a);
            p[1] = guchar((ag * fa + 128 * ta * (1 - fa)) / a);
            p[2] = guchar((ab * fa + 128 * ta * (1 - fa)) / a);
            p[3] = guchar(a * 255);
        }
    }
    cache.emplace(key, pb);
    return pb;
}

// KDE, GTK'ye renk şemasını kullanıcı CSS'i (~/.config/gtk-N.0/colors.css) ile verir ve bu
// açık/koyu seçiminin önüne geçer. Açık ya da koyu seçildiğinde aynı renk adlarını daha
// yüksek öncelikli bir sağlayıcıyla kendi paletimize göre yeniden tanımlarız.
std::string colorOverrideCss(bool dark)
{
    const char *cfg = std::getenv("XDG_CONFIG_HOME");
    const std::string file = (cfg && *cfg ? std::string(cfg) : homeDir() + "/.config") +
                             (DT_GTK4 ? "/gtk-4.0/colors.css" : "/gtk-3.0/colors.css");
    gchar *text = nullptr;
    if (!g_file_get_contents(file.c_str(), &text, nullptr, nullptr))
        return {};
    struct P {
        const char *bg, *fg, *base, *text, *sel, *selfg, *border, *insens, *header, *button, *tooltip, *link;
    };
    const P c = dark ? P{"#1f2227", "#e3e5e8", "#181a1e", "#e3e5e8", "#3d7eff", "#ffffff", "#3a3f47", "#6b717a",
                         "#24282e", "#2a2e35", "#2a2e35", "#7aa7ff"}
                     : P{"#f2f3f5", "#1f2328", "#ffffff", "#1f2328", "#2f6fed", "#ffffff", "#c4c8cf", "#9da3ab",
                         "#e8eaee", "#e8eaee", "#ffffff", "#2f6fed"};
    auto pick = [&](const std::string &n) -> const char * {
        auto has = [&n](const char *k) { return n.find(k) != std::string::npos; };
        if (has("error")) return "#da4453";
        if (has("warning")) return "#f67400";
        if (has("success")) return "#27ae60";
        if (has("link_visited")) return "#9b59b6";
        if (has("link")) return c.link;
        if (has("tooltip_background")) return c.tooltip;
        if (has("tooltip_border") || has("borders")) return c.border;
        if (has("tooltip_text")) return c.fg;
        if (has("selected_fg")) return c.selfg;
        if (has("selected_bg") || has("decoration")) return c.sel;
        if (has("button_background")) return c.button;
        if (has("header_background") || has("titlebar_background")) return c.header;
        if (has("insensitive") && (has("fg") || has("foreground"))) return c.insens;
        if (has("foreground")) return c.fg;
        if (has("base_color") || has("view_bg") || has("content_view_bg")) return c.base;
        if (has("text_color") || has("view_text")) return c.text;
        if (has("fg_color")) return c.fg;
        if (has("bg_color")) return c.bg;
        return nullptr;
    };
    std::string css;
    const std::string src = text;
    g_free(text);
    for (size_t pos = 0; (pos = src.find("@define-color ", pos)) != std::string::npos;) {
        pos += 14;
        const size_t end = src.find_first_of(" \t", pos);
        if (end == std::string::npos)
            break;
        const std::string name = src.substr(pos, end - pos);
        if (const char *v = pick(name))
            css += "@define-color " + name + " " + v + ";\n";
    }
    return css;
}

void applyColorOverride(const std::string &theme)
{
    static GtkCssProvider *provider = nullptr;
    if (provider) {
#if DT_GTK4
        gtk_style_context_remove_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider));
#else
        gtk_style_context_remove_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider));
#endif
        g_clear_object(&provider);
    }
    if (theme != "light" && theme != "dark")
        return;
    const std::string css = colorOverrideCss(theme == "dark");
    if (css.empty())
        return;
    provider = gtk_css_provider_new();
#if DT_GTK4
    gtk_css_provider_load_from_string(provider, css.c_str());
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_USER + 1);
#else
    gtk_css_provider_load_from_data(provider, css.c_str(), -1, nullptr);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_USER + 1);
#endif
}

// Tema: "system" masaüstünün GTK temasını kullanır; "light"/"dark" aynı temanın
// açık ya da koyu biçimini zorlar.
void applyGtkTheme(const std::string &theme)
{
    applyColorOverride(theme);
    GtkSettings *gs = gtk_settings_get_default();
    static std::string origName, origIcons;
    static gboolean origDark = FALSE;
    static bool saved = false;
    if (!saved) {
        gchar *n = nullptr, *ic = nullptr;
        g_object_get(gs, "gtk-theme-name", &n, "gtk-icon-theme-name", &ic, "gtk-application-prefer-dark-theme",
                     &origDark, nullptr);
        origName = n ? n : "";
        origIcons = ic ? ic : "";
        g_free(n);
        g_free(ic);
        saved = true;
    }
    // Simge teması da açık/koyu biçime geçer (ör. breeze ↔ breeze-dark); yoksa olduğu gibi kalır.
    auto iconsFor = [](std::string name, bool dark) {
        auto exists = [](const std::string &n) {
            for (const std::string &dir : {homeDir() + "/.local/share/icons/", std::string("/usr/share/icons/")})
                if (g_file_test((dir + n).c_str(), G_FILE_TEST_IS_DIR))
                    return true;
            return false;
        };
        const bool isDark = name.size() > 5 && (name.compare(name.size() - 5, 5, "-dark") == 0 ||
                                                name.compare(name.size() - 5, 5, "-Dark") == 0);
        if (!dark && isDark && exists(name.substr(0, name.size() - 5)))
            return name.substr(0, name.size() - 5);
        if (dark && !isDark && exists(name + "-dark"))
            return name + "-dark";
        return name;
    };
#if GTK_CHECK_VERSION(4, 20, 0)
    // GTK 4.20+ koyu/açık seçimini renk şemasıyla yapar; eski "prefer-dark" yok sayılır.
    g_object_set(gs, "gtk-interface-color-scheme",
                 theme == "dark"    ? GTK_INTERFACE_COLOR_SCHEME_DARK
                 : theme == "light" ? GTK_INTERFACE_COLOR_SCHEME_LIGHT
                                    : GTK_INTERFACE_COLOR_SCHEME_DEFAULT,
                 nullptr);
#endif
    if (theme != "light" && theme != "dark") {
        g_object_set(gs, "gtk-theme-name", origName.c_str(), "gtk-icon-theme-name", origIcons.c_str(),
                     "gtk-application-prefer-dark-theme", origDark, nullptr);
        return;
    }
    g_object_set(gs, "gtk-icon-theme-name", iconsFor(origIcons, theme == "dark").c_str(), nullptr);
    std::string base = origName;
    for (const char *suffix : {"-Dark", "-dark", "-Darker", "-darker"}) {
        const size_t n = std::strlen(suffix);
        if (base.size() > n && base.compare(base.size() - n, n, suffix) == 0) {
            base.resize(base.size() - n);
            break;
        }
    }
    if (base.empty())
        base = "Adwaita";
    g_object_set(gs, "gtk-theme-name", base.c_str(), "gtk-application-prefer-dark-theme", theme == "dark", nullptr);
}

GdkPixbuf *swatch(const RGBf &c)
{
    GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 14, 14);
    auto ch = [](float v) { return guint32(std::min(255.f, v * 1.4f)); };
    gdk_pixbuf_fill(pb, ch(c.r) << 24 | ch(c.g) << 16 | ch(c.b) << 8 | 0xFF);
    return pb;
}

}  // namespace

// ───────────────────────────── Uygulama ─────────────────────────────

class App {
public:
    App(GtkApplication *gapp, const std::string &path);

    GtkApplication *gapp_;
    GtkWidget *win_;

private:
    // kurulum
    void buildActions();
    GtkWidget *buildToolbar();
    GtkWidget *buildTree();
    GtkWidget *buildExtList();
    GtkWidget *buildTreemap();
    GtkWidget *buildStatus();

    // tarama
    void chooseTarget();
    void startScan(const std::string &path, Node *target = nullptr);
    bool poll();
    void setModelRoot(Node *root);
    void resortView();
    void afterChange();

    // görünümler
    void fillExt();
    std::string extDescription(uint32_t ext);
    GIcon *iconFor(const Node *n);
    void updateTotals();
    void updateZoomLabel();
    void setStatus(Node *n);
    void selectNode(Node *n);
    Node *selectedNode();
    void zoomTo(Node *n);
    void setSortColumn(int col);

    // treemap
    void scheduleRender(int ms);
    void renderTreemap();
    void composeTreemap();
    Node *treemapHit(double x, double y);
    void treemapPress(int button, int nPress, double x, double y, void *event);

    // eylemler
    void showMenu(Node *n, GtkWidget *widget, double x, double y, void *event);
    void deleteNode(Node *n, bool permanent);
    void finishDelete(Node *n);
    void refreshNode(Node *n);
    void setActionEnabled(const char *name, bool on);

    static void cellData(GtkTreeViewColumn *col, GtkCellRenderer *cell, GtkTreeModel *, GtkTreeIter *it, gpointer self);
    static void nameIconData(GtkTreeViewColumn *, GtkCellRenderer *cell, GtkTreeModel *, GtkTreeIter *it, gpointer self);

    Session s_;
    DtModel *model_ = nullptr;
    GtkWidget *tree_ = nullptr;
    GtkTreeViewColumn *cols_[NCOLS] = {};
    int sortCol_ = C_SIZE;
    bool sortDesc_ = true;

    GtkListStore *extStore_ = nullptr;
    GtkWidget *extView_ = nullptr;

    GtkWidget *area_ = nullptr, *tmBox_ = nullptr, *zoomLabel_ = nullptr;
    GtkWidget *statusL_ = nullptr, *statusR_ = nullptr, *spinner_ = nullptr;
    TreemapRenderer tm_;
    std::vector<uint32_t> tmPixels_;
    cairo_surface_t *surface_ = nullptr;
    Node *tmRoot_ = nullptr, *selected_ = nullptr, *hover_ = nullptr, *menuNode_ = nullptr;
    int highlight_ = -1;
    guint pollId_ = 0, renderId_ = 0;

    std::unordered_map<uint32_t, std::string> extDesc_;
    std::unordered_map<uint32_t, GIcon *> icons_;
    GIcon *dirIcon_, *fileIcon_, *linkIcon_, *groupIcon_;
    bool applyingSettings_ = false;
    bool showTreemapSetting_ = true;
    std::string theme_ = "system";
    void regroup(bool on, uint32_t minCount);
    void applySettings();
    void saveSettings();

    friend void appDestroyed(GtkWidget *, gpointer);
    friend struct ActionTable;
};

void appDestroyed(GtkWidget *, gpointer self)
{
    App *a = static_cast<App *>(self);
    // Görünümler yok edilirken modele ve bize geri çağrı yapmasın: önce ayır. GTK 3'te
    // pencerenin "destroy" sinyali çocuklardan önce, GTK 4'te sonra gelir; zayıf
    // işaretçiler yok edilmiş bileşenleri sıfırlar.
    for (GtkWidget *view : {a->tree_, a->extView_}) {
        if (!view)
            continue;
        g_signal_handlers_disconnect_by_data(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), a);
        gtk_tree_view_set_model(GTK_TREE_VIEW(view), nullptr);
    }
    a->model_->root = nullptr;
    if (a->area_) {
#if DT_GTK4
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(a->area_), nullptr, nullptr, nullptr);
#endif
        g_signal_handlers_disconnect_by_data(a->area_, a);
    }
    for (GtkWidget **w : {&a->tree_, &a->extView_, &a->area_})
        if (*w)
            g_object_remove_weak_pointer(G_OBJECT(*w), reinterpret_cast<gpointer *>(w));
    a->s_.cancelScan();
    if (a->pollId_)
        g_source_remove(a->pollId_);
    if (a->renderId_)
        g_source_remove(a->renderId_);
    if (a->surface_)
        cairo_surface_destroy(a->surface_);
    for (auto &[k, ic] : a->icons_)
        g_object_unref(ic);
    g_object_unref(a->dirIcon_);
    g_object_unref(a->fileIcon_);
    g_object_unref(a->linkIcon_);
    g_object_unref(a->groupIcon_);
    g_object_unref(a->model_);
    g_object_unref(a->extStore_);
    delete a;
}

// ───────────── eylem tablosu ─────────────

struct ActionTable {
    static App *app(gpointer p) { return static_cast<App *>(p); }

    static void open(GSimpleAction *, GVariant *, gpointer p) { app(p)->chooseTarget(); }
    static void rescan(GSimpleAction *, GVariant *, gpointer p)
    {
        App *a = app(p);
        if (a->s_.root())
            a->startScan(a->s_.root()->path());
        else
            a->chooseTarget();
    }
    static void stop(GSimpleAction *, GVariant *, gpointer p) { app(p)->s_.cancelScan(); }
    static void zoomIn(GSimpleAction *, GVariant *, gpointer p)
    {
        if (Node *n = app(p)->selectedNode())
            app(p)->zoomTo(n->isDir ? n : n->parent);
    }
    static void zoomOut(GSimpleAction *, GVariant *, gpointer p)
    {
        App *a = app(p);
        if (a->tmRoot_ && a->tmRoot_->parent)
            a->zoomTo(a->tmRoot_->parent);
    }
    static void zoomRoot(GSimpleAction *, GVariant *, gpointer p) { app(p)->zoomTo(app(p)->s_.root()); }
    static void copy(GSimpleAction *, GVariant *, gpointer p)
    {
        if (Node *n = app(p)->selectedNode())
            setClipboard(app(p)->win_, n->path());
    }
    static void trash(GSimpleAction *, GVariant *, gpointer p)
    {
        if (Node *n = app(p)->selectedNode())
            app(p)->deleteNode(n, false);
    }
    static void del(GSimpleAction *, GVariant *, gpointer p)
    {
        if (Node *n = app(p)->selectedNode())
            app(p)->deleteNode(n, true);
    }
    static void quit(GSimpleAction *, GVariant *, gpointer p) { destroyWindow(app(p)->win_); }
    static void about(GSimpleAction *, GVariant *, gpointer p)
    {
        showError(GTK_WINDOW(app(p)->win_), "TuxDirStat",
                  "Linux için disk kullanım analizörü.\n\nAğaçta veya treemap'te bir öğeye tıklayın; treemap'te "
                  "çift tıklayarak klasöre yakınlaşın, Backspace ile geri çıkın. Uzantı listesinde bir satır "
                  "seçmek o türdeki dosyaları treemap'te işaretler.");
    }

    static void apparent(GSimpleAction *a, GVariant *v, gpointer p)
    {
        g_simple_action_set_state(a, v);
        App *self = app(p);
        self->s_.apparent = g_variant_get_boolean(v);
        self->model_->apparent = self->s_.apparent;
        self->resortView();
        self->s_.recomputeExt();
        self->afterChange();
        self->saveSettings();
    }
    static void oneFs(GSimpleAction *a, GVariant *v, gpointer p)
    {
        g_simple_action_set_state(a, v);
        app(p)->s_.oneFs = g_variant_get_boolean(v);
        app(p)->saveSettings();
    }
    static void group(GSimpleAction *a, GVariant *v, gpointer p)
    {
        g_simple_action_set_state(a, v);
        app(p)->regroup(g_variant_get_boolean(v), app(p)->s_.groupMin);
    }
    static void groupMin(GSimpleAction *a, GVariant *v, gpointer p)
    {
        g_simple_action_set_state(a, v);
        app(p)->regroup(app(p)->s_.grouping, g_variant_get_uint32(v));
    }
    static void theme(GSimpleAction *a, GVariant *v, gpointer p)
    {
        g_simple_action_set_state(a, v);
        app(p)->theme_ = g_variant_get_string(v, nullptr);
        applyGtkTheme(app(p)->theme_);
        app(p)->saveSettings();
    }
    static void showTreemap(GSimpleAction *a, GVariant *v, gpointer p)
    {
        g_simple_action_set_state(a, v);
        gtk_widget_set_visible(app(p)->tmBox_, g_variant_get_boolean(v));
        app(p)->saveSettings();
    }

    // Bağlam menüsü: menuNode_ üzerinde çalışır
    static Node *mn(gpointer p) { return app(p)->menuNode_; }
    static void nOpen(GSimpleAction *, GVariant *, gpointer p)
    {
        if (Node *n = mn(p))
            openPath(n->path());
    }
    static void nReveal(GSimpleAction *, GVariant *, gpointer p)
    {
        if (Node *n = mn(p))
            revealPath(n->path());
    }
    static void nCopy(GSimpleAction *, GVariant *, gpointer p)
    {
        if (Node *n = mn(p))
            setClipboard(app(p)->win_, n->path());
    }
    static void nZoom(GSimpleAction *, GVariant *, gpointer p) { app(p)->zoomTo(mn(p)); }
    static void nRefresh(GSimpleAction *, GVariant *, gpointer p)
    {
        if (Node *n = mn(p))
            app(p)->refreshNode(n);
    }
    static void nTrash(GSimpleAction *, GVariant *, gpointer p)
    {
        if (Node *n = mn(p))
            app(p)->deleteNode(n, false);
    }
    static void nDelete(GSimpleAction *, GVariant *, gpointer p)
    {
        if (Node *n = mn(p))
            app(p)->deleteNode(n, true);
    }
};

void App::buildActions()
{
    static const GActionEntry entries[] = {
        {"open", ActionTable::open, nullptr, nullptr, nullptr},
        {"rescan", ActionTable::rescan, nullptr, nullptr, nullptr},
        {"stop", ActionTable::stop, nullptr, nullptr, nullptr},
        {"zoom-in", ActionTable::zoomIn, nullptr, nullptr, nullptr},
        {"zoom-out", ActionTable::zoomOut, nullptr, nullptr, nullptr},
        {"zoom-root", ActionTable::zoomRoot, nullptr, nullptr, nullptr},
        {"copy", ActionTable::copy, nullptr, nullptr, nullptr},
        {"trash", ActionTable::trash, nullptr, nullptr, nullptr},
        {"delete", ActionTable::del, nullptr, nullptr, nullptr},
        {"quit", ActionTable::quit, nullptr, nullptr, nullptr},
        {"about", ActionTable::about, nullptr, nullptr, nullptr},
        {"apparent", nullptr, nullptr, "false", ActionTable::apparent},
        {"onefs", nullptr, nullptr, "true", ActionTable::oneFs},
        {"group", nullptr, nullptr, "false", ActionTable::group},
        {"group-min", nullptr, "u", "uint32 16", ActionTable::groupMin},
        {"theme", nullptr, "s", "'system'", ActionTable::theme},
        {"treemap", nullptr, nullptr, "true", ActionTable::showTreemap},
        {"node-open", ActionTable::nOpen, nullptr, nullptr, nullptr},
        {"node-reveal", ActionTable::nReveal, nullptr, nullptr, nullptr},
        {"node-copy", ActionTable::nCopy, nullptr, nullptr, nullptr},
        {"node-zoom", ActionTable::nZoom, nullptr, nullptr, nullptr},
        {"node-refresh", ActionTable::nRefresh, nullptr, nullptr, nullptr},
        {"node-trash", ActionTable::nTrash, nullptr, nullptr, nullptr},
        {"node-delete", ActionTable::nDelete, nullptr, nullptr, nullptr},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(win_), entries, G_N_ELEMENTS(entries), this);
    struct {
        const char *action;
        const char *keys[3];
    } accels[] = {
        {"win.open", {"<Control>o", nullptr}},     {"win.rescan", {"F5", nullptr}},
        {"win.stop", {"Escape", nullptr}},         {"win.zoom-in", {"<Control>plus", "<Control>KP_Add", nullptr}},
        {"win.zoom-out", {"BackSpace", nullptr}},  {"win.zoom-root", {"<Control>0", nullptr}},
        {"win.copy", {"<Control>c", nullptr}},     {"win.trash", {"Delete", nullptr}},
        {"win.delete", {"<Shift>Delete", nullptr}}, {"win.quit", {"<Control>q", nullptr}},
        {"win.treemap", {"F9", nullptr}},          {"win.group", {"<Control>g", nullptr}},
    };
    for (auto &a : accels)
        gtk_application_set_accels_for_action(gapp_, a.action, a.keys);
    setActionEnabled("stop", false);
    setActionEnabled("zoom-out", false);
}

// Kayıtlı ayarları uygular; arayüz kurulduktan sonra çağrılır (kök yokken yalnızca durumları kurar).
void App::applySettings()
{
    applyingSettings_ = true;
    const Settings st = Settings::load();
    s_.groupMin = st.groupMin;
    auto set = [this](const char *name, GVariant *v) {
        g_action_change_state(g_action_map_lookup_action(G_ACTION_MAP(win_), name), v);
    };
    set("apparent", g_variant_new_boolean(st.apparent));
    set("onefs", g_variant_new_boolean(st.oneFs));
    set("group-min", g_variant_new_uint32(st.groupMin));
    set("group", g_variant_new_boolean(st.grouping));
    set("theme", g_variant_new_string(st.theme.c_str()));
    showTreemapSetting_ = st.showTreemap;
    applyingSettings_ = false;
}

void App::saveSettings()
{
    if (applyingSettings_)
        return;
    Settings st;
    st.apparent = s_.apparent;
    st.oneFs = s_.oneFs;
    st.grouping = s_.grouping;
    st.groupMin = s_.groupMin;
    st.theme = theme_;
    GVariant *v = g_action_get_state(g_action_map_lookup_action(G_ACTION_MAP(win_), "treemap"));
    st.showTreemap = g_variant_get_boolean(v);
    g_variant_unref(v);
    st.save();
}

void App::setActionEnabled(const char *name, bool on)
{
    if (GAction *a = g_action_map_lookup_action(G_ACTION_MAP(win_), name))
        g_simple_action_set_enabled(G_SIMPLE_ACTION(a), on);
}

// ───────────── kurulum ─────────────

App::App(GtkApplication *gapp, const std::string &path) : gapp_(gapp)
{
    dirIcon_ = g_themed_icon_new("folder");
    fileIcon_ = g_themed_icon_new("text-x-generic");
    linkIcon_ = g_themed_icon_new("emblem-symbolic-link");
    groupIcon_ = g_themed_icon_new("package-x-generic");

    win_ = gtk_application_window_new(gapp);
    gtk_window_set_title(GTK_WINDOW(win_), "TuxDirStat");
    gtk_window_set_default_size(GTK_WINDOW(win_), 1280, 860);
    gtk_window_set_icon_name(GTK_WINDOW(win_), "drive-harddisk");
    g_signal_connect(win_, "destroy", G_CALLBACK(appDestroyed), this);
    buildActions();

    GtkWidget *top = paned(GTK_ORIENTATION_HORIZONTAL, scrolled(buildTree()), scrolled(buildExtList()), 820);
    GtkWidget *main = paned(GTK_ORIENTATION_VERTICAL, top, buildTreemap(), 430);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    boxAdd(vbox, buildToolbar());
    boxAdd(vbox, main, true);
    boxAdd(vbox, buildStatus());
    windowSetChild(win_, vbox);
    for (GtkWidget **w : {&tree_, &extView_, &area_})
        g_object_add_weak_pointer(G_OBJECT(*w), reinterpret_cast<gpointer *>(w));
    applySettings();
    updateZoomLabel();

#if DT_GTK4
    gtk_window_present(GTK_WINDOW(win_));
#else
    gtk_widget_show_all(win_);
#endif
    if (!showTreemapSetting_)  // gösterildikten sonra gizlenmeli (GTK 3 show_all hepsini açar)
        g_action_change_state(g_action_map_lookup_action(G_ACTION_MAP(win_), "treemap"), g_variant_new_boolean(FALSE));
    if (!path.empty())
        startScan(path);
    else
        g_idle_add_once([](gpointer p) { static_cast<App *>(p)->chooseTarget(); }, this);
}

GtkWidget *App::buildToolbar()
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_margin_start(bar, 4);
    gtk_widget_set_margin_end(bar, 4);
    gtk_widget_set_margin_top(bar, 3);
    gtk_widget_set_margin_bottom(bar, 3);
    auto sep = [bar] { boxAdd(bar, gtk_separator_new(GTK_ORIENTATION_VERTICAL)); };
    boxAdd(bar, toolButton("document-open", "Aç…", "win.open", false));
    boxAdd(bar, toolButton("view-refresh", "Yeniden tara", "win.rescan", false));
    boxAdd(bar, toolButton("process-stop", "Durdur", "win.stop", false));
    sep();
    boxAdd(bar, toolButton("zoom-in", "Yakınlaştır", "win.zoom-in", false));
    boxAdd(bar, toolButton("zoom-out", "Uzaklaştır", "win.zoom-out", false));
    boxAdd(bar, toolButton("zoom-original", "Kök", "win.zoom-root", false));
    sep();
    boxAdd(bar, toolButton("view-statistics", "Görünür boyut", "win.apparent", true,
                           "Açık: dosyaların görünür boyutu (ls)\nKapalı: diskte kapladığı alan (du)"));
    boxAdd(bar, toolButton("drive-harddisk", "Tek dosya sistemi", "win.onefs", true,
                           "Başka disklere/bağlama noktalarına geçme\n(aynı aygıttaki btrfs alt birimleri dahil edilir)"));
    boxAdd(bar, toolButton("view-group", "Dosyaları grupla", "win.group", true,
                           "Bir klasördeki çok sayıda benzer öğeyi (oyun asset'leri gibi) tek kutuda topla:\n"
                           "aynı uzantılı dosyalar, küçük dosyalar ve küçük alt klasörler"));
    boxAdd(bar, toolButton("view-split-top-bottom", "Treemap", "win.treemap", true));
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    boxAdd(bar, spacer, true);
    GMenu *menu = g_menu_new();
    GMenu *thr = g_menu_new();
    for (uint32_t n : GROUP_MIN_CHOICES) {
        const std::string label = "En az " + std::to_string(n) + " öğe";
        GMenuItem *it = g_menu_item_new(label.c_str(), nullptr);
        g_menu_item_set_action_and_target_value(it, "win.group-min", g_variant_new_uint32(n));
        g_menu_append_item(thr, it);
        g_object_unref(it);
    }
    g_menu_append_section(menu, "Gruplama eşiği", G_MENU_MODEL(thr));
    GMenu *themes = g_menu_new();
    for (auto [id, label] : {std::pair{"system", "Sistem"}, {"light", "Açık"}, {"dark", "Koyu"}}) {
        GMenuItem *it = g_menu_item_new(label, nullptr);
        g_menu_item_set_action_and_target_value(it, "win.theme", g_variant_new_string(id));
        g_menu_append_item(themes, it);
        g_object_unref(it);
    }
    g_menu_append_section(menu, "Tema", G_MENU_MODEL(themes));
    g_object_unref(themes);
    GMenu *other = g_menu_new();
    g_menu_append(other, "Hakkında", "win.about");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(other));
    GtkWidget *mb = gtk_menu_button_new();
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(mb), G_MENU_MODEL(menu));
#if DT_GTK4
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(mb), "open-menu-symbolic");
#else
    gtk_button_set_image(GTK_BUTTON(mb), gtk_image_new_from_icon_name("open-menu-symbolic", GTK_ICON_SIZE_SMALL_TOOLBAR));
#endif
    gtk_widget_set_tooltip_text(mb, "Menü");
    g_object_unref(thr);
    g_object_unref(other);
    g_object_unref(menu);
    boxAdd(bar, mb);
    return bar;
}

GtkWidget *App::buildTree()
{
    model_ = static_cast<DtModel *>(g_object_new(dt_model_get_type(), nullptr));
    tree_ = gtk_tree_view_new_with_model(GTK_TREE_MODEL(model_));  // model_ referansı bizde kalır
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(tree_), FALSE);
    gtk_tree_view_set_fixed_height_mode(GTK_TREE_VIEW(tree_), TRUE);
    gtk_widget_set_hexpand(tree_, TRUE);
    gtk_widget_set_vexpand(tree_, TRUE);
    for (int c = 0; c < NCOLS; ++c) {
        GtkTreeViewColumn *col = gtk_tree_view_column_new();
        gtk_tree_view_column_set_title(col, COL_TITLES[c]);
        gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);
        gtk_tree_view_column_set_fixed_width(col, COL_WIDTHS[c]);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_column_set_clickable(col, TRUE);
        g_object_set_data(G_OBJECT(col), "dt-col", GINT_TO_POINTER(c));
        g_signal_connect(col, "clicked", DT_CB(+[](GtkTreeViewColumn *cl, gpointer self) {
                             static_cast<App *>(self)->setSortColumn(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(cl), "dt-col")));
                         }),
                         this);
        GtkCellRenderer *cell;
        if (c == C_NAME) {
            GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
            gtk_tree_view_column_pack_start(col, icon, FALSE);
            gtk_tree_view_column_set_cell_data_func(col, icon, nameIconData, this, nullptr);
            cell = gtk_cell_renderer_text_new();
            g_object_set(cell, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
            gtk_tree_view_column_set_expand(col, TRUE);
        } else if (c == C_BAR) {
            cell = gtk_cell_renderer_pixbuf_new();
            g_object_set(cell, "xalign", 0.0, "xpad", 6, nullptr);
        } else {
            cell = gtk_cell_renderer_text_new();
            g_object_set(cell, "xalign", 1.0, nullptr);
            gtk_tree_view_column_set_alignment(col, 1.0);
        }
        gtk_tree_view_column_pack_start(col, cell, TRUE);
        gtk_tree_view_column_set_cell_data_func(col, cell, cellData, this, nullptr);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tree_), col);
        cols_[c] = col;
    }
    gtk_tree_view_set_expander_column(GTK_TREE_VIEW(tree_), cols_[C_NAME]);
    setSortColumn(-1);  // yalnızca göstergeyi çiz

    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_));
    gtk_tree_selection_set_mode(sel, GTK_SELECTION_SINGLE);
    g_signal_connect(sel, "changed", DT_CB(+[](GtkTreeSelection *, gpointer self) {
                         App *a = static_cast<App *>(self);
                         a->selected_ = a->selectedNode();
                         a->composeTreemap();
                         if (a->selected_)
                             a->setStatus(a->selected_);
                     }),
                     this);
    g_signal_connect(tree_, "row-activated", DT_CB(+[](GtkTreeView *tv, GtkTreePath *path, GtkTreeViewColumn *, gpointer) {
                         GtkTreeIter it;
                         if (!gtk_tree_model_get_iter(gtk_tree_view_get_model(tv), &it, path))
                             return;
                         Node *n = dtNode(&it);
                         if (!n->isDir)
                             openPath(n->path());
                         else if (gtk_tree_view_row_expanded(tv, path))
                             gtk_tree_view_collapse_row(tv, path);
                         else
                             gtk_tree_view_expand_row(tv, path, FALSE);
                     }),
                     nullptr);

    // Sağ tık menüsü
#if DT_GTK4
    GtkGesture *g = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g), GDK_BUTTON_SECONDARY);
    g_signal_connect(g, "pressed", DT_CB(+[](GtkGestureClick *, int, double x, double y, gpointer self) {
                         App *a = static_cast<App *>(self);
                         int bx, by;
                         gtk_tree_view_convert_widget_to_bin_window_coords(GTK_TREE_VIEW(a->tree_), int(x), int(y), &bx, &by);
                         GtkTreePath *path = nullptr;
                         if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(a->tree_), bx, by, &path, nullptr, nullptr, nullptr))
                             return;
                         GtkTreeIter it;
                         gtk_tree_model_get_iter(GTK_TREE_MODEL(a->model_), &it, path);
                         gtk_tree_view_set_cursor(GTK_TREE_VIEW(a->tree_), path, nullptr, FALSE);
                         gtk_tree_path_free(path);
                         a->showMenu(dtNode(&it), a->tree_, x, y, nullptr);
                     }),
                     this);
    gtk_widget_add_controller(tree_, GTK_EVENT_CONTROLLER(g));
#else
    g_signal_connect(tree_, "button-press-event", DT_CB(+[](GtkWidget *w, GdkEventButton *ev, gpointer self) -> gboolean {
                         if (ev->type != GDK_BUTTON_PRESS || ev->button != GDK_BUTTON_SECONDARY)
                             return FALSE;
                         App *a = static_cast<App *>(self);
                         GtkTreePath *path = nullptr;
                         if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), int(ev->x), int(ev->y), &path, nullptr, nullptr, nullptr))
                             return FALSE;
                         GtkTreeIter it;
                         gtk_tree_model_get_iter(GTK_TREE_MODEL(a->model_), &it, path);
                         gtk_tree_view_set_cursor(GTK_TREE_VIEW(w), path, nullptr, FALSE);
                         gtk_tree_path_free(path);
                         a->showMenu(dtNode(&it), w, ev->x, ev->y, ev);
                         return TRUE;
                     }),
                     this);
#endif
    return tree_;
}

enum ExtCol { E_ID, E_SWATCH, E_LABEL, E_DESC, E_SIZE, E_PCT, E_COUNT, E_BYTES, E_NUM, E_NCOLS };

GtkWidget *App::buildExtList()
{
    extStore_ = gtk_list_store_new(E_NCOLS, G_TYPE_INT, GDK_TYPE_PIXBUF, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                   G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT64, G_TYPE_UINT64);
    extView_ = gtk_tree_view_new_with_model(GTK_TREE_MODEL(extStore_));  // extStore_ referansı bizde kalır
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(extView_), FALSE);
    gtk_widget_set_vexpand(extView_, TRUE);

    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(col, "Uzantı");
    GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(col, pix, FALSE);
    gtk_tree_view_column_add_attribute(col, pix, "pixbuf", E_SWATCH);
    GtkCellRenderer *txt = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col, txt, TRUE);
    gtk_tree_view_column_add_attribute(col, txt, "text", E_LABEL);
    gtk_tree_view_column_set_sort_column_id(col, E_LABEL);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(extView_), col);

    struct {
        const char *title;
        int text, sort;
        bool right, expand;
    } spec[] = {{"Tür", E_DESC, E_DESC, false, true},
                {"Boyut", E_SIZE, E_BYTES, true, false},
                {"%", E_PCT, E_BYTES, true, false},
                {"Dosya", E_COUNT, E_NUM, true, false}};
    for (auto &c : spec) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        if (c.right)
            g_object_set(r, "xalign", 1.0, nullptr);
        else
            g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
        GtkTreeViewColumn *tc = gtk_tree_view_column_new_with_attributes(c.title, r, "text", c.text, nullptr);
        gtk_tree_view_column_set_sort_column_id(tc, c.sort);
        gtk_tree_view_column_set_resizable(tc, TRUE);
        gtk_tree_view_column_set_expand(tc, c.expand);
        if (c.right)
            gtk_tree_view_column_set_alignment(tc, 1.0);
        gtk_tree_view_append_column(GTK_TREE_VIEW(extView_), tc);
    }
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(extStore_), E_BYTES, GTK_SORT_DESCENDING);
    gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(extView_), E_DESC);  // kesilen açıklamanın tamamı

    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(extView_)), "changed",
                     DT_CB(+[](GtkTreeSelection *sel, gpointer self) {
                         App *a = static_cast<App *>(self);
                         GtkTreeModel *m;
                         GtkTreeIter it;
                         int ext = -1;
                         if (gtk_tree_selection_get_selected(sel, &m, &it))
                             gtk_tree_model_get(m, &it, E_ID, &ext, -1);
                         a->highlight_ = ext;
                         a->composeTreemap();
                     }),
                     this);
    return extView_;
}

GtkWidget *App::buildTreemap()
{
    tmBox_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    zoomLabel_ = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(zoomLabel_), 0);
    gtk_label_set_ellipsize(GTK_LABEL(zoomLabel_), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_margin_start(zoomLabel_, 6);
    gtk_widget_set_margin_top(zoomLabel_, 2);
    gtk_widget_set_margin_bottom(zoomLabel_, 2);
    boxAdd(tmBox_, zoomLabel_);

    area_ = gtk_drawing_area_new();
    gtk_widget_set_size_request(area_, -1, 80);
    boxAdd(tmBox_, area_, true);

    auto draw = [](GtkWidget *w, cairo_t *cr, App *a) {
        const int ww = gtk_widget_get_allocated_width(w), wh = gtk_widget_get_allocated_height(w);
        if (!a->surface_) {
            cairo_set_source_rgb(cr, 0.12, 0.12, 0.12);
            cairo_paint(cr);
            return;
        }
        cairo_save(cr);
        cairo_scale(cr, double(ww) / a->tm_.width(), double(wh) / a->tm_.height());
        cairo_set_source_surface(cr, a->surface_, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
        cairo_paint(cr);
        cairo_restore(cr);
    };
#if DT_GTK4
    static decltype(draw) drawFn = draw;
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area_), [](GtkDrawingArea *w, cairo_t *cr, int, int, gpointer self) {
        drawFn(GTK_WIDGET(w), cr, static_cast<App *>(self));
    }, this, nullptr);
    g_signal_connect(area_, "resize", DT_CB(+[](GtkDrawingArea *, int, int, gpointer self) {
                         static_cast<App *>(self)->scheduleRender(150);
                     }),
                     this);
    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", DT_CB(+[](GtkEventControllerMotion *, double x, double y, gpointer self) {
                         App *a = static_cast<App *>(self);
                         Node *n = a->treemapHit(x, y);
                         if (n != a->hover_) {
                             a->hover_ = n;
                             a->setStatus(n ? n : a->selected_);
                         }
                     }),
                     this);
    g_signal_connect(motion, "leave", DT_CB(+[](GtkEventControllerMotion *, gpointer self) {
                         App *a = static_cast<App *>(self);
                         a->hover_ = nullptr;
                         a->setStatus(a->selected_);
                     }),
                     this);
    gtk_widget_add_controller(area_, motion);
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "pressed", DT_CB(+[](GtkGestureClick *g, int n, double x, double y, gpointer self) {
                         int button = int(gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g)));
                         static_cast<App *>(self)->treemapPress(button, n, x, y, nullptr);
                     }),
                     this);
    gtk_widget_add_controller(area_, GTK_EVENT_CONTROLLER(click));
#else
    static decltype(draw) drawFn = draw;
    g_signal_connect(area_, "draw", DT_CB(+[](GtkWidget *w, cairo_t *cr, gpointer self) -> gboolean {
                         drawFn(w, cr, static_cast<App *>(self));
                         return TRUE;
                     }),
                     this);
    g_signal_connect(area_, "size-allocate", DT_CB(+[](GtkWidget *, GdkRectangle *, gpointer self) {
                         static_cast<App *>(self)->scheduleRender(150);
                     }),
                     this);
    gtk_widget_add_events(area_, GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(area_, "motion-notify-event", DT_CB(+[](GtkWidget *, GdkEventMotion *ev, gpointer self) -> gboolean {
                         App *a = static_cast<App *>(self);
                         Node *n = a->treemapHit(ev->x, ev->y);
                         if (n != a->hover_) {
                             a->hover_ = n;
                             a->setStatus(n ? n : a->selected_);
                         }
                         return FALSE;
                     }),
                     this);
    g_signal_connect(area_, "leave-notify-event", DT_CB(+[](GtkWidget *, GdkEventCrossing *, gpointer self) -> gboolean {
                         App *a = static_cast<App *>(self);
                         a->hover_ = nullptr;
                         a->setStatus(a->selected_);
                         return FALSE;
                     }),
                     this);
    g_signal_connect(area_, "button-press-event", DT_CB(+[](GtkWidget *, GdkEventButton *ev, gpointer self) -> gboolean {
                         int n = ev->type == GDK_2BUTTON_PRESS ? 2 : ev->type == GDK_BUTTON_PRESS ? 1 : 0;
                         if (n)
                             static_cast<App *>(self)->treemapPress(int(ev->button), n, ev->x, ev->y, ev);
                         return TRUE;
                     }),
                     this);
#endif
    return tmBox_;
}

GtkWidget *App::buildStatus()
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(bar, 6);
    gtk_widget_set_margin_end(bar, 6);
    gtk_widget_set_margin_top(bar, 3);
    gtk_widget_set_margin_bottom(bar, 3);
    statusL_ = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(statusL_), 0);
    gtk_label_set_ellipsize(GTK_LABEL(statusL_), PANGO_ELLIPSIZE_MIDDLE);
    spinner_ = gtk_spinner_new();
    statusR_ = gtk_label_new(nullptr);
    boxAdd(bar, statusL_, true);
    boxAdd(bar, spinner_);
    boxAdd(bar, statusR_);
    return bar;
}

// ───────────── hücre verisi ─────────────

void App::nameIconData(GtkTreeViewColumn *, GtkCellRenderer *cell, GtkTreeModel *, GtkTreeIter *it, gpointer self)
{
    g_object_set(cell, "gicon", static_cast<App *>(self)->iconFor(dtNode(it)), nullptr);
}

void App::cellData(GtkTreeViewColumn *col, GtkCellRenderer *cell, GtkTreeModel *, GtkTreeIter *it, gpointer self)
{
    App *a = static_cast<App *>(self);
    const Node *n = dtNode(it);
    const bool ap = a->s_.apparent;
    const int c = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(col), "dt-col"));
    auto frac = [&] {
        if (n == a->model_->root || !n->parent)
            return 1.0;
        const uint64_t tot = n->parent->sizeOf(ap);
        return tot ? double(n->sizeOf(ap)) / double(tot) : 0.0;
    };
    std::string text;
    switch (c) {
    case C_NAME:
        text = displayName(n, n == a->model_->root);
        g_object_set(cell, "text", text.c_str(), "foreground", (n->flags & F_ERROR) ? "#dc4646" : "#8c8c8c",
                     "foreground-set", (n->flags & (F_ERROR | F_MOUNT)) != 0, nullptr);
        return;
    case C_BAR:
        g_object_set(cell, "pixbuf", barPixbuf(int(frac() * 100 + 0.5), n->depth()), nullptr);
        return;
    case C_PCT:
        text = fmtPct(frac());
        break;
    case C_SIZE:
        text = fmtSize(n->sizeOf(ap));
        break;
    case C_ITEMS:
        text = n->isDir ? fmtInt(uint64_t(n->files) + n->dirs) : "";
        break;
    case C_FILES:
        text = n->isDir ? fmtInt(n->files) : "";
        break;
    case C_DIRS:
        text = n->isDir ? fmtInt(n->dirs) : "";
        break;
    case C_MTIME:
        text = fmtTime(n->mtime);
        break;
    }
    g_object_set(cell, "text", text.c_str(), nullptr);
}

GIcon *App::iconFor(const Node *n)
{
    if (n->flags & F_GROUP)
        return groupIcon_;
    if (n->isDir)
        return dirIcon_;
    if (n->flags & F_LINK)
        return linkIcon_;
    if (!n->ext)
        return fileIcon_;
    auto it = icons_.find(n->ext);
    if (it != icons_.end())
        return it->second;
    const std::string fake = "x." + ExtTable::name(n->ext);
    gchar *ct = g_content_type_guess(fake.c_str(), nullptr, 0, nullptr);
    GIcon *ic = g_content_type_get_icon(ct);
    g_free(ct);
    icons_.emplace(n->ext, ic);
    return ic;
}

std::string App::extDescription(uint32_t ext)
{
    auto it = extDesc_.find(ext);
    if (it != extDesc_.end())
        return it->second;
    std::string sys;
    if (ext) {
        const std::string fake = "x." + ExtTable::name(ext);
        gchar *ct = g_content_type_guess(fake.c_str(), nullptr, 0, nullptr);
        if (!g_content_type_is_unknown(ct)) {
            gchar *desc = g_content_type_get_description(ct);
            sys = desc;
            g_free(desc);
        }
        g_free(ct);
    }
    const std::string d = describeExt(ext, sys);
    extDesc_.emplace(ext, d);
    return d;
}

// ───────────── tarama ─────────────

void App::chooseTarget()
{
    GtkWidget *dlg = gtk_window_new(
#if !DT_GTK4
        GTK_WINDOW_TOPLEVEL
#endif
    );
    gtk_window_set_title(GTK_WINDOW(dlg), "TuxDirStat — Taranacak konum");
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(win_));
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 720, 400);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 10);
    gtk_widget_set_margin_end(box, 10);
    gtk_widget_set_margin_top(box, 10);
    gtk_widget_set_margin_bottom(box, 10);
    GtkWidget *lbl = gtk_label_new("Taramak istediğiniz konumu seçin:");
    gtk_label_set_xalign(GTK_LABEL(lbl), 0);
    boxAdd(box, lbl);

    enum { D_ICON, D_LABEL, D_FS, D_USED, D_TOTAL, D_PCT, D_PATH, D_N };
    GtkListStore *st = gtk_list_store_new(D_N, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                          G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING);
    const std::string home = homeDir();
    gtk_list_store_insert_with_values(st, nullptr, -1, D_ICON, "user-home", D_LABEL,
                                      ("Ev klasörü  (" + home + ")").c_str(), D_PCT, -1, D_PATH, home.c_str(), -1);
    for (const Drive &d : listDrives())
        gtk_list_store_insert_with_values(st, nullptr, -1, D_ICON, "drive-harddisk", D_LABEL, d.mp.c_str(), D_FS,
                                          d.fs.c_str(), D_USED, fmtSize(d.used).c_str(), D_TOTAL,
                                          fmtSize(d.total).c_str(), D_PCT, int(d.used * 100 / d.total), D_PATH,
                                          d.mp.c_str(), -1);
    GtkWidget *list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(st));
    g_object_unref(st);
    GtkTreeViewColumn *c0 = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(c0, "Konum");
    GtkCellRenderer *ir = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(c0, ir, FALSE);
    gtk_tree_view_column_add_attribute(c0, ir, "icon-name", D_ICON);
    GtkCellRenderer *tr = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(c0, tr, TRUE);
    gtk_tree_view_column_add_attribute(c0, tr, "text", D_LABEL);
    gtk_tree_view_column_set_expand(c0, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(list), c0);
    struct {
        const char *t;
        int col;
    } cs[] = {{"Tür", D_FS}, {"Kullanılan", D_USED}, {"Toplam", D_TOTAL}};
    for (auto &c : cs) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        if (c.col != D_FS)
            g_object_set(r, "xalign", 1.0, nullptr);
        gtk_tree_view_append_column(GTK_TREE_VIEW(list), gtk_tree_view_column_new_with_attributes(c.t, r, "text", c.col, nullptr));
    }
    GtkCellRenderer *pr = gtk_cell_renderer_pixbuf_new();
    g_object_set(pr, "xalign", 0.0, nullptr);
    GtkTreeViewColumn *pc = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(pc, "Doluluk");
    gtk_tree_view_column_pack_start(pc, pr, TRUE);
    gtk_tree_view_column_set_cell_data_func(pc, pr, [](GtkTreeViewColumn *, GtkCellRenderer *cell, GtkTreeModel *m, GtkTreeIter *it, gpointer) {
        int v;
        gtk_tree_model_get(m, it, D_PCT, &v, -1);
        g_object_set(cell, "visible", v >= 0, "pixbuf", barPixbuf(std::max(v, 0), 0), nullptr);
    }, nullptr, nullptr);
    gtk_tree_view_column_set_min_width(pc, 120);
    gtk_tree_view_append_column(GTK_TREE_VIEW(list), pc);
    GtkTreePath *first = gtk_tree_path_new_first();
    gtk_tree_view_set_cursor(GTK_TREE_VIEW(list), first, nullptr, FALSE);
    gtk_tree_path_free(first);
    boxAdd(box, scrolled(list), true);

    struct Ctx {
        App *app;
        GtkWidget *dlg, *list;
    };
    auto *ctx = new Ctx{this, dlg, list};
    g_object_set_data_full(G_OBJECT(dlg), "dt-ctx", ctx, [](gpointer p) { delete static_cast<Ctx *>(p); });
    auto accept = [](Ctx *c) {
        GtkTreeModel *m;
        GtkTreeIter it;
        if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(c->list)), &m, &it))
            return;
        gchar *path;
        gtk_tree_model_get(m, &it, D_PATH, &path, -1);
        std::string p = path;
        g_free(path);
        App *app = c->app;
        destroyWindow(c->dlg);
        app->startScan(p);
    };
    static decltype(accept) acceptFn = accept;
    g_signal_connect(list, "row-activated", DT_CB(+[](GtkTreeView *, GtkTreePath *, GtkTreeViewColumn *, gpointer c) {
                         acceptFn(static_cast<Ctx *>(c));
                     }),
                     ctx);

    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *pick = gtk_button_new_with_label("Klasör seç…");
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *cancel = gtk_button_new_with_label("İptal");
    GtkWidget *ok = gtk_button_new_with_label("Tara");
#if DT_GTK4
    gtk_widget_add_css_class(ok, "suggested-action");
#else
    gtk_style_context_add_class(gtk_widget_get_style_context(ok), "suggested-action");
#endif
    boxAdd(btns, pick);
    boxAdd(btns, spacer, true);
    boxAdd(btns, cancel);
    boxAdd(btns, ok);
    boxAdd(box, btns);
    g_signal_connect(ok, "clicked", DT_CB(+[](GtkButton *, gpointer c) { acceptFn(static_cast<Ctx *>(c)); }), ctx);
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(destroyWindow), dlg);
    g_signal_connect(pick, "clicked", DT_CB(+[](GtkButton *, gpointer cp) {
                         Ctx *c = static_cast<Ctx *>(cp);
                         App *app = c->app;
                         GtkWidget *dlg = c->dlg;
                         pickFolder(GTK_WINDOW(dlg), [app, dlg](const std::string &p) {
                             destroyWindow(dlg);
                             app->startScan(p);
                         });
                     }),
                     ctx);
    windowSetChild(dlg, box);
#if DT_GTK4
    gtk_window_present(GTK_WINDOW(dlg));
#else
    gtk_widget_show_all(dlg);
#endif
}

void App::startScan(const std::string &path, Node *target)
{
    char *real = realpath(path.c_str(), nullptr);
    const std::string abs = real ? real : path;
    free(real);
    s_.startScan(abs, target);
    setActionEnabled("stop", true);
    gtk_spinner_start(GTK_SPINNER(spinner_));
    if (!target)
        gtk_window_set_title(GTK_WINDOW(win_), ("TuxDirStat — " + abs).c_str());
    if (!pollId_)
        pollId_ = g_timeout_add(100, [](gpointer p) -> gboolean {
            App *a = static_cast<App *>(p);
            if (a->poll())
                return G_SOURCE_CONTINUE;
            a->pollId_ = 0;
            return G_SOURCE_REMOVE;
        }, this);
}

bool App::poll()
{
    const Session::Result res = s_.poll();
    if (res == Session::Result::Running) {
        const std::string txt = "Taranıyor… " + fmtInt(s_.progressCount()) + " öğe · " +
                                fmtSize(s_.progressBytes()) + " · " + s_.progressPath();
        gtk_label_set_text(GTK_LABEL(statusL_), txt.c_str());
        return true;
    }
    gtk_spinner_stop(GTK_SPINNER(spinner_));
    setActionEnabled("stop", false);
    switch (res) {
    case Session::Result::Failed:
        gtk_label_set_text(GTK_LABEL(statusL_), "");
        showError(GTK_WINDOW(win_), "Tarama başarısız", s_.error());
        return false;
    case Session::Result::Cancelled:
        gtk_label_set_text(GTK_LABEL(statusL_), "Tarama durduruldu.");
        return false;
    case Session::Result::NewTree: {
        Node *root = s_.pendingRoot();
        tm_.forgetNodes();
        tmRoot_ = root;
        selected_ = hover_ = menuNode_ = nullptr;
        setModelRoot(root);
        s_.commitNewTree();
        GtkTreePath *p = gtk_tree_path_new_first();
        gtk_tree_view_expand_row(GTK_TREE_VIEW(tree_), p, FALSE);
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(tree_), p, nullptr, FALSE);
        gtk_tree_path_free(p);
        break;
    }
    case Session::Result::Refresh: {
        Node *target = s_.refreshTarget();
        tm_.forgetNodes();
        hover_ = menuNode_ = nullptr;
        if (tmRoot_ && tmRoot_ != target && tmRoot_->isWithin(target))
            tmRoot_ = target;
        if (selected_ && selected_ != target && selected_->isWithin(target))
            selected_ = target;
        // Görünümü modelden ayırıp yeniden bağlar; açık dalları geri açar.
        std::vector<Node *> expanded;
        gtk_tree_view_map_expanded_rows(GTK_TREE_VIEW(tree_), [](GtkTreeView *tv, GtkTreePath *path, gpointer v) {
            GtkTreeIter it;
            if (gtk_tree_model_get_iter(gtk_tree_view_get_model(tv), &it, path))
                static_cast<std::vector<Node *> *>(v)->push_back(dtNode(&it));
        }, &expanded);
        gtk_tree_view_set_model(GTK_TREE_VIEW(tree_), nullptr);
        takeChildren(target, s_.pendingRoot());
        s_.commitRefresh();
        gtk_tree_view_set_model(GTK_TREE_VIEW(tree_), GTK_TREE_MODEL(model_));
        expanded.push_back(target);
        for (Node *n : expanded) {
            if (n != target && n->isWithin(target))
                continue;  // eski alt ağaç
            GtkTreePath *p = dtPathFor(model_, n);
            gtk_tree_view_expand_to_path(GTK_TREE_VIEW(tree_), p);
            gtk_tree_path_free(p);
        }
        if (selected_)
            selectNode(selected_);
        break;
    }
    default:
        return false;
    }
    afterChange();
    const std::string done = "Tarama tamamlandı: " + fmtInt(s_.lastCount()) + " öğe, " +
                             std::to_string(s_.elapsed()).substr(0, 4) + " sn.";
    gtk_label_set_text(GTK_LABEL(statusL_), done.c_str());
    return false;
}

void App::setModelRoot(Node *root)
{
    gtk_tree_view_set_model(GTK_TREE_VIEW(tree_), nullptr);
    model_->root = root;
    model_->sorted->clear();
    ++model_->gen;
    ++model_->stamp;
    if (root)
        root->row = 0;
    gtk_tree_view_set_model(GTK_TREE_VIEW(tree_), GTK_TREE_MODEL(model_));
}

// Sıralama ölçütü değişti: açık dalları ve seçimi koruyarak yeniden sıralar.
void App::resortView()
{
    if (!model_->root)
        return;
    std::vector<Node *> expanded;
    gtk_tree_view_map_expanded_rows(GTK_TREE_VIEW(tree_), [](GtkTreeView *tv, GtkTreePath *path, gpointer v) {
        GtkTreeIter it;
        if (gtk_tree_model_get_iter(gtk_tree_view_get_model(tv), &it, path))
            static_cast<std::vector<Node *> *>(v)->push_back(dtNode(&it));
    }, &expanded);
    Node *sel = selectedNode();
    gtk_tree_view_set_model(GTK_TREE_VIEW(tree_), nullptr);
    model_->key = COL_KEYS[sortCol_];
    model_->desc = sortDesc_;
    model_->sorted->clear();
    ++model_->gen;
    gtk_tree_view_set_model(GTK_TREE_VIEW(tree_), GTK_TREE_MODEL(model_));
    for (Node *n : expanded) {
        GtkTreePath *p = dtPathFor(model_, n);
        gtk_tree_view_expand_to_path(GTK_TREE_VIEW(tree_), p);
        gtk_tree_path_free(p);
    }
    if (sel)
        selectNode(sel);
}

void App::setSortColumn(int col)
{
    if (col >= 0) {
        if (col == sortCol_)
            sortDesc_ = !sortDesc_;
        else
            sortDesc_ = col != C_NAME;
        sortCol_ = col;
        resortView();
    }
    for (int c = 0; c < NCOLS; ++c) {
        gtk_tree_view_column_set_sort_indicator(cols_[c], c == sortCol_);
        gtk_tree_view_column_set_sort_order(cols_[c], sortDesc_ ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING);
    }
}

// Ağaçta bir değişiklikten sonra uzantıları, treemap'i ve toplamları tazeler.
void App::afterChange()
{
    fillExt();
    scheduleRender(0);
    updateTotals();
    updateZoomLabel();
    gtk_widget_queue_draw(tree_);
}

// ───────────── görünümler ─────────────

void App::fillExt()
{
    Node *root = s_.root();
    const double total = root ? std::max<uint64_t>(1, root->sizeOf(s_.apparent)) : 1;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(extView_));
    g_signal_handlers_block_matched(sel, G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, this);
    gtk_tree_view_set_model(GTK_TREE_VIEW(extView_), nullptr);
    gtk_list_store_clear(extStore_);
    for (const ExtRow &e : s_.ext) {
        GdkPixbuf *pb = swatch(e.color);
        gtk_list_store_insert_with_values(extStore_, nullptr, -1, E_ID, int(e.ext), E_SWATCH, pb, E_LABEL,
                                          s_.extLabel(e.ext).c_str(), E_DESC, extDescription(e.ext).c_str(), E_SIZE,
                                          fmtSize(e.bytes).c_str(), E_PCT, fmtPct(e.bytes / total).c_str(), E_COUNT,
                                          fmtInt(e.count).c_str(), E_BYTES, guint64(e.bytes), E_NUM, guint64(e.count), -1);
        g_object_unref(pb);
    }
    gtk_tree_view_set_model(GTK_TREE_VIEW(extView_), GTK_TREE_MODEL(extStore_));
    g_signal_handlers_unblock_matched(sel, G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, this);
    highlight_ = -1;
}

void App::updateTotals()
{
    Node *root = s_.root();
    if (!root) {
        gtk_label_set_text(GTK_LABEL(statusR_), "");
        return;
    }
    const std::string txt = "Toplam: " + fmtSize(root->sizeOf(s_.apparent)) + " · " + fmtInt(root->files) +
                            " dosya · " + fmtInt(root->dirs) + " klasör · Boş alan: " + fmtSize(freeSpace(root->path()));
    gtk_label_set_text(GTK_LABEL(statusR_), txt.c_str());
}

void App::updateZoomLabel()
{
    const std::string m = tmRoot_ ? "Treemap: <b>" + markupEscape(tmRoot_->path()) + "</b>" : "Treemap";
    gtk_label_set_markup(GTK_LABEL(zoomLabel_), m.c_str());
    setActionEnabled("zoom-out", tmRoot_ && tmRoot_->parent);
}

void App::setStatus(Node *n)
{
    if (!n || s_.scanning())
        return;
    const std::string txt = n->path() + "  —  " + fmtSize(n->sizeOf(s_.apparent));
    gtk_label_set_text(GTK_LABEL(statusL_), txt.c_str());
}

Node *App::selectedNode()
{
    GtkTreeModel *m;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_)), &m, &it))
        return nullptr;
    return dtNode(&it);
}

void App::selectNode(Node *n)
{
    if (!n || !model_->root || !n->isWithin(model_->root))
        return;
    GtkTreePath *p = dtPathFor(model_, n);
    if (gtk_tree_path_get_depth(p) > 1) {
        GtkTreePath *up = gtk_tree_path_copy(p);
        gtk_tree_path_up(up);
        gtk_tree_view_expand_to_path(GTK_TREE_VIEW(tree_), up);
        gtk_tree_path_free(up);
    }
    gtk_tree_view_set_cursor(GTK_TREE_VIEW(tree_), p, nullptr, FALSE);
    gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(tree_), p, nullptr, FALSE, 0, 0);
    gtk_tree_path_free(p);
}

void App::zoomTo(Node *n)
{
    if (!n || !n->isDir)
        return;
    tmRoot_ = n;
    tm_.forgetNodes();
    scheduleRender(0);
    updateZoomLabel();
}

// ───────────── treemap ─────────────

void App::scheduleRender(int ms)
{
    if (renderId_)
        g_source_remove(renderId_);
    renderId_ = g_timeout_add(guint(ms), [](gpointer p) -> gboolean {
        App *a = static_cast<App *>(p);
        a->renderId_ = 0;
        a->renderTreemap();
        return G_SOURCE_REMOVE;
    }, this);
}

void App::renderTreemap()
{
    const int scale = gtk_widget_get_scale_factor(area_);
    tm_.render(tmRoot_, gtk_widget_get_allocated_width(area_) * scale, gtk_widget_get_allocated_height(area_) * scale,
               s_.apparent, s_.colors);
    composeTreemap();
}

void App::composeTreemap()
{
    tm_.compose(tmPixels_, highlight_, selected_);
    if (surface_) {
        cairo_surface_destroy(surface_);
        surface_ = nullptr;
    }
    if (!tmPixels_.empty())
        surface_ = cairo_image_surface_create_for_data(reinterpret_cast<unsigned char *>(tmPixels_.data()),
                                                       CAIRO_FORMAT_RGB24, tm_.width(), tm_.height(), tm_.width() * 4);
    gtk_widget_queue_draw(area_);
}

Node *App::treemapHit(double x, double y)
{
    const int ww = gtk_widget_get_allocated_width(area_), wh = gtk_widget_get_allocated_height(area_);
    if (ww <= 0 || wh <= 0)
        return nullptr;
    return tm_.hit(int(x / ww * tm_.width()), int(y / wh * tm_.height()));
}

void App::treemapPress(int button, int nPress, double x, double y, void *event)
{
    Node *n = treemapHit(x, y);
    if (!n)
        return;
    if (nPress == 2 && button == GDK_BUTTON_PRIMARY) {
        while (n && n->parent != tmRoot_)
            n = n->parent;
        if (n && n->isDir)
            zoomTo(n);
        return;
    }
    selectNode(n);
    if (button == GDK_BUTTON_SECONDARY)
        showMenu(n, area_, x, y, event);
}

// ───────────── bağlam menüsü ─────────────

void App::showMenu(Node *n, GtkWidget *widget, double x, double y, void *event)
{
    menuNode_ = n;
    GMenu *menu = g_menu_new();
    GMenu *s1 = g_menu_new();
    g_menu_append(s1, "Aç", "win.node-open");
    g_menu_append(s1, "Dosya yöneticisinde göster", "win.node-reveal");
    g_menu_append(s1, "Yolu kopyala", "win.node-copy");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(s1));
    if (n->isDir) {
        GMenu *s2 = g_menu_new();
        g_menu_append(s2, "Treemap'te yakınlaştır", "win.node-zoom");
        g_menu_append(s2,
                      (n->flags & F_GROUP)   ? "Bulunduğu klasörü yeniden tara"
                      : (n->flags & F_MOUNT) ? "Bu dosya sistemini de tara"
                                             : "Bu klasörü yeniden tara",
                      "win.node-refresh");
        g_menu_append_section(menu, nullptr, G_MENU_MODEL(s2));
        g_object_unref(s2);
    }
    if (n->parent) {
        GMenu *s3 = g_menu_new();
        g_menu_append(s3, "Çöp kutusuna taşı", "win.node-trash");
        g_menu_append(s3, "Kalıcı olarak sil…", "win.node-delete");
        g_menu_append_section(menu, nullptr, G_MENU_MODEL(s3));
        g_object_unref(s3);
    }
    g_object_unref(s1);
    setActionEnabled("node-refresh", !s_.scanning());
    setActionEnabled("node-trash", !s_.scanning());
    setActionEnabled("node-delete", !s_.scanning());
#if DT_GTK4
    (void)event;
    GtkWidget *pop = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    gtk_widget_set_parent(pop, widget);
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
    GdkRectangle r{int(x), int(y), 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(pop), &r);
    gtk_widget_set_halign(pop, GTK_ALIGN_START);
    g_signal_connect(pop, "closed", DT_CB(+[](GtkPopover *p, gpointer) {
                         g_idle_add_once([](gpointer w) { gtk_widget_unparent(GTK_WIDGET(w)); }, p);
                     }),
                     nullptr);
    gtk_popover_popup(GTK_POPOVER(pop));
#else
    (void)x;
    (void)y;
    GtkWidget *m = gtk_menu_new_from_model(G_MENU_MODEL(menu));
    gtk_menu_attach_to_widget(GTK_MENU(m), widget, nullptr);
    g_signal_connect(m, "deactivate", DT_CB(+[](GtkMenuShell *ms, gpointer) {
                         g_idle_add_once([](gpointer w) { gtk_widget_destroy(GTK_WIDGET(w)); }, ms);
                     }),
                     nullptr);
    gtk_menu_popup_at_pointer(GTK_MENU(m), static_cast<GdkEvent *>(event));
#endif
    g_object_unref(menu);
}

// ───────────── değişiklikler ─────────────

void App::refreshNode(Node *n)
{
    n = realNode(n);  // grup için bulunduğu klasör
    if (n && !s_.scanning())
        startScan(n->path(), n);
}

void App::regroup(bool on, uint32_t minCount)
{
    // Eski gruplar ağaçtan ayrılır; seçimi ve yakınlaştırmayı gerçek düğüme taşı.
    Node *sel = realNode(selectedNode());
    Node *zoom = realNode(tmRoot_);
    tm_.forgetNodes();
    selected_ = hover_ = menuNode_ = nullptr;
    gtk_tree_view_set_model(GTK_TREE_VIEW(tree_), nullptr);
    s_.setGrouping(on, minCount);
    saveSettings();
    setModelRoot(s_.root());
    tmRoot_ = zoom ? zoom : s_.root();
    if (s_.root()) {
        GtkTreePath *p = gtk_tree_path_new_first();
        gtk_tree_view_expand_row(GTK_TREE_VIEW(tree_), p, FALSE);
        gtk_tree_path_free(p);
        selectNode(sel ? sel : s_.root());
    }
    scheduleRender(0);
    updateZoomLabel();
}

void App::deleteNode(Node *n, bool permanent)
{
    if (!n->parent || s_.scanning())
        return;
    const std::string path = n->path();
    const std::string kind = (n->flags & F_GROUP) ? "gruptaki " + fmtInt(n->files) + " dosyayı"
                             : n->isDir           ? "klasörü"
                                                  : "dosyayı";
    const std::string size = fmtSize(n->sizeOf(s_.apparent));
    const std::string title = permanent ? "Bu " + kind + " kalıcı olarak silmek istediğinize emin misiniz?"
                                        : "Bu " + kind + " çöp kutusuna taşınsın mı?";
    const std::string detail = path + "\n(" + size + ")" + (permanent ? "\n\nBu işlem geri alınamaz." : "");
    confirm(GTK_WINDOW(win_), title, detail, permanent ? "Kalıcı olarak sil" : "Çöpe taşı",
            [this, n, permanent](bool yes) {
                if (!yes || s_.scanning() || !s_.root() || !n->isWithin(s_.root()))
                    return;
                std::string err, firstFailed;
                int failed = 0;
                for (const std::string &p : diskPaths(n)) {
                    bool ok;
                    if (permanent) {
                        ok = removePath(p, &err);
                    } else {
                        GFile *f = g_file_new_for_path(p.c_str());
                        GError *e = nullptr;
                        ok = g_file_trash(f, nullptr, &e);
                        if (e) {
                            err = e->message;
                            g_error_free(e);
                        }
                        g_object_unref(f);
                        if (!ok)
                            ok = trashWithCommand(p);
                    }
                    if (!ok && !failed++)
                        firstFailed = p;
                }
                if (failed) {
                    showError(GTK_WINDOW(win_), "Silinemedi (ya da yalnızca kısmen silindi)", firstFailed + "\n\n" + err);
                    refreshNode(n->parent);  // ağacı diskle eşitle
                    return;
                }
                finishDelete(n);
            });
}

void App::finishDelete(Node *n)
{
    n = s_.removalTarget(n);  // grubun son öğesiyse grup da kalkar
    Node *p = n->parent;
    tm_.forgetNodes();
    hover_ = menuNode_ = nullptr;
    if (tmRoot_ && tmRoot_->isWithin(n))
        tmRoot_ = p;
    if (selected_ && selected_->isWithin(n))
        selected_ = nullptr;
    GtkTreePath *path = dtPathFor(model_, n);
    s_.remove(n);
    gtk_tree_model_row_deleted(GTK_TREE_MODEL(model_), path);
    if (p->children.empty() && gtk_tree_path_up(path)) {
        GtkTreeIter it;
        dtSet(model_, &it, p);
        gtk_tree_model_row_has_child_toggled(GTK_TREE_MODEL(model_), path, &it);
    }
    gtk_tree_path_free(path);
    afterChange();
}

// ───────────────────────────── main ─────────────────────────────

int main(int argc, char **argv)
{
    if (int rc = runCommandLine(argc, argv); rc >= 0)
        return rc;
    const std::string path = pathArgument(argc, argv);
#if DT_GTK4
    GtkApplication *app = gtk_application_new("io.github.tuxdirstat.Gtk4", G_APPLICATION_NON_UNIQUE);
#else
    GtkApplication *app = gtk_application_new("io.github.tuxdirstat.Gtk3", G_APPLICATION_NON_UNIQUE);
#endif
    g_set_application_name("TuxDirStat");
    auto *startPath = new std::string(path);
    g_signal_connect(app, "activate", DT_CB(+[](GtkApplication *a, gpointer p) {
                         new App(a, *static_cast<std::string *>(p));
                     }),
                     startPath);
    char *args[] = {argv[0], nullptr};
    int rc = g_application_run(G_APPLICATION(app), 1, args);
    g_object_unref(app);
    delete startPath;
    return rc;
}
