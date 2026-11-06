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
#include "i18n.h"
#include "tools.h"
#include "report.h"
#include "session.h"
#include "treemap_render.h"

#define DT_GTK4 GTK_CHECK_VERSION(4, 0, 0)
// Lambda içindeki virgüller makro argümanlarını bölmesin diye ek parantez.
#define DT_CB(...) G_CALLBACK((__VA_ARGS__))

namespace {

enum Col { C_NAME, C_BAR, C_PCT, C_SIZE, C_ITEMS, C_FILES, C_DIRS, C_MTIME, NCOLS };
const char *const COL_TITLES[NCOLS] = {N_("Name"), N_("Subtree"), "%", N_("Size"), N_("Items"), N_("Files"), N_("Folders"), N_("Modified")};
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
    const char *buttons[] = {T("Cancel"), yes, nullptr};
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
    gtk_dialog_add_buttons(GTK_DIALOG(d), T("Cancel"), GTK_RESPONSE_CANCEL, yes, GTK_RESPONSE_ACCEPT, nullptr);
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
    gtk_file_dialog_set_title(d, T("Choose folder"));
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
    GtkFileChooserNative *d = gtk_file_chooser_native_new(T("Choose folder"), parent, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                                          T("Choose"), T("Cancel"));
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

bool checkActive(GtkWidget *b)
{
#if DT_GTK4
    return gtk_check_button_get_active(GTK_CHECK_BUTTON(b));
#else
    return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(b));
#endif
}

void present(GtkWidget *w)
{
#if DT_GTK4
    gtk_window_present(GTK_WINDOW(w));
#else
    gtk_widget_show_all(w);
#endif
}

void addCssClass(GtkWidget *w, const char *cls)
{
#if DT_GTK4
    gtk_widget_add_css_class(w, cls);
#else
    gtk_style_context_add_class(gtk_widget_get_style_context(w), cls);
#endif
}

// Kipli bir alt pencere ve içindeki dikey kutu.
GtkWidget *dialogWindow(GtkWindow *parent, const std::string &title, int w, int h, GtkWidget **box)
{
    GtkWidget *win = gtk_window_new(
#if !DT_GTK4
        GTK_WINDOW_TOPLEVEL
#endif
    );
    gtk_window_set_title(GTK_WINDOW(win), ("TuxDirStat — " + title).c_str());
    gtk_window_set_transient_for(GTK_WINDOW(win), parent);
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(win), w, h);
    *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(*box, 10);
    gtk_widget_set_margin_end(*box, 10);
    gtk_widget_set_margin_top(*box, 10);
    gtk_widget_set_margin_bottom(*box, 10);
    windowSetChild(win, *box);
    return win;
}

// Dosya açma/kaydetme penceresi; seçilince cb(yol) çağrılır.
void pickFile(GtkWindow *parent, const std::string &title, bool save, const std::string &suggested,
              const char *filterName, std::initializer_list<const char *> patterns,
              std::function<void(const std::string &)> cb)
{
    auto *fn = new std::function<void(const std::string &)>(std::move(cb));
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, filterName);
    for (const char *pat : patterns)
        gtk_file_filter_add_pattern(filter, pat);
    GtkFileFilter *all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, T("All files"));
    gtk_file_filter_add_pattern(all, "*");
#if GTK_CHECK_VERSION(4, 10, 0)
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, title.c_str());
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_filters(d, G_LIST_MODEL(filters));
    g_object_unref(filters);
    g_object_unref(filter);
    g_object_unref(all);
    GFile *home = g_file_new_for_path(homeDir().c_str());
    gtk_file_dialog_set_initial_folder(d, home);
    g_object_unref(home);
    auto done = [](GObject *src, GAsyncResult *res, gpointer data, bool isSave) {
        auto *f = static_cast<std::function<void(const std::string &)> *>(data);
        GFile *file = isSave ? gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, nullptr)
                             : gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, nullptr);
        if (file) {
            if (char *p = g_file_get_path(file)) {
                (*f)(p);
                g_free(p);
            }
            g_object_unref(file);
        }
        delete f;
    };
    static decltype(done) doneFn = done;
    if (save) {
        gtk_file_dialog_set_initial_name(d, suggested.c_str());
        gtk_file_dialog_save(d, parent, nullptr, [](GObject *o, GAsyncResult *r, gpointer p) { doneFn(o, r, p, true); }, fn);
    } else {
        gtk_file_dialog_open(d, parent, nullptr, [](GObject *o, GAsyncResult *r, gpointer p) { doneFn(o, r, p, false); }, fn);
    }
    g_object_unref(d);
#else
    GtkFileChooserNative *d = gtk_file_chooser_native_new(title.c_str(), parent,
                                                          save ? GTK_FILE_CHOOSER_ACTION_SAVE : GTK_FILE_CHOOSER_ACTION_OPEN,
                                                          save ? T("Save") : T("Open"), T("Cancel"));
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(d), filter);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(d), all);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(d), homeDir().c_str());
    if (save) {
        gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(d), TRUE);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(d), suggested.c_str());
    }
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

