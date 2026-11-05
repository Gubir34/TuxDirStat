// TuxDirStat — Tk arayüzü. Yerleşim gömülü Tcl betiğinde, veri işlemleri C++'ta (`dt` komutu).
// Kullanım: tuxdirstat-tk [klasör]

#include <tcl.h>
#include <tk.h>
#include <gio/gio.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "extinfo.h"
#include "report.h"
#include "session.h"
#include "treemap_render.h"

namespace {

const char *const UI_SCRIPT = R"TCL(
ttk::style theme use clam
ttk::style configure Treeview -rowheight 22
option add *tearOff 0
wm title . TuxDirStat
wm geometry . 1280x860
wm protocol . WM_DELETE_WINDOW {dt act quit}

# Küçük simgeler
image create photo ico_dir -width 16 -height 16
ico_dir put #b88a22 -to 1 3 8 6
ico_dir put #e8b84a -to 1 5 15 14
image create photo ico_file -width 16 -height 16
ico_file put #8a8a8a -to 3 1 13 15
ico_file put #fdfdfd -to 4 2 12 14
image create photo ico_link -width 16 -height 16
ico_link put #4a7ad8 -to 3 1 13 15
ico_link put #fdfdfd -to 4 2 12 14
image create photo ico_group -width 16 -height 16
ico_group put #6f5bc9 -to 2 2 10 10
ico_group put #8f7be8 -to 5 5 13 13
ico_group put #b3a5f5 -to 8 8 16 16
image create photo tmimg

set ::apparent 0
set ::onefs 1
set ::group 0
set ::groupmin 16
set ::theme system
set ::groupMinText "Eşik: 16"
set ::showtm 1
set ::statusL ""
set ::statusR ""
set ::zoomText "Treemap"

# Araç çubuğu
ttk::frame .tb -padding {4 3}
ttk::button .tb.open -text "Aç…" -command {dt act open}
ttk::button .tb.rescan -text "Yeniden tara" -command {dt act rescan}
ttk::button .tb.stop -text "Durdur" -command {dt act stop} -state disabled
ttk::separator .tb.s1 -orient vertical
ttk::button .tb.zin -text "Yakınlaştır" -command {dt act zoomin}
ttk::button .tb.zout -text "Uzaklaştır" -command {dt act zoomout} -state disabled
ttk::button .tb.zroot -text "Kök" -command {dt act zoomroot}
ttk::separator .tb.s2 -orient vertical
ttk::checkbutton .tb.app -text "Görünür boyut" -variable ::apparent -command {dt act apparent}
ttk::checkbutton .tb.onefs -text "Tek dosya sistemi" -variable ::onefs -command {dt act onefs}
ttk::checkbutton .tb.group -text "Dosyaları grupla" -variable ::group -command {dt act group}
ttk::menubutton .tb.gmin -textvariable ::groupMinText -menu .tb.gmin.m
menu .tb.gmin.m
foreach n {8 16 32 64 128} {
    .tb.gmin.m add radiobutton -label "En az $n öğe" -variable ::groupmin -value $n -command {dt act groupmin}
}
ttk::checkbutton .tb.tm -text "Treemap" -variable ::showtm -command toggleTreemap
ttk::menubutton .tb.theme -text "Tema" -menu .tb.theme.m
menu .tb.theme.m
foreach {id label} {system Sistem light Açık dark Koyu} {
    .tb.theme.m add radiobutton -label $label -variable ::theme -value $id -command {dt act theme}
}
ttk::button .tb.about -text "Hakkında" -command {dt act about}
pack .tb.open .tb.rescan .tb.stop -side left -padx 1
pack .tb.s1 -side left -fill y -padx 4
pack .tb.zin .tb.zout .tb.zroot -side left -padx 1
pack .tb.s2 -side left -fill y -padx 4
pack .tb.app .tb.onefs .tb.group .tb.gmin .tb.tm -side left -padx 4
pack .tb.about .tb.theme -side right -padx 2

# Durum çubuğu
ttk::frame .sb -padding {6 2}
ttk::label .sb.l -textvariable ::statusL -anchor w -width 1
ttk::progressbar .sb.p -mode indeterminate -length 120
ttk::label .sb.r -textvariable ::statusR -anchor e
pack .sb.r -side right
pack .sb.l -side left -fill x -expand 1

ttk::panedwindow .pv -orient vertical
ttk::panedwindow .ph -orient horizontal

# Klasör ağacı
ttk::frame .tf
ttk::treeview .t -columns {bar pct size items files dirs mtime} -selectmode browse
.t heading #0 -text "Ad" -anchor w -command {dt sort 0}
set i 1
foreach {col title w} {bar "Alt ağaç" 120 pct "%" 70 size "Boyut" 90 items "Öğe" 80 files "Dosya" 80 dirs "Klasör" 70 mtime "Son değişiklik" 140} {
    .t heading $col -text $title -anchor e -command [list dt sort $i]
    .t column $col -width $w -anchor e -stretch 0
    incr i
}
.t heading bar -anchor w
.t column bar -anchor w
.t column #0 -width 340 -stretch 1
.t tag configure err -foreground #dc4646
.t tag configure mount -foreground #8c8c8c
ttk::scrollbar .tf.ys -orient vertical -command {.t yview}
ttk::scrollbar .tf.xs -orient horizontal -command {.t xview}
.t configure -yscrollcommand {.tf.ys set} -xscrollcommand {.tf.xs set}
grid .t -in .tf -row 0 -column 0 -sticky nsew
grid .tf.ys -row 0 -column 1 -sticky ns
grid .tf.xs -row 1 -column 0 -sticky ew
grid rowconfigure .tf 0 -weight 1
grid columnconfigure .tf 0 -weight 1
bind .t <<TreeviewOpen>> {dt open [.t focus]}
bind .t <<TreeviewSelect>> {dt select}
bind .t <Double-1> {dt activate [.t identify item %x %y]}
bind .t <Button-3> {
    set it [.t identify item %x %y]
    if {$it ne ""} { .t selection set $it; .t focus $it; dt treemenu $it %X %Y }
}

# Uzantılar
ttk::frame .ef
ttk::treeview .e -columns {desc size pct count} -selectmode browse
.e heading #0 -text "Uzantı" -anchor w -command {dt extsort 0}
.e heading desc -text "Tür" -anchor w -command {dt extsort 1}
.e heading size -text "Boyut" -anchor e -command {dt extsort 2}
.e heading pct -text "%" -anchor e -command {dt extsort 2}
.e heading count -text "Dosya" -anchor e -command {dt extsort 3}
.e column #0 -width 90 -stretch 0
.e column desc -width 200 -stretch 1
.e column size -width 80 -anchor e -stretch 0
.e column pct -width 60 -anchor e -stretch 0
.e column count -width 70 -anchor e -stretch 0
ttk::scrollbar .ef.ys -orient vertical -command {.e yview}
.e configure -yscrollcommand {.ef.ys set}
grid .e -in .ef -row 0 -column 0 -sticky nsew
grid .ef.ys -row 0 -column 1 -sticky ns
grid rowconfigure .ef 0 -weight 1
grid columnconfigure .ef 0 -weight 1
bind .e <<TreeviewSelect>> {dt extselect}

# Treemap
ttk::frame .tmf
ttk::label .tmf.z -textvariable ::zoomText -padding {6 2}
canvas .tmf.c -background #1e1e1e -highlightthickness 0 -height 80
.tmf.c create image 0 0 -anchor nw -image tmimg
pack .tmf.z -fill x
pack .tmf.c -fill both -expand 1
bind .tmf.c <Configure> {dt resize}
bind .tmf.c <Motion> {dt hover %x %y}
bind .tmf.c <Leave> {dt hover -1 -1}
bind .tmf.c <Button-1> {dt click %x %y}
bind .tmf.c <Double-Button-1> {dt zoomat %x %y}
bind .tmf.c <Button-3> {dt tmmenu %x %y %X %Y}

.ph add .tf -weight 2
.ph add .ef -weight 1
.pv add .ph -weight 3
.pv add .tmf -weight 2
pack .tb -side top -fill x
pack .sb -side bottom -fill x
pack .pv -fill both -expand 1

# Koyu/açık palet: ttk (clam) stilleri ve klasik Tk bileşenleri (menüler, iletişim kutuları).
proc applyPalette {dark} {
    if {$dark} {
        array set c {bg #1f2227 fg #e3e5e8 base #181a1e btn #2a2e35 border #3a3f47 sel #3d7eff dis #666c75 hover #333840}
    } else {
        array set c {bg #f2f3f5 fg #1f2328 base #ffffff btn #e8eaee border #c4c8cf sel #2f6fed dis #9da3ab hover #dde1e7}
    }
    tk_setPalette background $c(bg) foreground $c(fg) activeBackground $c(hover) activeForeground $c(fg) \
        selectBackground $c(sel) selectForeground #ffffff highlightColor $c(sel) insertBackground $c(fg) \
        troughColor $c(base) disabledForeground $c(dis)
    ttk::style configure . -background $c(bg) -foreground $c(fg) -fieldbackground $c(base) \
        -bordercolor $c(border) -lightcolor $c(btn) -darkcolor $c(btn) -troughcolor $c(base) \
        -selectbackground $c(sel) -selectforeground #ffffff -insertcolor $c(fg) -focuscolor $c(sel)
    ttk::style map . -background [list disabled $c(bg) active $c(hover)] -foreground [list disabled $c(dis)]
    ttk::style configure TButton -background $c(btn) -padding {8 3}
    ttk::style map TButton -background [list disabled $c(bg) pressed $c(border) active $c(hover)]
    ttk::style configure TMenubutton -background $c(btn) -arrowcolor $c(fg)
    ttk::style map TMenubutton -background [list active $c(hover)]
    ttk::style configure TCheckbutton -background $c(bg) -indicatorbackground $c(base) -indicatorforeground $c(sel)
    ttk::style map TCheckbutton -background [list active $c(bg)] -indicatorbackground [list selected $c(base)]
    ttk::style configure Treeview -background $c(base) -fieldbackground $c(base) -foreground $c(fg) -bordercolor $c(border)
    ttk::style map Treeview -background [list selected $c(sel)] -foreground [list selected #ffffff]
    ttk::style configure Treeview.Heading -background $c(bg) -foreground $c(fg) -relief flat -padding {6 4}
    ttk::style map Treeview.Heading -background [list active $c(hover)]
    ttk::style configure TScrollbar -background $c(btn) -troughcolor $c(bg) -arrowcolor $c(fg) -bordercolor $c(bg)
    ttk::style map TScrollbar -background [list active $c(border)]
    ttk::style configure TProgressbar -background $c(sel) -troughcolor $c(base)
    ttk::style configure TPanedwindow -background $c(bg)
    ttk::style configure Sash -sashthickness 6 -gripcount 0 -background $c(bg)
    .t tag configure mount -foreground $c(dis)
    foreach m {.tb.gmin.m .tb.theme.m} { $m configure -background $c(btn) -foreground $c(fg) -activebackground $c(sel) -activeforeground #ffffff -selectcolor $c(sel) }
}

proc toggleTreemap {} {
    if {$::showtm} { .pv add .tmf -weight 2 } else { .pv forget .tmf }
    dt act save
}

bind . <F5> {dt act rescan}
bind . <Control-o> {dt act open}
bind . <Escape> {dt act stop}
bind . <BackSpace> {dt act zoomout}
bind . <Delete> {dt act trash}
bind . <Shift-Delete> {dt act delete}
bind . <Control-c> {dt act copy}
bind . <Control-q> {dt act quit}
bind . <Control-0> {dt act zoomroot}
bind . <Control-plus> {dt act zoomin}
bind . <F9> {set ::showtm [expr {!$::showtm}]; toggleTreemap}
bind . <Control-g> {set ::group [expr {!$::group}]; dt act group}

proc showMenu {X Y isDir isMount isGroup hasParent busy} {
    destroy .ctx
    menu .ctx
    set st [expr {$busy ? "disabled" : "normal"}]
    .ctx add command -label "Aç" -command {dt nact open}
    .ctx add command -label "Dosya yöneticisinde göster" -command {dt nact reveal}
    .ctx add command -label "Yolu kopyala" -command {dt nact copy}
    if {$isDir} {
        .ctx add separator
        .ctx add command -label "Treemap'te yakınlaştır" -command {dt nact zoom}
        set lbl [expr {$isGroup ? "Bulunduğu klasörü yeniden tara" : $isMount ? "Bu dosya sistemini de tara" : "Bu klasörü yeniden tara"}]
        .ctx add command -label $lbl -command {dt nact refresh} -state $st
    }
    if {$hasParent} {
        .ctx add separator
        .ctx add command -label "Çöp kutusuna taşı" -command {dt nact trash} -state $st
        .ctx add command -label "Kalıcı olarak sil…" -command {dt nact delete} -state $st
    }
    tk_popup .ctx $X $Y
}

proc confirm {title detail} {
    set r [tk_messageBox -parent . -icon warning -type yesno -default no -title TuxDirStat -message $title -detail $detail]
    return [expr {$r eq "yes"}]
}

proc showError {title detail} {
    tk_messageBox -parent . -icon warning -type ok -title TuxDirStat -message $title -detail $detail
}

proc startDialog {} {
    set w .start
    destroy $w
    toplevel $w
    wm title $w "TuxDirStat — Taranacak konum"
    wm transient $w .
    wm geometry $w 720x400
    ttk::frame $w.f -padding 10
    pack $w.f -fill both -expand 1
    ttk::label $w.f.l -text "Taramak istediğiniz konumu seçin:"
    ttk::treeview $w.f.t -columns {fs used total pct} -show {tree headings} -selectmode browse
    $w.f.t heading #0 -text "Konum" -anchor w
    $w.f.t heading fs -text "Tür" -anchor w
    $w.f.t heading used -text "Kullanılan" -anchor e
    $w.f.t heading total -text "Toplam" -anchor e
    $w.f.t heading pct -text "Doluluk" -anchor w
    $w.f.t column #0 -width 280
    $w.f.t column fs -width 80 -stretch 0
    $w.f.t column used -width 90 -anchor e -stretch 0
    $w.f.t column total -width 90 -anchor e -stretch 0
    $w.f.t column pct -width 150 -stretch 0
    foreach d [dt drives] {
        lassign $d label path fs used total pct
        $w.f.t insert {} end -text $label -values [list $fs $used $total $pct] -tags [list $path]
    }
    set first [lindex [$w.f.t children {}] 0]
    $w.f.t selection set $first
    $w.f.t focus $first
    ttk::frame $w.f.b
    ttk::button $w.f.b.pick -text "Klasör seç…" -command {
        set d [tk_chooseDirectory -parent .start -title "Klasör seç" -initialdir [dt home] -mustexist 1]
        if {$d ne ""} { destroy .start; dt scan $d }
    }
    ttk::button $w.f.b.cancel -text "İptal" -command [list destroy $w]
    ttk::button $w.f.b.ok -text "Tara" -command startDialogAccept
    pack $w.f.b.pick -side left
    pack $w.f.b.ok $w.f.b.cancel -side right -padx 2
    pack $w.f.l -anchor w -pady {0 6}
    pack $w.f.b -side bottom -fill x -pady {8 0}
    pack $w.f.t -fill both -expand 1
    bind $w.f.t <Double-1> startDialogAccept
    bind $w <Return> startDialogAccept
    bind $w <Escape> [list destroy $w]
    focus $w.f.t
}

proc startDialogAccept {} {
    set sel [.start.f.t selection]
    if {$sel eq ""} return
    set path [lindex [.start.f.t item $sel -tags] 0]
    destroy .start
    dt scan $path
}
)TCL";

enum Col { C_NAME, C_BAR, C_PCT, C_SIZE, C_ITEMS, C_FILES, C_DIRS, C_MTIME, NCOLS };
const char *const HEADINGS[NCOLS] = {"Ad", "Alt ağaç", "%", "Boyut", "Öğe", "Dosya", "Klasör", "Son değişiklik"};
const char *const HEADING_IDS[NCOLS] = {"#0", "bar", "pct", "size", "items", "files", "dirs", "mtime"};
const SortKey COL_KEYS[NCOLS] = {SortKey::Name, SortKey::Size, SortKey::Size, SortKey::Size,
                                 SortKey::Items, SortKey::Files, SortKey::Dirs, SortKey::MTime};

Tcl_Obj *S(const std::string &s)
{
    return Tcl_NewStringObj(s.data(), int(s.size()));
}

Tcl_Obj *S(const char *s)
{
    return Tcl_NewStringObj(s, -1);
}

std::string itemId(const Node *n)
{
    char b[32];
    std::snprintf(b, sizeof b, "n%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(n)));
    return b;
}

std::string dummyId(const Node *n)
{
    return "d" + itemId(n).substr(1);
}

// Yalnızca kendi ürettiğimiz kimlikleri çözer; silinen öğeler ağaçtan da kaldırılır.
Node *nodeOf(const char *id)
{
    if (!id || id[0] != 'n')
        return nullptr;
    return reinterpret_cast<Node *>(std::strtoull(id + 1, nullptr, 16));
}

std::string textBar(double f)
{
    const int n = std::clamp(int(f * 12 + 0.5), 0, 12);
    std::string s;
    for (int i = 0; i < 12; ++i)
        s += i < n ? "█" : "░";
    return s;
}

std::string colorHex(const RGBf &c)
{
    char b[16];
    auto ch = [](float v) { return int(std::min(255.f, v * 1.4f)); };
    std::snprintf(b, sizeof b, "#%02x%02x%02x", ch(c.r), ch(c.g), ch(c.b));
    return b;
}

class TkApp {
public:
    explicit TkApp(Tcl_Interp *in) : in_(in) {}
    static int command(ClientData cd, Tcl_Interp *in, int objc, Tcl_Obj *const objv[]);
    void startScan(const std::string &path, Node *target = nullptr);
    void showStartDialog() { eval("startDialog"); }

private:
    // Tcl yardımcıları
    int call(std::initializer_list<Tcl_Obj *> objs);
    int eval(const char *script) { return Tcl_EvalEx(in_, script, -1, TCL_EVAL_GLOBAL); }
    std::string result() const { return Tcl_GetStringResult(in_); }
    void setVar(const char *name, const std::string &v) { Tcl_SetVar2Ex(in_, name, nullptr, S(v), TCL_GLOBAL_ONLY); }
    bool varBool(const char *name)
    {
        int b = 0;
        Tcl_Obj *o = Tcl_GetVar2Ex(in_, name, nullptr, TCL_GLOBAL_ONLY);
        return o && Tcl_GetBooleanFromObj(in_, o, &b) == TCL_OK && b;
    }
    void setState(const char *widget, bool on) { call({S(widget), S("configure"), S("-state"), S(on ? "normal" : "disabled")}); }

    // tarama
    static void pollTimer(ClientData cd);
    void poll();

    // ağaç
    Tcl_Obj *rowValues(const Node *n);
    void insertRow(const std::string &parentId, Node *c, int index);
    void fill(Node *n);
    void forgetFilled(Node *subtree);
    void refreshValues();
    void resort();
    void updateHeadings();
    void selectNode(Node *n);
    Node *selectedNode();

    // uzantılar, durum
    void fillExt();
    std::string extDescription(uint32_t ext);
    void updateTotals();
    void updateZoom();
    void setStatus(Node *n);
    void afterChange();

    // treemap
    static void renderTimer(ClientData cd);
    void scheduleRender(int ms);
    void render();
    void compose();
    Node *hit(int x, int y);
    void zoomTo(Node *n);

    // eylemler
    void action(const std::string &name);
    void nodeAction(const std::string &name);
    void popupMenu(Node *n, int X, int Y);
    void copyPath(Node *n);
    void deleteNode(Node *n, bool permanent);
    void refreshNode(Node *n);
    void rebuildTree(Node *select);
    void regroup(bool on, uint32_t minCount);

public:
    void applySettings();

private:
    void saveSettings();
    bool applyingSettings_ = false;

    Tcl_Interp *in_;
    Session s_;
    std::unordered_set<Node *> filled_;  // çocuk satırları eklenmiş klasörler
    uint32_t gen_ = 1;
    int sortCol_ = C_SIZE;
    bool sortDesc_ = true;
    int extSortCol_ = 2;
    bool extSortDesc_ = true;
    std::vector<std::pair<uint32_t, std::string>> extDescCache_;
    std::unordered_set<std::string> swatches_;

    TreemapRenderer tm_;
    std::vector<uint32_t> px_;
    Node *tmRoot_ = nullptr, *selected_ = nullptr, *hover_ = nullptr, *menuNode_ = nullptr;
    int highlight_ = -1;
    Tcl_TimerToken pollToken_ = nullptr, renderToken_ = nullptr;
};

int TkApp::call(std::initializer_list<Tcl_Obj *> objs)
{
    std::vector<Tcl_Obj *> v(objs);
    for (Tcl_Obj *o : v)
        Tcl_IncrRefCount(o);
    int rc = Tcl_EvalObjv(in_, int(v.size()), v.data(), TCL_EVAL_GLOBAL);
    for (Tcl_Obj *o : v)
        Tcl_DecrRefCount(o);
    return rc;
}

// ───────────── tarama ─────────────

void TkApp::startScan(const std::string &path, Node *target)
{
    char *real = realpath(path.c_str(), nullptr);
    const std::string abs = real ? real : path;
    std::free(real);
    s_.oneFs = varBool("::onefs");
    s_.startScan(abs, target);
    setState(".tb.stop", true);
    eval("pack .sb.p -side right -padx 6; .sb.p start 10");
    if (!target)
        call({S("wm"), S("title"), S("."), S("TuxDirStat — " + abs)});
    if (!pollToken_)
        pollToken_ = Tcl_CreateTimerHandler(100, pollTimer, this);
}

void TkApp::pollTimer(ClientData cd)
{
    auto *a = static_cast<TkApp *>(cd);
    a->pollToken_ = nullptr;
    a->poll();
}

void TkApp::poll()
{
    const Session::Result res = s_.poll();
    if (res == Session::Result::Running) {
        setVar("::statusL", "Taranıyor… " + fmtInt(s_.progressCount()) + " öğe · " + fmtSize(s_.progressBytes()) +
                                " · " + s_.progressPath());
        pollToken_ = Tcl_CreateTimerHandler(100, pollTimer, this);
        return;
    }
    eval(".sb.p stop; pack forget .sb.p");
    setState(".tb.stop", false);
    switch (res) {
    case Session::Result::Failed:
        setVar("::statusL", "");
        call({S("showError"), S("Tarama başarısız"), S(s_.error())});
        return;
    case Session::Result::Cancelled:
        setVar("::statusL", "Tarama durduruldu.");
        return;
    case Session::Result::NewTree: {
        Node *root = s_.pendingRoot();
        tm_.forgetNodes();
        tmRoot_ = root;
        selected_ = hover_ = menuNode_ = nullptr;
        s_.commitNewTree();
        rebuildTree(root);
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
        const std::string tid = itemId(target);
        eval(("set _k [.t children " + tid + "]; if {[llength $_k]} {.t delete $_k}").c_str());
        forgetFilled(target);
        takeChildren(target, s_.pendingRoot());
        s_.commitRefresh();
        fill(target);
        call({S(".t"), S("item"), S(tid), S("-open"), S("1")});
        if (selected_)
            selectNode(selected_);
        break;
    }
    default:
        return;
    }
    afterChange();
    char secs[32];
    std::snprintf(secs, sizeof secs, "%.2f", s_.elapsed());
    setVar("::statusL", "Tarama tamamlandı: " + fmtInt(s_.lastCount()) + " öğe, " + secs + " sn.");
}

// ───────────── ağaç ─────────────

Tcl_Obj *TkApp::rowValues(const Node *n)
{
    const bool ap = s_.apparent;
    double f = 1.0;
    if (n != s_.root() && n->parent) {
        const uint64_t tot = n->parent->sizeOf(ap);
        f = tot ? double(n->sizeOf(ap)) / double(tot) : 0.0;
    }
    Tcl_Obj *l = Tcl_NewListObj(0, nullptr);
    auto add = [&](const std::string &s) { Tcl_ListObjAppendElement(nullptr, l, S(s)); };
    add(textBar(f));
    add(fmtPct(f));
    add(fmtSize(n->sizeOf(ap)));
    add(n->isDir ? fmtInt(uint64_t(n->files) + n->dirs) : "");
    add(n->isDir ? fmtInt(n->files) : "");
    add(n->isDir ? fmtInt(n->dirs) : "");
    add(fmtTime(n->mtime));
    return l;
}

void TkApp::insertRow(const std::string &parentId, Node *c, int index)
{
    const std::string name = displayName(c, parentId.empty());
    const char *tag = (c->flags & F_ERROR) ? "err" : (c->flags & F_MOUNT) ? "mount" : "";
    const char *icon = (c->flags & F_GROUP) ? "ico_group"
                       : c->isDir           ? "ico_dir"
                       : (c->flags & F_LINK) ? "ico_link"
                                             : "ico_file";
    call({S(".t"), S("insert"), S(parentId), S(std::to_string(index)), S("-id"), S(itemId(c)), S("-text"), S(name),
          S("-image"), S(icon), S("-values"), rowValues(c), S("-tags"), S(tag)});
    if (c->isDir && !c->children.empty())
        call({S(".t"), S("insert"), S(itemId(c)), S("0"), S("-id"), S(dummyId(c))});
}

// Klasörün çocuk satırlarını ekler. Başa ekleme sabit sürelidir; bu yüzden sondan başa doğru eklenir.
void TkApp::fill(Node *n)
{
    if (filled_.count(n))
        return;
    if (n->sgen != gen_) {
        sortKids(n->children, COL_KEYS[sortCol_], sortDesc_, s_.apparent);
        n->sgen = gen_;
    }
    const std::string pid = itemId(n);
    for (size_t i = n->children.size(); i-- > 0;)
        insertRow(pid, n->children[i], 0);
    filled_.insert(n);
}

void TkApp::forgetFilled(Node *subtree)
{
    std::vector<Node *> st{subtree};
    while (!st.empty()) {
        Node *n = st.back();
        st.pop_back();
        if (!filled_.erase(n))
            continue;
        for (Node *c : n->children)
            if (c->isDir)
                st.push_back(c);
    }
}

void TkApp::refreshValues()
{
    if (Node *root = s_.root())
        call({S(".t"), S("item"), S(itemId(root)), S("-values"), rowValues(root)});
    for (Node *n : filled_)
        for (Node *c : n->children)
            call({S(".t"), S("item"), S(itemId(c)), S("-values"), rowValues(c)});
}

void TkApp::resort()
{
    ++gen_;
    for (Node *n : filled_) {
        sortKids(n->children, COL_KEYS[sortCol_], sortDesc_, s_.apparent);
        n->sgen = gen_;
        Tcl_Obj *ids = Tcl_NewListObj(0, nullptr);
        for (Node *c : n->children)
            Tcl_ListObjAppendElement(nullptr, ids, S(itemId(c)));
        call({S(".t"), S("children"), S(itemId(n)), ids});
    }
    updateHeadings();
}

void TkApp::updateHeadings()
{
    for (int c = 0; c < NCOLS; ++c) {
        std::string t = HEADINGS[c];
        if (c == sortCol_)
            t += sortDesc_ ? "  ▼" : "  ▲";
        call({S(".t"), S("heading"), S(HEADING_IDS[c]), S("-text"), S(t)});
    }
}

void TkApp::selectNode(Node *n)
{
    if (!n || !s_.root() || !n->isWithin(s_.root()))
        return;
    std::vector<Node *> chain;
    for (Node *p = n->parent; p; p = p->parent)
        chain.push_back(p);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        Node *p = *it;
        if (!filled_.count(p)) {
            call({S(".t"), S("delete"), S(dummyId(p))});
            fill(p);
        }
        call({S(".t"), S("item"), S(itemId(p)), S("-open"), S("1")});
    }
    const std::string id = itemId(n);
    call({S(".t"), S("selection"), S("set"), S(id)});
    call({S(".t"), S("focus"), S(id)});
    call({S(".t"), S("see"), S(id)});
}

Node *TkApp::selectedNode()
{
    if (call({S(".t"), S("selection")}) != TCL_OK)
        return nullptr;
    const std::string r = result();
    return nodeOf(r.c_str());
}

// ───────────── uzantılar, durum ─────────────

std::string TkApp::extDescription(uint32_t ext)
{
    for (auto &[e, d] : extDescCache_)
        if (e == ext)
            return d;
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
    extDescCache_.emplace_back(ext, d);
    return d;
}

void TkApp::fillExt()
{
    std::vector<ExtRow> rows = s_.ext;
    auto less = [this](const ExtRow &a, const ExtRow &b) {
        switch (extSortCol_) {
        case 0:
            return s_.extLabel(a.ext) < s_.extLabel(b.ext);
        case 1:
            return extDescription(a.ext) < extDescription(b.ext);
        case 3:
            return a.count < b.count;
        default:
            return a.bytes < b.bytes;
        }
    };
    std::stable_sort(rows.begin(), rows.end(),
                     [&](const ExtRow &a, const ExtRow &b) { return extSortDesc_ ? less(b, a) : less(a, b); });
    Node *root = s_.root();
    const double total = root ? std::max<uint64_t>(1, root->sizeOf(s_.apparent)) : 1;
    eval(".e delete [.e children {}]");
    for (const ExtRow &r : rows) {
        const std::string img = "sw" + colorHex(r.color).substr(1);
        if (swatches_.insert(img).second) {
            call({S("image"), S("create"), S("photo"), S(img), S("-width"), S("14"), S("-height"), S("14")});
            call({S(img), S("put"), S(colorHex(r.color)), S("-to"), S("0"), S("0"), S("14"), S("14")});
        }
        Tcl_Obj *vals = Tcl_NewListObj(0, nullptr);
        for (const std::string &v : {extDescription(r.ext), fmtSize(r.bytes), fmtPct(r.bytes / total), fmtInt(r.count)})
            Tcl_ListObjAppendElement(nullptr, vals, S(v));
        call({S(".e"), S("insert"), S(""), S("end"), S("-id"), S("e" + std::to_string(r.ext)), S("-text"),
              S(s_.extLabel(r.ext)), S("-image"), S(img), S("-values"), vals});
    }
    highlight_ = -1;
}

void TkApp::updateTotals()
{
    Node *root = s_.root();
    if (!root) {
        setVar("::statusR", "");
        return;
    }
    setVar("::statusR", "Toplam: " + fmtSize(root->sizeOf(s_.apparent)) + " · " + fmtInt(root->files) + " dosya · " +
                            fmtInt(root->dirs) + " klasör · Boş alan: " + fmtSize(freeSpace(root->path())));
}

void TkApp::updateZoom()
{
    setVar("::zoomText", tmRoot_ ? "Treemap: " + tmRoot_->path() : std::string("Treemap"));
    setState(".tb.zout", tmRoot_ && tmRoot_->parent);
}

void TkApp::setStatus(Node *n)
{
    if (n && !s_.scanning())
        setVar("::statusL", n->path() + "  —  " + fmtSize(n->sizeOf(s_.apparent)));
}

void TkApp::afterChange()
{
    fillExt();
    refreshValues();
    scheduleRender(0);
    updateTotals();
    updateZoom();
}

// ───────────── treemap ─────────────

void TkApp::renderTimer(ClientData cd)
{
    auto *a = static_cast<TkApp *>(cd);
    a->renderToken_ = nullptr;
    a->render();
}

void TkApp::scheduleRender(int ms)
{
    if (renderToken_)
        Tcl_DeleteTimerHandler(renderToken_);
    renderToken_ = Tcl_CreateTimerHandler(ms, renderTimer, this);
}

void TkApp::render()
{
    eval("list [winfo width .tmf.c] [winfo height .tmf.c]");
    int w = 0, h = 0;
    std::sscanf(result().c_str(), "%d %d", &w, &h);
    tm_.render(tmRoot_, w, h, s_.apparent, s_.colors);
    compose();
}

void TkApp::compose()
{
    tm_.compose(px_, highlight_, selected_);
    Tk_PhotoHandle ph = Tk_FindPhoto(in_, "tmimg");
    if (!ph)
        return;
    if (px_.empty()) {
        Tk_PhotoBlank(ph);
        return;
    }
    const int w = tm_.width(), h = tm_.height();
    Tk_PhotoSetSize(in_, ph, w, h);
    Tk_PhotoImageBlock blk;
    blk.pixelPtr = reinterpret_cast<unsigned char *>(px_.data());
    blk.width = w;
    blk.height = h;
    blk.pitch = w * 4;
    blk.pixelSize = 4;
    // 0xFFRRGGBB küçük-sonlu bellekte B,G,R,A sırasıyla durur.
    blk.offset[0] = 2;
    blk.offset[1] = 1;
    blk.offset[2] = 0;
    blk.offset[3] = 3;
    Tk_PhotoPutBlock(in_, ph, &blk, 0, 0, w, h, TK_PHOTO_COMPOSITE_SET);
}

Node *TkApp::hit(int x, int y)
{
    if (x < 0 || y < 0)
        return nullptr;
    eval("list [winfo width .tmf.c] [winfo height .tmf.c]");
    int w = 0, h = 0;
    std::sscanf(result().c_str(), "%d %d", &w, &h);
    if (w <= 0 || h <= 0)
        return nullptr;
    return tm_.hit(int(double(x) / w * tm_.width()), int(double(y) / h * tm_.height()));
}

void TkApp::zoomTo(Node *n)
{
    if (!n || !n->isDir)
        return;
    tmRoot_ = n;
    tm_.forgetNodes();
    scheduleRender(0);
    updateZoom();
}

// ───────────── eylemler ─────────────

void TkApp::action(const std::string &a)
{
    if (a == "open") {
        showStartDialog();
    } else if (a == "rescan") {
        if (s_.root())
            startScan(s_.root()->path());
        else
            showStartDialog();
    } else if (a == "stop") {
        s_.cancelScan();
    } else if (a == "zoomin") {
        if (Node *n = selectedNode())
            zoomTo(n->isDir ? n : n->parent);
    } else if (a == "zoomout") {
        if (tmRoot_ && tmRoot_->parent)
            zoomTo(tmRoot_->parent);
    } else if (a == "zoomroot") {
        zoomTo(s_.root());
    } else if (a == "copy") {
        if (Node *n = selectedNode())
            copyPath(n);
    } else if (a == "trash" || a == "delete") {
        if (Node *n = selectedNode())
            deleteNode(n, a == "delete");
    } else if (a == "apparent") {
        s_.apparent = varBool("::apparent");
        resort();
        s_.recomputeExt();
        afterChange();
        saveSettings();
    } else if (a == "group") {
        regroup(varBool("::group"), s_.groupMin);
    } else if (a == "groupmin") {
        Tcl_Obj *o = Tcl_GetVar2Ex(in_, "::groupmin", nullptr, TCL_GLOBAL_ONLY);
        int n = GROUP_MIN;
        if (o)
            Tcl_GetIntFromObj(in_, o, &n);
        regroup(s_.grouping, uint32_t(n));
    } else if (a == "theme") {
        const char *t = Tcl_GetVar(in_, "::theme", TCL_GLOBAL_ONLY);
        call({S("applyPalette"), S(wantsDark(t ? t : "system") ? "1" : "0")});
        saveSettings();
    } else if (a == "save") {
        saveSettings();
    } else if (a == "onefs") {
        s_.oneFs = varBool("::onefs");
        saveSettings();
    } else if (a == "about") {
        call({S("tk_messageBox"), S("-parent"), S("."), S("-icon"), S("info"), S("-title"), S("TuxDirStat"), S("-message"),
              S("TuxDirStat"), S("-detail"),
              S("Linux için disk kullanım analizörü.\n\nAğaçta veya treemap'te bir öğeye tıklayın; treemap'te çift "
                "tıklayarak klasöre yakınlaşın, Backspace ile geri çıkın. Uzantı listesinde bir satır seçmek o "
                "türdeki dosyaları treemap'te işaretler.")});
    } else if (a == "quit") {
        s_.cancelScan();
        if (pollToken_)
            Tcl_DeleteTimerHandler(pollToken_);
        if (renderToken_)
            Tcl_DeleteTimerHandler(renderToken_);
        pollToken_ = renderToken_ = nullptr;
        eval("destroy .");
    }
}

void TkApp::nodeAction(const std::string &a)
{
    Node *n = menuNode_;
    if (!n)
        return;
    if (a == "open")
        openPath(n->path());
    else if (a == "reveal")
        revealPath(n->path());
    else if (a == "copy")
        copyPath(n);
    else if (a == "zoom")
        zoomTo(n);
    else if (a == "refresh")
        refreshNode(n);
    else if (a == "trash" || a == "delete")
        deleteNode(n, a == "delete");
}

void TkApp::popupMenu(Node *n, int X, int Y)
{
    menuNode_ = n;
    call({S("showMenu"), Tcl_NewIntObj(X), Tcl_NewIntObj(Y), Tcl_NewIntObj(n->isDir), Tcl_NewIntObj((n->flags & F_MOUNT) != 0),
          Tcl_NewIntObj((n->flags & F_GROUP) != 0), Tcl_NewIntObj(n->parent != nullptr), Tcl_NewIntObj(s_.scanning())});
}

void TkApp::copyPath(Node *n)
{
    eval("clipboard clear");
    call({S("clipboard"), S("append"), S("--"), S(n->path())});
}

void TkApp::refreshNode(Node *n)
{
    n = realNode(n);  // grup için bulunduğu klasör
    if (n && !s_.scanning())
        startScan(n->path(), n);
}

// Ağaç satırlarını baştan kurar (yeni ağaç ya da gruplama değişikliği).
void TkApp::rebuildTree(Node *select)
{
    eval(".t delete [.t children {}]");
    filled_.clear();
    ++gen_;
    Node *root = s_.root();
    if (!root)
        return;
    root->row = 0;
    insertRow("", root, 0);
    call({S(".t"), S("delete"), S(dummyId(root))});
    fill(root);
    call({S(".t"), S("item"), S(itemId(root)), S("-open"), S("1")});
    updateHeadings();
    selectNode(select ? select : root);
}

void TkApp::regroup(bool on, uint32_t minCount)
{
    // Eski gruplar ağaçtan ayrılır; seçimi ve yakınlaştırmayı gerçek düğüme taşı.
    Node *sel = realNode(selectedNode());
    Node *zoom = realNode(tmRoot_);
    tm_.forgetNodes();
    selected_ = hover_ = menuNode_ = nullptr;
    s_.setGrouping(on, minCount);
    setVar("::groupMinText", "Eşik: " + std::to_string(minCount));
    saveSettings();
    tmRoot_ = zoom ? zoom : s_.root();
    rebuildTree(sel);
    scheduleRender(0);
    updateZoom();
}

void TkApp::deleteNode(Node *n, bool permanent)
{
    if (!n->parent || s_.scanning())
        return;
    const std::string path = n->path();
    const std::string kind = (n->flags & F_GROUP) ? "gruptaki " + fmtInt(n->files) + " dosyayı"
                             : n->isDir           ? "klasörü"
                                                  : "dosyayı";
    const std::string title = permanent ? "Bu " + kind + " kalıcı olarak silmek istediğinize emin misiniz?"
                                        : "Bu " + kind + " çöp kutusuna taşınsın mı?";
    const std::string detail = path + "\n(" + fmtSize(n->sizeOf(s_.apparent)) + ")" +
                               (permanent ? "\n\nBu işlem geri alınamaz." : "");
    if (call({S("confirm"), S(title), S(detail)}) != TCL_OK || result() != "1")
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
        call({S("showError"), S("Silinemedi (ya da yalnızca kısmen silindi)"), S(firstFailed + "\n\n" + err)});
        refreshNode(n->parent);  // ağacı diskle eşitle
        return;
    }
    n = s_.removalTarget(n);  // grubun son öğesiyse grup da kalkar
    Node *p = n->parent;
    tm_.forgetNodes();
    hover_ = menuNode_ = nullptr;
    if (tmRoot_ && tmRoot_->isWithin(n))
        tmRoot_ = p;
    if (selected_ && selected_->isWithin(n))
        selected_ = nullptr;
    call({S(".t"), S("delete"), S(itemId(n))});
    forgetFilled(n);
    s_.remove(n);
    afterChange();
}

// ───────────── ayarlar ─────────────

void TkApp::applySettings()
{
    applyingSettings_ = true;
    const Settings st = Settings::load();
    s_.apparent = st.apparent;
    s_.oneFs = st.oneFs;
    s_.grouping = st.grouping;
    s_.groupMin = st.groupMin;
    setVar("::apparent", st.apparent ? "1" : "0");
    setVar("::onefs", st.oneFs ? "1" : "0");
    setVar("::group", st.grouping ? "1" : "0");
    setVar("::groupmin", std::to_string(st.groupMin));
    setVar("::groupMinText", "Eşik: " + std::to_string(st.groupMin));
    setVar("::showtm", st.showTreemap ? "1" : "0");
    setVar("::theme", st.theme);
    call({S("applyPalette"), S(wantsDark(st.theme) ? "1" : "0")});
    if (!st.showTreemap)
        eval("toggleTreemap");
    applyingSettings_ = false;
}

void TkApp::saveSettings()
{
    if (applyingSettings_)
        return;
    Settings st;
    st.apparent = s_.apparent;
    st.oneFs = s_.oneFs;
    st.grouping = s_.grouping;
    st.groupMin = s_.groupMin;
    st.showTreemap = varBool("::showtm");
    if (const char *t = Tcl_GetVar(in_, "::theme", TCL_GLOBAL_ONLY))
        st.theme = t;
    st.save();
}

// ───────────── Tcl komutu ─────────────

int TkApp::command(ClientData cd, Tcl_Interp *in, int objc, Tcl_Obj *const objv[])
{
    auto *a = static_cast<TkApp *>(cd);
    if (objc < 2) {
        Tcl_WrongNumArgs(in, 1, objv, "alt-komut ?argümanlar?");
        return TCL_ERROR;
    }
    const std::string sub = Tcl_GetString(objv[1]);
    auto arg = [&](int i) -> std::string { return i < objc ? Tcl_GetString(objv[i]) : ""; };
    auto num = [&](int i) { return std::atoi(arg(i).c_str()); };

    if (sub == "open") {
        if (Node *n = nodeOf(arg(2).c_str()); n && n->isDir && !a->filled_.count(n)) {
            a->call({S(".t"), S("delete"), S(dummyId(n))});
            a->fill(n);
        }
    } else if (sub == "select") {
        a->selected_ = a->selectedNode();
        a->compose();
        a->setStatus(a->selected_);
    } else if (sub == "activate") {
        if (Node *n = nodeOf(arg(2).c_str()); n && !n->isDir)
            openPath(n->path());
    } else if (sub == "sort") {
        const int c = num(2);
        a->sortDesc_ = c == a->sortCol_ ? !a->sortDesc_ : c != C_NAME;
        a->sortCol_ = c;
        a->resort();
    } else if (sub == "resize") {
        a->scheduleRender(150);
    } else if (sub == "hover") {
        Node *n = a->hit(num(2), num(3));
        if (n != a->hover_) {
            a->hover_ = n;
            a->setStatus(n ? n : a->selected_);
        }
    } else if (sub == "click") {
        if (Node *n = a->hit(num(2), num(3)))
            a->selectNode(n);
    } else if (sub == "zoomat") {
        Node *n = a->hit(num(2), num(3));
        while (n && n->parent != a->tmRoot_)
            n = n->parent;
        a->zoomTo(n);
    } else if (sub == "tmmenu") {
        if (Node *n = a->hit(num(2), num(3))) {
            a->selectNode(n);
            a->popupMenu(n, num(4), num(5));
        }
    } else if (sub == "treemenu") {
        if (Node *n = nodeOf(arg(2).c_str()))
            a->popupMenu(n, num(3), num(4));
    } else if (sub == "nact") {
        a->nodeAction(arg(2));
    } else if (sub == "act") {
        a->action(arg(2));
    } else if (sub == "extselect") {
        a->call({S(".e"), S("selection")});
        const std::string r = a->result();
        a->highlight_ = r.size() > 1 && r[0] == 'e' ? std::atoi(r.c_str() + 1) : -1;
        a->compose();
    } else if (sub == "extsort") {
        const int c = num(2);
        a->extSortDesc_ = c == a->extSortCol_ ? !a->extSortDesc_ : c >= 2;
        a->extSortCol_ = c;
        a->fillExt();
        a->compose();
    } else if (sub == "drives") {
        Tcl_Obj *list = Tcl_NewListObj(0, nullptr);
        auto row = [&](const std::vector<std::string> &v) {
            Tcl_Obj *r = Tcl_NewListObj(0, nullptr);
            for (const auto &s : v)
                Tcl_ListObjAppendElement(nullptr, r, S(s));
            Tcl_ListObjAppendElement(nullptr, list, r);
        };
        const std::string home = homeDir();
        row({"Ev klasörü  (" + home + ")", home, "", "", "", ""});
        for (const Drive &d : listDrives()) {
            const double f = double(d.used) / double(d.total);
            row({d.mp, d.mp, d.fs, fmtSize(d.used), fmtSize(d.total), textBar(f) + " " + fmtPct(f)});
        }
        Tcl_SetObjResult(in, list);
    } else if (sub == "home") {
        Tcl_SetObjResult(in, S(homeDir()));
    } else if (sub == "scan") {
        a->startScan(arg(2));
    } else {
        Tcl_SetObjResult(in, S("bilinmeyen alt komut: " + sub));
        return TCL_ERROR;
    }
    return TCL_OK;
}

}  // namespace

int main(int argc, char **argv)
{
    if (int rc = runCommandLine(argc, argv); rc >= 0)
        return rc;
    const std::string path = pathArgument(argc, argv);
    Tcl_FindExecutable(argv[0]);
    Tcl_Interp *in = Tcl_CreateInterp();
    if (Tcl_Init(in) != TCL_OK || Tk_Init(in) != TCL_OK) {
        std::fprintf(stderr, "Tcl/Tk başlatılamadı: %s\n", Tcl_GetStringResult(in));
        return 1;
    }
    auto *app = new TkApp(in);
    Tcl_CreateObjCommand(in, "dt", TkApp::command, app, nullptr);
    if (Tcl_EvalEx(in, UI_SCRIPT, -1, TCL_EVAL_GLOBAL) != TCL_OK) {
        std::fprintf(stderr, "Arayüz betiği hatası: %s\n%s\n", Tcl_GetStringResult(in),
                     Tcl_GetVar(in, "errorInfo", TCL_GLOBAL_ONLY));
        return 1;
    }
    app->applySettings();
    if (!path.empty())
        app->startScan(path);
    else
        app->showStartDialog();
    Tk_MainLoop();
    delete app;
    Tcl_DeleteInterp(in);
    return 0;
}