const char *const SNAPSHOT_PATTERNS_NAME = N_("Snapshots (JSON)");

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
    void startPrivilegedScan(const std::string &path);
    void openSnapshot(const std::string &file);
    void beginBusy(const std::string &title);
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
    bool removeNow(Node *n, bool permanent, std::string *err);  // onay sormadan
    void finishDelete(Node *n);

    // araçlar
    bool colorByAge();
    bool toolReady();
    void showTable(ResultTable t);
    void runTool(const char *title, std::function<void(Job &)> fn);
    void findFiles();
    void openSnapshotDialog();
    void saveSnapshotDialog();
    void exportCsvDialog();
    void compareDialog();
    std::string lastQuery_;
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
        showError(GTK_WINDOW(app(p)->win_), "TuxDirStat " TUXDIRSTAT_VERSION,
                  std::string(T("Disk usage analyzer for Linux.")) + "\n\n" +
                      T("Click an item in the tree or the treemap. Double-click in the treemap to zoom into a "
                        "folder, Backspace goes back. Selecting a row in the extension list marks those files "
                        "in the treemap.") +
                      "\n\n" + T("License: GPL-3.0-or-later") + "\nhttps://github.com/Gubir34/TuxDirStat");
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
    static void age(GSimpleAction *a, GVariant *v, gpointer p)
    {
        g_simple_action_set_state(a, v);
        app(p)->tm_.forgetNodes();
        app(p)->scheduleRender(0);
        app(p)->updateZoomLabel();
        app(p)->saveSettings();
    }
    static void language(GSimpleAction *a, GVariant *v, gpointer p)
    {
        g_simple_action_set_state(a, v);
        app(p)->saveSettings();
        if (!app(p)->applyingSettings_)
            showError(GTK_WINDOW(app(p)->win_), "TuxDirStat", T("The new language is used after TuxDirStat is restarted."));
    }
    static void openSnap(GSimpleAction *, GVariant *, gpointer p) { app(p)->openSnapshotDialog(); }
    static void saveSnap(GSimpleAction *, GVariant *, gpointer p) { app(p)->saveSnapshotDialog(); }
    static void csv(GSimpleAction *, GVariant *, gpointer p) { app(p)->exportCsvDialog(); }
    static void find(GSimpleAction *, GVariant *, gpointer p) { app(p)->findFiles(); }
    static void compare(GSimpleAction *, GVariant *, gpointer p) { app(p)->compareDialog(); }
    static void duplicates(GSimpleAction *, GVariant *, gpointer p)
    {
        Node *root = app(p)->s_.root();
        app(p)->runTool(T("Duplicate files"), [root](Job &j) { j.result = findDuplicates(root, j); });
    }
    static void cleanup(GSimpleAction *, GVariant *, gpointer p)
    {
        Node *root = app(p)->s_.root();
        const bool ap = app(p)->s_.apparent;
        app(p)->runTool(T("Cleanup suggestions"), [root, ap](Job &j) { j.result = findCleanup(root, ap, j); });
    }
    static void owners(GSimpleAction *, GVariant *, gpointer p)
    {
        if (app(p)->toolReady())
            app(p)->showTable(ownerUsage(app(p)->s_.root(), app(p)->s_.apparent));
    }
    static void unowned(GSimpleAction *, GVariant *, gpointer p)
    {
        Node *root = app(p)->s_.root();
        const bool ap = app(p)->s_.apparent;
        app(p)->runTool(T("Files no package owns"), [root, ap](Job &j) { j.result = findUnowned(root, ap, j); });
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
    static void nPackage(GSimpleAction *, GVariant *, gpointer p)
    {
        if (Node *n = mn(p))
            showError(GTK_WINDOW(app(p)->win_), "TuxDirStat", packageOwnerMessage(n->path()));
    }
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
        {"age", nullptr, nullptr, "false", ActionTable::age},
        {"language", nullptr, "s", "'system'", ActionTable::language},
        {"open-snapshot", ActionTable::openSnap, nullptr, nullptr, nullptr},
        {"save-snapshot", ActionTable::saveSnap, nullptr, nullptr, nullptr},
        {"export-csv", ActionTable::csv, nullptr, nullptr, nullptr},
        {"find", ActionTable::find, nullptr, nullptr, nullptr},
        {"duplicates", ActionTable::duplicates, nullptr, nullptr, nullptr},
        {"cleanup", ActionTable::cleanup, nullptr, nullptr, nullptr},
        {"owners", ActionTable::owners, nullptr, nullptr, nullptr},
        {"unowned", ActionTable::unowned, nullptr, nullptr, nullptr},
        {"compare", ActionTable::compare, nullptr, nullptr, nullptr},
        {"node-package", ActionTable::nPackage, nullptr, nullptr, nullptr},
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
        {"win.age", {"F8", nullptr}},              {"win.find", {"<Control>f", nullptr}},
        {"win.save-snapshot", {"<Control>s", nullptr}},
    };
    for (auto &a : accels)
        gtk_application_set_accels_for_action(gapp_, a.action, a.keys);
    setActionEnabled("stop", false);
    setActionEnabled("zoom-out", false);
    setActionEnabled("unowned", havePacman());
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
    set("age", g_variant_new_boolean(st.colorByAge));
    set("language", g_variant_new_string(st.language.c_str()));
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
    st.colorByAge = colorByAge();
    v = g_action_get_state(g_action_map_lookup_action(G_ACTION_MAP(win_), "language"));
    st.language = g_variant_get_string(v, nullptr);
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
    gtk_window_set_icon_name(GTK_WINDOW(win_), "tuxdirstat");
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
    if (!path.empty() && g_file_test(path.c_str(), G_FILE_TEST_IS_REGULAR))
        openSnapshot(path);
    else if (!path.empty())
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
    boxAdd(bar, toolButton("document-open", T("Open…"), "win.open", false));
    boxAdd(bar, toolButton("view-refresh", T("Rescan"), "win.rescan", false));
    boxAdd(bar, toolButton("process-stop", T("Stop"), "win.stop", false));
    sep();
    boxAdd(bar, toolButton("zoom-in", T("Zoom in"), "win.zoom-in", false));
    boxAdd(bar, toolButton("zoom-out", T("Zoom out"), "win.zoom-out", false));
    boxAdd(bar, toolButton("zoom-original", T("Root"), "win.zoom-root", false));
    sep();
    boxAdd(bar, toolButton("view-statistics", T("Apparent size"), "win.apparent", true,
                           T("On: the apparent size of files (ls)\nOff: the space they take on disk (du)")));
    boxAdd(bar, toolButton("drive-harddisk", T("One file system"), "win.onefs", true,
                           T("Do not cross into other disks or mount points\n(btrfs subvolumes on the same device are included)")));
    boxAdd(bar, toolButton("view-group", T("Group files"), "win.group", true,
                           T("Collect many similar items in a folder (like game assets) into one box:\n"
                             "files with the same extension, small files and small subfolders")));
    boxAdd(bar, toolButton("view-split-top-bottom", T("Treemap"), "win.treemap", true));
    boxAdd(bar, toolButton("chronometer", T("Color by age"), "win.age", true,
                           T("Color the treemap by when files last changed instead of by type")));
    sep();
    boxAdd(bar, toolButton("edit-find", T("Find…"), "win.find", false, T("Find files…")));
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    boxAdd(bar, spacer, true);
    GMenu *menu = g_menu_new();
    GMenu *file = g_menu_new();
    g_menu_append(file, T("Open snapshot…"), "win.open-snapshot");
    g_menu_append(file, T("Save snapshot…"), "win.save-snapshot");
    g_menu_append(file, T("Export as CSV…"), "win.export-csv");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(file));
    g_object_unref(file);
    GMenu *tools = g_menu_new();
    g_menu_append(tools, T("Find files…"), "win.find");
    g_menu_append(tools, T("Duplicate files…"), "win.duplicates");
    g_menu_append(tools, T("Cleanup suggestions…"), "win.cleanup");
    g_menu_append(tools, T("Usage by owner"), "win.owners");
    g_menu_append(tools, T("Files no package owns…"), "win.unowned");
    g_menu_append(tools, T("Compare with snapshot…"), "win.compare");
    g_menu_append_section(menu, T("Tools"), G_MENU_MODEL(tools));
    g_object_unref(tools);
    GMenu *thr = g_menu_new();
    for (uint32_t n : GROUP_MIN_CHOICES) {
        const std::string label = TF("At least %1 items", {std::to_string(n)});
        GMenuItem *it = g_menu_item_new(label.c_str(), nullptr);
        g_menu_item_set_action_and_target_value(it, "win.group-min", g_variant_new_uint32(n));
        g_menu_append_item(thr, it);
        g_object_unref(it);
    }
    g_menu_append_section(menu, T("Grouping threshold"), G_MENU_MODEL(thr));
    GMenu *themes = g_menu_new();
    for (auto [id, label] : {std::pair{"system", N_("System")}, {"light", N_("Light")}, {"dark", N_("Dark")}}) {
        GMenuItem *it = g_menu_item_new(T(label), nullptr);
        g_menu_item_set_action_and_target_value(it, "win.theme", g_variant_new_string(id));
        g_menu_append_item(themes, it);
        g_object_unref(it);
    }
    g_menu_append_section(menu, T("Theme"), G_MENU_MODEL(themes));
    g_object_unref(themes);
    GMenu *langs = g_menu_new();
    for (auto [id, label] : {std::pair{"system", T("System")}, {"en", "English"}, {"tr", "Türkçe"}}) {
        GMenuItem *it = g_menu_item_new(label, nullptr);
        g_menu_item_set_action_and_target_value(it, "win.language", g_variant_new_string(id));
        g_menu_append_item(langs, it);
        g_object_unref(it);
    }
    g_menu_append_section(menu, T("Language"), G_MENU_MODEL(langs));
    g_object_unref(langs);
    GMenu *other = g_menu_new();
    g_menu_append(other, T("About"), "win.about");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(other));
    GtkWidget *mb = gtk_menu_button_new();
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(mb), G_MENU_MODEL(menu));
#if DT_GTK4
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(mb), "open-menu-symbolic");
#else
    gtk_button_set_image(GTK_BUTTON(mb), gtk_image_new_from_icon_name("open-menu-symbolic", GTK_ICON_SIZE_SMALL_TOOLBAR));
#endif
    gtk_widget_set_tooltip_text(mb, T("Menu"));
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
        gtk_tree_view_column_set_title(col, T(COL_TITLES[c]));
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
    gtk_tree_view_column_set_title(col, T("Extension"));
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
    } spec[] = {{T("Type"), E_DESC, E_DESC, false, true},
                {T("Size"), E_SIZE, E_BYTES, true, false},
                {"%", E_PCT, E_BYTES, true, false},
                {T("Files"), E_COUNT, E_NUM, true, false}};
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
    gtk_window_set_title(GTK_WINDOW(dlg), (std::string("TuxDirStat — ") + T("Location to scan")).c_str());
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(win_));
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 720, 400);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 10);
    gtk_widget_set_margin_end(box, 10);
    gtk_widget_set_margin_top(box, 10);
    gtk_widget_set_margin_bottom(box, 10);
    GtkWidget *lbl = gtk_label_new(T("Choose what to scan:"));
    gtk_label_set_xalign(GTK_LABEL(lbl), 0);
    boxAdd(box, lbl);

    enum { D_ICON, D_LABEL, D_FS, D_USED, D_TOTAL, D_PCT, D_PATH, D_N };
    GtkListStore *st = gtk_list_store_new(D_N, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                          G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING);
    const std::string home = homeDir();
    gtk_list_store_insert_with_values(st, nullptr, -1, D_ICON, "user-home", D_LABEL,
                                      TF("Home folder  (%1)", {home}).c_str(), D_PCT, -1, D_PATH, home.c_str(), -1);
    for (const Drive &d : listDrives())
        gtk_list_store_insert_with_values(st, nullptr, -1, D_ICON, "drive-harddisk", D_LABEL, d.mp.c_str(), D_FS,
                                          d.fs.c_str(), D_USED, fmtSize(d.used).c_str(), D_TOTAL,
                                          fmtSize(d.total).c_str(), D_PCT, int(d.used * 100 / d.total), D_PATH,
                                          d.mp.c_str(), -1);
    GtkWidget *list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(st));
    g_object_unref(st);
    GtkTreeViewColumn *c0 = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(c0, T("Location"));
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
    } cs[] = {{T("Type"), D_FS}, {T("Used"), D_USED}, {T("Total"), D_TOTAL}};
    for (auto &c : cs) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        if (c.col != D_FS)
            g_object_set(r, "xalign", 1.0, nullptr);
        gtk_tree_view_append_column(GTK_TREE_VIEW(list), gtk_tree_view_column_new_with_attributes(c.t, r, "text", c.col, nullptr));
    }
    GtkCellRenderer *pr = gtk_cell_renderer_pixbuf_new();
    g_object_set(pr, "xalign", 0.0, nullptr);
    GtkTreeViewColumn *pc = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(pc, T("Usage"));
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
    GtkWidget *admin = gtk_check_button_new_with_label(T("Scan as administrator (read-only, asks for your password)"));
    gtk_widget_set_tooltip_text(admin, T("Reads folders your user cannot open, such as /root or /var/lib. Deleting still uses your own rights."));
    boxAdd(box, admin);

    struct Ctx {
        App *app;
        GtkWidget *dlg, *list, *admin;
    };
    auto *ctx = new Ctx{this, dlg, list, admin};
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
        const bool asAdmin = checkActive(c->admin);
        destroyWindow(c->dlg);
        if (asAdmin)
            app->startPrivilegedScan(p);
        else
            app->startScan(p);
    };
    static decltype(accept) acceptFn = accept;
    g_signal_connect(list, "row-activated", DT_CB(+[](GtkTreeView *, GtkTreePath *, GtkTreeViewColumn *, gpointer c) {
                         acceptFn(static_cast<Ctx *>(c));
                     }),
                     ctx);

    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *pick = gtk_button_new_with_label(T("Choose folder…"));
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *cancel = gtk_button_new_with_label(T("Cancel"));
    GtkWidget *ok = gtk_button_new_with_label(T("Scan"));
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
                         GtkWidget *dlg = c->dlg, *admin = c->admin;
                         pickFolder(GTK_WINDOW(dlg), [app, dlg, admin](const std::string &p) {
                             const bool asAdmin = checkActive(admin);
                             destroyWindow(dlg);
                             if (asAdmin)
                                 app->startPrivilegedScan(p);
                             else
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
    beginBusy(target ? std::string() : abs);
}

void App::startPrivilegedScan(const std::string &path)
{
    char *real = realpath(path.c_str(), nullptr);
    const std::string abs = real ? real : path;
    free(real);
    s_.startPrivilegedScan(abs);
    beginBusy(abs);
}

void App::openSnapshot(const std::string &file)
{
    char *real = realpath(file.c_str(), nullptr);
    const std::string abs = real ? real : file;
    free(real);
    s_.openSnapshot(abs);
    beginBusy(abs.substr(abs.rfind('/') + 1));
}

void App::beginBusy(const std::string &title)
{
    setActionEnabled("stop", true);
    gtk_spinner_start(GTK_SPINNER(spinner_));
    if (!title.empty())
        gtk_window_set_title(GTK_WINDOW(win_), ("TuxDirStat — " + title).c_str());
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
        gtk_label_set_text(GTK_LABEL(statusL_), s_.progressText().c_str());
        return true;
    }
    gtk_spinner_stop(GTK_SPINNER(spinner_));
    setActionEnabled("stop", false);
    switch (res) {
    case Session::Result::Failed:
        gtk_label_set_text(GTK_LABEL(statusL_), "");
        showError(GTK_WINDOW(win_), T("The scan failed:"), s_.error());
        if (s_.root())
            gtk_window_set_title(GTK_WINDOW(win_), ("TuxDirStat — " + s_.root()->path()).c_str());
        return false;
    case Session::Result::Cancelled:
        gtk_label_set_text(GTK_LABEL(statusL_), T("Scan stopped."));
        return false;
    case Session::Result::NewTree: {
        Node *root = s_.pendingRoot();
        tm_.forgetNodes();
        tmRoot_ = root;
        selected_ = hover_ = menuNode_ = nullptr;
        setModelRoot(root);
        s_.commitNewTree();
        gtk_window_set_title(GTK_WINDOW(win_), ("TuxDirStat — " + root->path()).c_str());
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
    const std::string done = TF("Done: %1 items in %2 s.", {fmtInt(s_.lastCount()), fmtDecimal(s_.elapsed(), 2)});
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
    const std::string txt = TF("Total: %1 · %2 files · %3 folders · Free: %4",
                               {fmtSize(root->sizeOf(s_.apparent)), fmtInt(root->files), fmtInt(root->dirs),
                                fmtSize(freeSpace(root->path()))});
    gtk_label_set_text(GTK_LABEL(statusR_), txt.c_str());
}

void App::updateZoomLabel()
{
    std::string m = markupEscape(T("Treemap"));
    if (tmRoot_)
        m += ": <b>" + markupEscape(tmRoot_->path()) + "</b>";
    if (colorByAge()) {
        m += "      " + markupEscape(T("Last change:"));
        for (const AgeBucket &b : ageBuckets()) {
            char hex[16];
            auto ch = [](float v) { return int(std::min(255.f, v * 1.4f)); };
            std::snprintf(hex, sizeof hex, "#%02x%02x%02x", ch(b.color.r), ch(b.color.g), ch(b.color.b));
            m += std::string("  <span foreground=\"") + hex + "\">■</span> " + markupEscape(T(b.label));
        }
    }
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
    tm_.setColorMode(colorByAge() ? ColorMode::Age : ColorMode::Extension);
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
    g_menu_append(s1, T("Open"), "win.node-open");
    g_menu_append(s1, T("Show in file manager"), "win.node-reveal");
    g_menu_append(s1, T("Copy path"), "win.node-copy");
    if (havePacman() && !(n->flags & F_GROUP))
        g_menu_append(s1, T("Which package owns this?"), "win.node-package");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(s1));
    if (n->isDir) {
        GMenu *s2 = g_menu_new();
        g_menu_append(s2, T("Zoom into in treemap"), "win.node-zoom");
        g_menu_append(s2,
                      (n->flags & F_GROUP)   ? T("Rescan its folder")
                      : (n->flags & F_MOUNT) ? T("Scan this file system too")
                                             : T("Rescan this folder"),
                      "win.node-refresh");
        g_menu_append_section(menu, nullptr, G_MENU_MODEL(s2));
        g_object_unref(s2);
    }
    if (n->parent) {
        GMenu *s3 = g_menu_new();
        g_menu_append(s3, T("Move to trash"), "win.node-trash");
        g_menu_append(s3, T("Delete permanently…"), "win.node-delete");
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
    const std::string kind = (n->flags & F_GROUP) ? TF("the %1 files in this group", {fmtInt(n->files)})
                             : n->isDir           ? T("this folder")
                                                  : T("this file");
    const std::string size = fmtSize(n->sizeOf(s_.apparent));
    const std::string title = permanent ? TF("Delete %1 permanently?", {kind}) : TF("Move %1 to the trash?", {kind});
    const std::string detail = path + "\n(" + size + ")" + (permanent ? std::string("\n\n") + T("This cannot be undone.") : "");
    confirm(GTK_WINDOW(win_), title, detail, permanent ? T("Delete permanently") : T("Move to trash"),
            [this, n, permanent](bool yes) {
                if (!yes || s_.scanning() || !s_.root() || !n->isWithin(s_.root()))
                    return;
                std::string err;
                if (!removeNow(n, permanent, &err))
                    showError(GTK_WINDOW(win_), T("Could not delete (or only partly deleted):"), err);
            });
}

bool App::removeNow(Node *n, bool permanent, std::string *errOut)
{
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
        if (errOut)
            *errOut = firstFailed + "\n\n" + err;
        refreshNode(n->parent);  // ağacı diskle eşitle
        return false;
    }
    finishDelete(n);
    return true;
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

// ───────────── araçlar ─────────────

bool App::colorByAge()
{
    GAction *a = g_action_map_lookup_action(G_ACTION_MAP(win_), "age");
    if (!a)
        return false;
    GVariant *v = g_action_get_state(a);
    const bool on = g_variant_get_boolean(v);
    g_variant_unref(v);
    return on;
}

bool App::toolReady()
{
    if (s_.scanning()) {
        showError(GTK_WINDOW(win_), "TuxDirStat", T("Wait until the scan has finished."));
        return false;
    }
    if (!s_.root()) {
        showError(GTK_WINDOW(win_), "TuxDirStat", T("Scan a folder first."));
        return false;
    }
    return true;
}

void App::showTable(ResultTable table)
{
    struct Ctx {
        App *app;
        ResultTable t;
        std::vector<bool> gone;
        GtkWidget *win = nullptr, *view = nullptr;
        GtkTreeStore *store = nullptr;
        GtkWidget *bShow = nullptr, *bReveal = nullptr, *bCopy = nullptr, *bTrash = nullptr;
        int ncols = 0;
        int colRow() const { return ncols; }
        int colBytes() const { return ncols + 1; }
        int colWeight() const { return ncols + 2; }
        int colTip() const { return ncols + 3; }

        std::vector<int> selected() const
        {
            std::vector<int> v;
            if (!view)
                return v;
            GtkTreeModel *m;
            GList *paths = gtk_tree_selection_get_selected_rows(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), &m);
            for (GList *l = paths; l; l = l->next) {
                GtkTreeIter it;
                if (gtk_tree_model_get_iter(m, &it, static_cast<GtkTreePath *>(l->data))) {
                    int r = -1;
                    gtk_tree_model_get(m, &it, colRow(), &r, -1);
                    if (r >= 0 && !gone[size_t(r)])
                        v.push_back(r);
                }
            }
            g_list_free_full(paths, reinterpret_cast<GDestroyNotify>(gtk_tree_path_free));
            return v;
        }
        void update()
        {
            if (!view)
                return;
            const std::vector<int> rows = selected();
            const ResultRow *one = rows.size() == 1 ? &t.rows[size_t(rows[0])] : nullptr;
            Node *root = app->s_.root();
            gtk_widget_set_sensitive(bShow, one && one->node && root && one->node->isWithin(root));
            gtk_widget_set_sensitive(bReveal, one && !one->path.empty() && one->path[0] == '/');
            gtk_widget_set_sensitive(bCopy, !rows.empty());
            gtk_button_set_label(GTK_BUTTON(bCopy), one && !one->command.empty() ? T("Copy command") : T("Copy path"));
            bool del = !rows.empty();
            for (int r : rows)
                del = del && t.rows[size_t(r)].deletable && !t.rows[size_t(r)].path.empty();
            gtk_widget_set_sensitive(bTrash, del);
        }
        void showInTree()
        {
            const std::vector<int> rows = selected();
            if (rows.size() != 1)
                return;
            Node *n = t.rows[size_t(rows[0])].node;
            Node *root = app->s_.root();
            if (!n || !root || !n->isWithin(root))
                return;
            App *a = app;
            destroyWindow(win);  // Ctx burada yok edilir
            a->selectNode(n);
        }
        void prune()
        {
            GtkTreeModel *m = GTK_TREE_MODEL(store);
            GtkTreeIter it;
            bool valid = gtk_tree_model_get_iter_first(m, &it);
            while (valid) {
                int r = -1;
                gtk_tree_model_get(m, &it, colRow(), &r, -1);
                if (r < 0) {  // kopya kümesi
                    GtkTreeIter c;
                    bool cv = gtk_tree_model_iter_children(m, &c, &it);
                    while (cv) {
                        int cr = -1;
                        gtk_tree_model_get(m, &c, colRow(), &cr, -1);
                        cv = cr >= 0 && gone[size_t(cr)] ? gtk_tree_store_remove(store, &c) : gtk_tree_model_iter_next(m, &c);
                    }
                    valid = gtk_tree_model_iter_n_children(m, &it) < 2 ? gtk_tree_store_remove(store, &it)
                                                                        : gtk_tree_model_iter_next(m, &it);
                } else {
                    valid = gone[size_t(r)] ? gtk_tree_store_remove(store, &it) : gtk_tree_model_iter_next(m, &it);
                }
            }
        }
    };
    auto *c = new Ctx{this, std::move(table), {}};
    c->gone.assign(c->t.rows.size(), false);
    GtkWidget *box;
    c->win = dialogWindow(GTK_WINDOW(win_), c->t.title, 980, 560, &box);
    g_object_set_data_full(G_OBJECT(c->win), "dt-ctx", c, [](gpointer p) { delete static_cast<Ctx *>(p); });
    GtkWidget *title = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(title), ("<big><b>" + markupEscape(c->t.title) + "</b></big>").c_str());
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    boxAdd(box, title);
    if (!c->t.summary.empty()) {
        GtkWidget *sum = gtk_label_new(c->t.summary.c_str());
        gtk_label_set_xalign(GTK_LABEL(sum), 0);
#if DT_GTK4
        gtk_label_set_wrap(GTK_LABEL(sum), TRUE);
#else
        gtk_label_set_line_wrap(GTK_LABEL(sum), TRUE);
#endif
        addCssClass(sum, "dim-label");
        boxAdd(box, sum);
    }
    if (c->t.rows.empty()) {
        GtkWidget *empty = gtk_label_new(c->t.emptyText.c_str());
        addCssClass(empty, "dim-label");
        boxAdd(box, empty, true);
    } else {
        const ResultTable &t = c->t;
        c->ncols = int(t.columns.size());
        std::vector<GType> types(size_t(c->ncols), G_TYPE_STRING);
        types.push_back(G_TYPE_INT);
        types.push_back(G_TYPE_UINT64);
        types.push_back(G_TYPE_INT);
        types.push_back(G_TYPE_STRING);
        c->store = gtk_tree_store_newv(int(types.size()), types.data());
        std::unordered_map<int, GtkTreeIter> groups;
        for (size_t i = 0; i < t.rows.size(); ++i) {
            const ResultRow &r = t.rows[i];
            GtkTreeIter it, *parent = nullptr;
            if (t.grouped) {
                auto g = groups.find(r.group);
                if (g == groups.end()) {
                    int n = 0;
                    for (const ResultRow &o : t.rows)
                        n += o.group == r.group;
                    GtkTreeIter gi;
                    gtk_tree_store_insert_with_values(
                        c->store, &gi, nullptr, -1, 0,
                        TF("Set %1 · %2 × %3", {std::to_string(r.group), std::to_string(n), fmtSize(r.bytes)}).c_str(),
                        c->colRow(), -1, c->colBytes(), guint64(r.bytes), c->colWeight(), 700, -1);
                    g = groups.emplace(r.group, gi).first;
                }
                parent = &g->second;
            }
            gtk_tree_store_insert_with_values(c->store, &it, parent, -1, c->colRow(), int(i), c->colBytes(), guint64(r.bytes),
                                              c->colWeight(), 400, c->colTip(),
                                              (r.command.empty() ? r.path : r.command).c_str(), -1);
            for (size_t col = 0; col < r.cells.size() && int(col) < c->ncols; ++col)
                gtk_tree_store_set(c->store, &it, int(col), r.cells[col].c_str(), -1);
        }
        c->view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(c->store));
        g_object_unref(c->store);  // görünüm sahiplenir
        if (!t.grouped) {
            gtk_tree_view_set_show_expanders(GTK_TREE_VIEW(c->view), FALSE);
            gtk_tree_view_set_level_indentation(GTK_TREE_VIEW(c->view), 0);
        }
        // En uzun metinli (yol) sütun esner ve ortadan kısaltılır; diğerleri içeriğe göre.
        int stretch = 0;
        size_t best = 0;
        for (int col = 0; col < c->ncols; ++col) {
            if (size_t(col) < t.numeric.size() && t.numeric[size_t(col)])
                continue;
            size_t len = 0;
            for (const ResultRow &r : t.rows)
                len += size_t(col) < r.cells.size() ? r.cells[size_t(col)].size() : 0;
            if (len > best) {
                best = len;
                stretch = col;
            }
        }
        for (int col = 0; col < c->ncols; ++col) {
            const bool num = size_t(col) < t.numeric.size() && t.numeric[size_t(col)];
            GtkCellRenderer *r = gtk_cell_renderer_text_new();
            if (num)
                g_object_set(r, "xalign", 1.0, nullptr);
            else if (col == stretch)
                g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, nullptr);
            GtkTreeViewColumn *tc = gtk_tree_view_column_new_with_attributes(t.columns[size_t(col)].c_str(), r, "text", col,
                                                                             "weight", c->colWeight(), nullptr);
            gtk_tree_view_column_set_resizable(tc, TRUE);
            if (num)
                gtk_tree_view_column_set_alignment(tc, 1.0);
            if (col == stretch) {
                gtk_tree_view_column_set_expand(tc, TRUE);
                gtk_tree_view_column_set_min_width(tc, 200);
            }
            if (!t.grouped)
                gtk_tree_view_column_set_sort_column_id(tc, num ? c->colBytes() : col);
            gtk_tree_view_append_column(GTK_TREE_VIEW(c->view), tc);
        }
        gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(c->view), c->colTip());
        gtk_tree_view_expand_all(GTK_TREE_VIEW(c->view));
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(c->view));
        gtk_tree_selection_set_mode(sel, GTK_SELECTION_MULTIPLE);
        g_signal_connect(sel, "changed", DT_CB(+[](GtkTreeSelection *, gpointer p) { static_cast<Ctx *>(p)->update(); }), c);
        g_signal_connect(c->view, "row-activated", DT_CB(+[](GtkTreeView *, GtkTreePath *, GtkTreeViewColumn *, gpointer p) {
                             static_cast<Ctx *>(p)->showInTree();
                         }),
                         c);
        GtkWidget *sw = scrolled(c->view);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        boxAdd(box, sw, true);
    }
    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    if (c->view) {
        c->bShow = gtk_button_new_with_label(T("Show in tree"));
        c->bReveal = gtk_button_new_with_label(T("Open folder"));
        c->bCopy = gtk_button_new_with_label(T("Copy path"));
        c->bTrash = gtk_button_new_with_label(T("Move to trash"));
        for (GtkWidget *b : {c->bShow, c->bReveal, c->bCopy, c->bTrash})
            boxAdd(btns, b);
        g_signal_connect(c->bShow, "clicked", DT_CB(+[](GtkButton *, gpointer p) { static_cast<Ctx *>(p)->showInTree(); }), c);
        g_signal_connect(c->bReveal, "clicked", DT_CB(+[](GtkButton *, gpointer p) {
                             Ctx *c = static_cast<Ctx *>(p);
                             const std::vector<int> rows = c->selected();
                             if (rows.size() == 1)
                                 revealPath(c->t.rows[size_t(rows[0])].path);
                         }),
                         c);
        g_signal_connect(c->bCopy, "clicked", DT_CB(+[](GtkButton *, gpointer p) {
                             Ctx *c = static_cast<Ctx *>(p);
                             std::string text;
                             for (int r : c->selected()) {
                                 const ResultRow &row = c->t.rows[size_t(r)];
                                 text += (text.empty() ? "" : "\n") + (row.command.empty() ? row.path : row.command);
                             }
                             setClipboard(c->win, text);
                         }),
                         c);
        g_signal_connect(c->bTrash, "clicked", DT_CB(+[](GtkButton *, gpointer p) {
                             Ctx *c = static_cast<Ctx *>(p);
                             const std::vector<int> rows = c->selected();
                             if (rows.empty())
                                 return;
                             uint64_t bytes = 0;
                             for (int r : rows)
                                 bytes += c->t.rows[size_t(r)].bytes;
                             const std::string warn = deletionWarning(c->t, rows);
                             const std::string title = TF("Move %1 items (%2) to the trash?", {fmtInt(rows.size()), fmtSize(bytes)});
                             confirm(GTK_WINDOW(c->win), title, warn, T("Move to trash"), [c, rows](bool yes) {
                                 if (!yes)
                                     return;
                                 int failed = 0;
                                 std::string firstErr;
                                 for (int r : rows) {
                                     const ResultRow &row = c->t.rows[size_t(r)];
                                     Node *root = c->app->s_.root();
                                     std::string err;
                                     bool ok;
                                     if (!g_file_test(row.path.c_str(), G_FILE_TEST_EXISTS) &&
                                         !g_file_test(row.path.c_str(), G_FILE_TEST_IS_SYMLINK)) {
                                         ok = true;  // daha önce silinen bir klasörün içindeydi
                                     } else if (row.node && root && row.node->isWithin(root)) {
                                         ok = c->app->removeNow(row.node, false, &err);
                                     } else {
                                         GFile *f = g_file_new_for_path(row.path.c_str());
                                         ok = g_file_trash(f, nullptr, nullptr) || trashWithCommand(row.path);
                                         g_object_unref(f);
                                         err = row.path;
                                     }
                                     if (ok)
                                         c->gone[size_t(r)] = true;
                                     else if (!failed++)
                                         firstErr = err;
                                 }
                                 c->prune();
                                 c->update();
                                 if (failed)
                                     showError(GTK_WINDOW(c->win),
                                               TF("%1 items could not be moved to the trash.", {std::to_string(failed)}), firstErr);
                             });
                         }),
                         c);
        c->update();
    }
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    boxAdd(btns, spacer, true);
    GtkWidget *close = gtk_button_new_with_label(T("Close"));
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(destroyWindow), c->win);
    boxAdd(btns, close);
    boxAdd(box, btns);
    present(c->win);
}

void App::runTool(const char *title, std::function<void(Job &)> fn)
{
    if (!toolReady())
        return;
    struct P {
        App *app;
        std::unique_ptr<Job> job;
        GtkWidget *win, *label, *bar;
    };
    GtkWidget *box;
    auto *p = new P{this, std::make_unique<Job>(title, std::move(fn)), nullptr, nullptr, nullptr};
    p->win = dialogWindow(GTK_WINDOW(win_), title, 520, -1, &box);
    gtk_window_set_deletable(GTK_WINDOW(p->win), FALSE);
    p->label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(p->label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(p->label), PANGO_ELLIPSIZE_MIDDLE);
    p->bar = gtk_progress_bar_new();
    GtkWidget *cancel = gtk_button_new_with_label(T("Cancel"));
    gtk_widget_set_halign(cancel, GTK_ALIGN_END);
    g_signal_connect(cancel, "clicked", DT_CB(+[](GtkButton *b, gpointer pp) {
                         static_cast<P *>(pp)->job->cancel();
                         gtk_widget_set_sensitive(GTK_WIDGET(b), FALSE);
                     }),
                     p);
    boxAdd(box, p->label);
    boxAdd(box, p->bar);
    boxAdd(box, cancel);
    present(p->win);
    p->job->start();
    g_timeout_add(100, [](gpointer pp) -> gboolean {
        P *p = static_cast<P *>(pp);
        const double f = p->job->progress();
        if (f < 0)
            gtk_progress_bar_pulse(GTK_PROGRESS_BAR(p->bar));
        else
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(p->bar), f);
        const std::string st = p->job->status();
        if (!st.empty())
            gtk_label_set_text(GTK_LABEL(p->label), st.c_str());
        if (!p->job->done())
            return G_SOURCE_CONTINUE;
        destroyWindow(p->win);
        if (!p->job->cancelled()) {
            if (!p->job->error.empty() && p->job->result.rows.empty())
                showError(GTK_WINDOW(p->app->win_), "TuxDirStat", p->job->error);
            else
                p->app->showTable(std::move(p->job->result));
        }
        delete p;
        return G_SOURCE_REMOVE;
    }, p);
}

void App::findFiles()
{
    if (!toolReady())
        return;
    struct F {
        App *app;
        GtkWidget *win, *entry;
    };
    GtkWidget *box;
    auto *f = new F{this, nullptr, nullptr};
    f->win = dialogWindow(GTK_WINDOW(win_), T("Find files"), 560, -1, &box);
    g_object_set_data_full(G_OBJECT(f->win), "dt-ctx", f, [](gpointer p) { delete static_cast<F *>(p); });
    f->entry = gtk_entry_new();
#if DT_GTK4
    gtk_editable_set_text(GTK_EDITABLE(f->entry), lastQuery_.c_str());
#else
    gtk_entry_set_text(GTK_ENTRY(f->entry), lastQuery_.c_str());
#endif
    gtk_entry_set_placeholder_text(GTK_ENTRY(f->entry), "*.iso >1G older:1y");
    boxAdd(box, f->entry);
    GtkWidget *help = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(help), ("<tt>" + markupEscape(searchHelp()) + "</tt>").c_str());
    gtk_label_set_xalign(GTK_LABEL(help), 0);
    addCssClass(help, "dim-label");
    boxAdd(box, help);
    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *cancel = gtk_button_new_with_label(T("Cancel"));
    GtkWidget *ok = gtk_button_new_with_label(T("Find"));
    addCssClass(ok, "suggested-action");
    boxAdd(btns, spacer, true);
    boxAdd(btns, cancel);
    boxAdd(btns, ok);
    boxAdd(box, btns);
    auto run = +[](F *f) {
#if DT_GTK4
        const std::string q = gtk_editable_get_text(GTK_EDITABLE(f->entry));
#else
        const std::string q = gtk_entry_get_text(GTK_ENTRY(f->entry));
#endif
        App *a = f->app;
        a->lastQuery_ = q;
        std::string err;
        ResultTable t = searchTree(a->s_.root(), q, a->s_.apparent, &err);
        if (!err.empty()) {
            showError(GTK_WINDOW(f->win), "TuxDirStat", err);
            return;
        }
        t.title += ": " + q;
        destroyWindow(f->win);
        a->showTable(std::move(t));
    };
    static decltype(run) runFn = run;
    g_signal_connect(ok, "clicked", DT_CB(+[](GtkButton *, gpointer p) { runFn(static_cast<F *>(p)); }), f);
    g_signal_connect(f->entry, "activate", DT_CB(+[](GtkEntry *, gpointer p) { runFn(static_cast<F *>(p)); }), f);
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(destroyWindow), f->win);
    present(f->win);
    gtk_widget_grab_focus(f->entry);
}

void App::openSnapshotDialog()
{
    if (s_.scanning())
        return;
    pickFile(GTK_WINDOW(win_), T("Open snapshot"), false, "", T(SNAPSHOT_PATTERNS_NAME),
             {"*.json", "*.json.gz", "*.json.zst", "*.json.xz"}, [this](const std::string &f) { openSnapshot(f); });
}

void App::saveSnapshotDialog()
{
    if (!toolReady())
        return;
    std::string name = s_.root()->path();
    name = name.substr(name.rfind('/') + 1);
    if (name.empty())
        name = "root";
    GDateTime *now = g_date_time_new_now_local();
    gchar *day = g_date_time_format(now, "%Y-%m-%d");
    name += std::string("-") + day + ".json";
    g_free(day);
    g_date_time_unref(now);
    pickFile(GTK_WINDOW(win_), T("Save snapshot"), true, name, T(SNAPSHOT_PATTERNS_NAME), {"*.json"},
             [this](const std::string &file) {
                 if (!s_.root() || s_.scanning())
                     return;
                 std::string f = file, err;
                 if (f.find('.', f.rfind('/') + 1) == std::string::npos)
                     f += ".json";
                 if (!saveSnapshot(s_.root(), f, &err))
                     showError(GTK_WINDOW(win_), T("Could not save the snapshot:"), err);
                 else
                     gtk_label_set_text(GTK_LABEL(statusL_), TF("Snapshot saved: %1", {f}).c_str());
             });
}

void App::exportCsvDialog()
{
    if (!toolReady())
        return;
    pickFile(GTK_WINDOW(win_), T("Export as CSV"), true, "tuxdirstat.csv", T("CSV files"), {"*.csv"},
             [this](const std::string &f) {
                 if (!s_.root() || s_.scanning())
                     return;
                 std::string err;
                 if (!exportCsv(s_.root(), f, &err))
                     showError(GTK_WINDOW(win_), T("Could not write the file:"), err);
                 else
                     gtk_label_set_text(GTK_LABEL(statusL_), TF("Exported: %1", {f}).c_str());
             });
}

void App::compareDialog()
{
    if (!toolReady())
        return;
    pickFile(GTK_WINDOW(win_), T("Compare with snapshot"), false, "", T(SNAPSHOT_PATTERNS_NAME),
             {"*.json", "*.json.gz", "*.json.zst", "*.json.xz"}, [this](const std::string &file) {
                 Node *root = s_.root();
                 const bool ap = s_.apparent;
                 runTool(T("Compare with snapshot"), [root, ap, file](Job &j) { j.result = compareWithSnapshot(root, file, ap, j); });
             });
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
