// TuxDirStat — Tk arayüzü. Yerleşim gömülü Tcl betiğinde, veri işlemleri C++'ta (`dt` komutu).
// Kullanım: tuxdirstat-tk [klasör]

#include <sys/stat.h>
#include <unistd.h>
#include <tcl.h>
#include <tk.h>
#include <gio/gio.h>

#include <algorithm>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "extinfo.h"
#include "i18n.h"
#include "tools.h"
#include "report.h"
#include "session.h"
#include "treemap_render.h"

namespace {

#if TCL_MAJOR_VERSION >= 9
using TclSize = Tcl_Size;
#else
using TclSize = int;
#endif

const char *const UI_SCRIPT = R"TCL(
proc T {s} { return [dt tr $s] }
ttk::style theme use clam
ttk::style configure Treeview -rowheight 22
ttk::style configure TButton -width -4
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
set ::lang system
set ::age 0
set ::sunburst 0
set ::openlast 1
set ::record 0
set ::remote ""
set ::asadmin 0
set ::showtm 1
set ::statusL ""
set ::statusR ""
set ::zoomText [T "Treemap"]
set ::query ""

# Araç çubuğu
ttk::frame .tb -padding {4 3}
ttk::button .tb.open -text [T "Open…"] -command {dt act open}
ttk::button .tb.rescan -text [T "Rescan"] -command {dt act rescan}
ttk::button .tb.stop -text [T "Stop"] -command {dt act stop} -state disabled
ttk::separator .tb.s1 -orient vertical
ttk::button .tb.zin -text [T "Zoom in"] -command {dt act zoomin}
ttk::button .tb.zout -text [T "Zoom out"] -command {dt act zoomout} -state disabled
ttk::button .tb.zroot -text [T "Root"] -command {dt act zoomroot}
ttk::separator .tb.s2 -orient vertical
ttk::checkbutton .tb.app -text [T "Apparent size"] -variable ::apparent -command {dt act apparent}
ttk::checkbutton .tb.onefs -text [T "One file system"] -variable ::onefs -command {dt act onefs}
ttk::checkbutton .tb.group -text [T "Group files"] -variable ::group -command {dt act group}
ttk::menubutton .tb.file -text [T "File"] -menu .tb.file.m
menu .tb.file.m
.tb.file.m add command -label [T "Open snapshot…"] -command {dt act opensnap}
.tb.file.m add command -label [T "Save snapshot…"] -command {dt act savesnap} -accelerator Ctrl+S
.tb.file.m add separator
.tb.file.m add command -label [T "Export HTML report…"] -command {dt act html}
.tb.file.m add command -label [T "Save map as image…"] -command {dt act png}
.tb.file.m add command -label [T "Export as CSV…"] -command {dt act csv}
ttk::menubutton .tb.tools -text [T "Tools"] -menu .tb.tools.m
menu .tb.tools.m
.tb.tools.m add command -label [T "Find files…"] -command {dt act find} -accelerator Ctrl+F
.tb.tools.m add command -label [T "Duplicate files…"] -command {dt act dup}
.tb.tools.m add command -label [T "Cleanup suggestions…"] -command {dt act cleanup}
.tb.tools.m add separator
.tb.tools.m add command -label [T "Usage by owner"] -command {dt act owners}
.tb.tools.m add command -label [T "Files no package owns…"] -command {dt act unowned}
.tb.tools.m add command -label [T "Installed packages…"] -command {dt act packages}
.tb.tools.m add command -label [T "Btrfs: shared and compressed data…"] -command {dt act btrfs}
.tb.tools.m add separator
.tb.tools.m add command -label [T "Compare with snapshot…"] -command {dt act compare}
.tb.tools.m add command -label [T "Size history…"] -command {dt act history}
.tb.tools.m add checkbutton -label [T "Record size history weekly"] -variable ::record -command {dt act record}
ttk::menubutton .tb.theme -text [T "View"] -menu .tb.theme.m
menu .tb.theme.m
.tb.theme.m add checkbutton -label [T "Treemap"] -variable ::showtm -command toggleTreemap -accelerator F9
.tb.theme.m add checkbutton -label [T "Sunburst"] -variable ::sunburst -command {dt act sunburst} -accelerator F7
.tb.theme.m add checkbutton -label [T "Color by age"] -variable ::age -command {dt act age} -accelerator F8
.tb.theme.m add checkbutton -label [T "Reopen the last scan at start"] -variable ::openlast -command {dt act openlast}
menu .tb.theme.m.gmin
foreach n {8 16 32 64 128} {
    .tb.theme.m.gmin add radiobutton -label [format [T "At least %s items"] $n] -variable ::groupmin -value $n -command {dt act groupmin}
}
.tb.theme.m add cascade -label [T "Grouping threshold"] -menu .tb.theme.m.gmin
.tb.theme.m add separator
.tb.theme.m add command -label [T "Theme"] -state disabled
foreach {id label} [list system [T "System"] light [T "Light"] dark [T "Dark"]] {
    .tb.theme.m add radiobutton -label $label -variable ::theme -value $id -command {dt act theme}
}
.tb.theme.m add separator
.tb.theme.m add command -label [T "Language"] -state disabled
foreach {id label} [concat [list system [T "System"]] [dt languages]] {
    .tb.theme.m add radiobutton -label $label -variable ::lang -value $id -command {dt act lang}
}
ttk::button .tb.about -text [T "About"] -command {dt act about}
# Menüler önce yerleşir: pencere daralınca sağdaki menüler değil soldaki seçenekler kısalır.
pack .tb.about .tb.theme .tb.tools .tb.file -side right -padx 2
pack .tb.open .tb.rescan .tb.stop -side left -padx 1
pack .tb.s1 -side left -fill y -padx 4
pack .tb.zin .tb.zout .tb.zroot -side left -padx 1
pack .tb.s2 -side left -fill y -padx 4
pack .tb.app .tb.onefs .tb.group -side left -padx 4

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
.t heading #0 -text [T "Name"] -anchor w -command {dt sort 0}
set i 1
foreach {col title w} [list bar [T "Subtree"] 120 pct "%" 70 size [T "Size"] 90 items [T "Items"] 80 files [T "Files"] 80 dirs [T "Folders"] 70 mtime [T "Modified"] 140] {
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
.e heading #0 -text [T "Extension"] -anchor w -command {dt extsort 0}
.e heading desc -text [T "Type"] -anchor w -command {dt extsort 1}
.e heading size -text [T "Size"] -anchor e -command {dt extsort 2}
.e heading pct -text "%" -anchor e -command {dt extsort 2}
.e heading count -text [T "Files"] -anchor e -command {dt extsort 3}
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
ttk::frame .tmf.top
ttk::label .tmf.z -textvariable ::zoomText -padding {6 2}
ttk::frame .tmf.legend
pack .tmf.z -in .tmf.top -side left -fill x -expand 1
canvas .tmf.c -background #1e1e1e -highlightthickness 0 -height 80
.tmf.c create image 0 0 -anchor nw -image tmimg
pack .tmf.top -fill x
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

# Yaş renk açıklaması: dt legend {renk etiket ...} ile doldurulur.
proc buildLegend {items} {
    foreach w [winfo children .tmf.legend] { destroy $w }
    ttk::label .tmf.legend.t -text [T "Last change:"]
    pack .tmf.legend.t -side left -padx {0 4}
    set i 0
    foreach {color label} $items {
        label .tmf.legend.c$i -text "■" -foreground $color -padx 0
        ttk::label .tmf.legend.l$i -text $label
        pack .tmf.legend.c$i .tmf.legend.l$i -side left
        incr i
    }
}

proc showLegend {on} {
    if {$on} { pack .tmf.legend -in .tmf.top -side right -padx 6 } else { pack forget .tmf.legend }
}

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
    ttk::style configure TButton -background $c(btn) -padding {8 3} -width -4
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
    ttk::style configure Dim.TLabel -foreground $c(dis)
    ttk::style configure Title.TLabel -font {TkDefaultFont 13 bold}
    .t tag configure mount -foreground $c(dis)
    foreach m {.tb.theme.m .tb.theme.m.gmin .tb.file.m .tb.tools.m} { $m configure -background $c(btn) -foreground $c(fg) -activebackground $c(sel) -activeforeground #ffffff -selectcolor $c(sel) }
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
bind . <Control-f> {dt act find}
bind . <Control-s> {dt act savesnap}
bind . <F8> {set ::age [expr {!$::age}]; dt act age}
bind . <F7> {set ::sunburst [expr {!$::sunburst}]; dt act sunburst}
bind . <F9> {set ::showtm [expr {!$::showtm}]; toggleTreemap}
bind . <Control-g> {set ::group [expr {!$::group}]; dt act group}

proc showMenu {X Y isDir isMount isGroup hasParent busy pacman local} {
    destroy .ctx
    menu .ctx
    set st [expr {$busy ? "disabled" : "normal"}]
    if {$local} {
        .ctx add command -label [T "Open"] -command {dt nact open}
        .ctx add command -label [T "Show in file manager"] -command {dt nact reveal}
    }
    .ctx add command -label [T "Copy path"] -command {dt nact copy}
    if {$local && $pacman && !$isGroup} {
        .ctx add command -label [T "Which package owns this?"] -command {dt nact package}
    }
    if {$isDir} {
        .ctx add separator
        .ctx add command -label [T "Zoom into in treemap"] -command {dt nact zoom}
        set lbl [expr {$isGroup ? [T "Rescan its folder"] : $isMount ? [T "Scan this file system too"] : [T "Rescan this folder"]}]
        .ctx add command -label $lbl -command {dt nact refresh} -state $st
    }
    if {$hasParent && $local} {
        .ctx add separator
        .ctx add command -label [T "Move to trash"] -command {dt nact trash} -state $st
        .ctx add command -label [T "Delete permanently…"] -command {dt nact delete} -state $st
    }
    tk_popup .ctx $X $Y
}

proc confirm {title detail} {
    set r [tk_messageBox -parent [focus] -icon warning -type yesno -default no -title TuxDirStat -message $title -detail $detail]
    return [expr {$r eq "yes"}]
}

proc showError {title detail} {
    set p [focus]
    if {$p eq ""} { set p . }
    tk_messageBox -parent [winfo toplevel $p] -icon warning -type ok -title TuxDirStat -message $title -detail $detail
}

proc showInfo {detail} {
    tk_messageBox -parent . -icon info -type ok -title TuxDirStat -message TuxDirStat -detail $detail
}

proc startDialog {} {
    set w .start
    destroy $w
    toplevel $w
    wm title $w "TuxDirStat — [T {Location to scan}]"
    wm transient $w .
    wm geometry $w 860x440
    ttk::frame $w.f -padding 10
    pack $w.f -fill both -expand 1
    ttk::label $w.f.l -text [T "Choose what to scan:"]
    ttk::treeview $w.f.t -columns {fs used free total pct} -show {tree headings} -selectmode browse
    $w.f.t heading #0 -text [T "Location"] -anchor w
    $w.f.t heading fs -text [T "Type"] -anchor w
    $w.f.t heading used -text [T "Used"] -anchor e
    $w.f.t heading free -text [T "Free"] -anchor e
    $w.f.t heading total -text [T "Total"] -anchor e
    $w.f.t heading pct -text [T "Usage"] -anchor w
    $w.f.t column #0 -width 280
    $w.f.t column fs -width 80 -stretch 0
    $w.f.t column used -width 90 -anchor e -stretch 0
    $w.f.t column free -width 90 -anchor e -stretch 0
    $w.f.t column total -width 90 -anchor e -stretch 0
    $w.f.t column pct -width 150 -stretch 0
    foreach d [dt drives] {
        lassign $d label path fs used free total pct
        $w.f.t insert {} end -text $label -values [list $fs $used $free $total $pct] -tags [list $path]
    }
    set first [lindex [$w.f.t children {}] 0]
    $w.f.t selection set $first
    $w.f.t focus $first
    ttk::frame $w.f.r
    ttk::label $w.f.r.l -text [T "Other computer (SSH):"]
    ttk::entry $w.f.r.e -textvariable ::remote
    pack $w.f.r.l -side left
    pack $w.f.r.e -side left -fill x -expand 1 -padx {8 0}
    ttk::checkbutton $w.f.admin -text [T "Scan as administrator (read-only, asks for your password)"] -variable ::asadmin
    ttk::frame $w.f.b
    ttk::button $w.f.b.pick -text [T "Choose folder…"] -command {
        set d [tk_chooseDirectory -parent .start -title [T "Choose folder"] -initialdir [dt home] -mustexist 1]
        if {$d ne ""} { destroy .start; dt scan $d $::asadmin }
    }
    ttk::button $w.f.b.cancel -text [T "Cancel"] -command [list destroy $w]
    ttk::button $w.f.b.ok -text [T "Scan"] -command startDialogAccept
    pack $w.f.b.pick -side left
    pack $w.f.b.ok $w.f.b.cancel -side right -padx 2
    pack $w.f.l -anchor w -pady {0 6}
    pack $w.f.b -side bottom -fill x -pady {8 0}
    pack $w.f.admin -side bottom -anchor w -pady {6 0}
    pack $w.f.r -side bottom -fill x -pady {6 0}
    pack $w.f.t -fill both -expand 1
    bind $w.f.t <Double-1> startDialogAccept
    bind $w <Return> startDialogAccept
    bind $w <Escape> [list destroy $w]
    focus $w.f.t
}

proc startDialogAccept {} {
    set r [string trim $::remote]
    if {$r ne ""} {
        if {![dt isremote $r]} {
            showError [T "Write the remote location as host:/path, e.g. pi:/home"] ""
            return
        }
        destroy .start
        dt scan $r 0
        return
    }
    set sel [.start.f.t selection]
    if {$sel eq ""} return
    set path [lindex [.start.f.t item $sel -tags] 0]
    destroy .start
    dt scan $path $::asadmin
}

# Araç sonuç penceresi. rows: {kimlik üst metinler ipucu kalın} listesi; widths: sütun
# genişlikleri (piksel), stretch: pencereyle birlikte esneyen sütun.
proc showTable {title summary empty columns numeric widths stretch rows} {
    set w .res
    destroy $w
    toplevel $w
    wm title $w "TuxDirStat — $title"
    wm transient $w .
    wm geometry $w 980x560
    ttk::frame $w.f -padding 10
    pack $w.f -fill both -expand 1
    ttk::label $w.f.title -text $title -style Title.TLabel
    pack $w.f.title -anchor w
    if {$summary ne ""} {
        ttk::label $w.f.sum -text $summary -style Dim.TLabel -justify left -wraplength 940
        pack $w.f.sum -anchor w -pady {2 6}
    }
    ttk::frame $w.f.b
    pack $w.f.b -side bottom -fill x -pady {8 0}
    ttk::button $w.f.b.close -text [T "Close"] -command [list destroy $w]
    pack $w.f.b.close -side right
    if {[llength $rows] == 0} {
        ttk::label $w.f.empty -text $empty -style Dim.TLabel -anchor center
        pack $w.f.empty -fill both -expand 1
    } else {
        set cols {}
        for {set i 1} {$i < [llength $columns]} {incr i} { lappend cols c$i }
        ttk::frame $w.f.lf
        ttk::treeview $w.f.t -columns $cols -selectmode extended
        $w.f.t heading #0 -text [lindex $columns 0] -anchor w
        $w.f.t column #0 -width [lindex $widths 0] -stretch [expr {$stretch == 0}]
        for {set i 1} {$i < [llength $columns]} {incr i} {
            set a [expr {[lindex $numeric $i] ? "e" : "w"}]
            $w.f.t heading c$i -text [lindex $columns $i] -anchor $a
            $w.f.t column c$i -anchor $a -width [lindex $widths $i] -stretch [expr {$stretch == $i}]
        }
        $w.f.t tag configure bold -font {TkDefaultFont 10 bold}
        foreach r $rows {
            lassign $r id parent texts tip bold
            $w.f.t insert $parent end -id $id -text [lindex $texts 0] -values [lrange $texts 1 end] -open 1 -tags [expr {$bold ? "bold" : ""}]
        }
        ttk::scrollbar $w.f.lf.ys -orient vertical -command [list $w.f.t yview]
        $w.f.t configure -yscrollcommand [list $w.f.lf.ys set]
        grid $w.f.t -in $w.f.lf -row 0 -column 0 -sticky nsew
        grid $w.f.lf.ys -row 0 -column 1 -sticky ns
        grid rowconfigure $w.f.lf 0 -weight 1
        grid columnconfigure $w.f.lf 0 -weight 1
        pack $w.f.lf -fill both -expand 1
        foreach {b label cmd} [list show [T "Show in tree"] show reveal [T "Open folder"] reveal copy [T "Copy path"] copy trash [T "Move to trash"] trash] {
            ttk::button $w.f.b.$b -text $label -command [list dt res $cmd]
            pack $w.f.b.$b -side left -padx {0 4}
        }
        bind $w.f.t <<TreeviewSelect>> {dt res sel}
        bind $w.f.t <Double-1> {dt res show}
        dt res sel
    }
    bind $w <Escape> [list destroy $w]
    update idletasks
    catch {grab $w}
}

proc resSelection {} { return [.res.f.t selection] }

proc jobDialog {title} {
    set w .job
    destroy $w
    toplevel $w
    wm title $w "TuxDirStat — $title"
    wm transient $w .
    wm protocol $w WM_DELETE_WINDOW {dt job cancel}
    ttk::frame $w.f -padding 12
    pack $w.f -fill both -expand 1
    ttk::label $w.f.l -text $title -width 70
    ttk::progressbar $w.f.p -mode indeterminate -length 480 -maximum 1000
    ttk::button $w.f.c -text [T "Cancel"] -command {dt job cancel}
    pack $w.f.l -anchor w
    pack $w.f.p -fill x -pady 8
    pack $w.f.c -anchor e
    $w.f.p start 10
    update idletasks
    catch {grab $w}
}

proc findDialog {help} {
    set w .find
    destroy $w
    toplevel $w
    wm title $w "TuxDirStat — [T {Find files}]"
    wm transient $w .
    ttk::frame $w.f -padding 10
    pack $w.f -fill both -expand 1
    ttk::entry $w.f.e -textvariable ::query -width 60
    label $w.f.h -text $help -justify left -font TkFixedFont -anchor w
    ttk::frame $w.f.b
    ttk::button $w.f.b.ok -text [T "Find"] -command {dt find $::query}
    ttk::button $w.f.b.cancel -text [T "Cancel"] -command [list destroy $w]
    pack $w.f.b.ok $w.f.b.cancel -side right -padx 2
    pack $w.f.e -fill x
    pack $w.f.h -fill x -pady 8
    pack $w.f.b -fill x
    bind $w.f.e <Return> {dt find $::query}
    bind $w <Escape> [list destroy $w]
    focus $w.f.e
    update idletasks
    catch {grab $w}
}
)TCL";

enum Col { C_NAME, C_BAR, C_PCT, C_SIZE, C_ITEMS, C_FILES, C_DIRS, C_MTIME, NCOLS };
const char *const HEADINGS[NCOLS] = {N_("Name"), N_("Subtree"), "%", N_("Size"), N_("Items"), N_("Files"), N_("Folders"), N_("Modified")};
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
    void startPrivilegedScan(const std::string &path);
    void openSnapshot(const std::string &file);
    void showStartDialog() { eval("startDialog"); }
    void openLast()
    {
        openedLast_ = true;
        openSnapshot(lastScanFile());
    }

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
    void beginBusy(const std::string &title);
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
    bool removeNow(Node *n, bool permanent, std::string *err);

    // araçlar
    bool toolReady();
    void showTable(ResultTable t);
    void resAction(const std::string &what);
    std::vector<int> resSelected();
    void runTool(const char *title, std::function<void(Job &)> fn, bool needsTree = true);
    bool local() const { return isLocalTree(s_.root()); }
    bool openedLast_ = false;
    static void jobTimer(ClientData cd);
    void jobPoll();
    // kind: 0 snapshot, 1 CSV, 2 HTML, 3 PNG
    std::string chooseFile(bool save, const std::string &title, const std::string &initial, int kind);
    bool colorByAge() { return varBool("::age"); }
    ResultTable table_;
    std::vector<bool> gone_;
    std::unique_ptr<Job> job_;
    Tcl_TimerToken jobToken_ = nullptr;

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
    openedLast_ = false;
    std::string host, rpath;
    char *real = parseRemote(path, host, rpath) ? nullptr : realpath(path.c_str(), nullptr);
    const std::string abs = real ? real : path;
    std::free(real);
    s_.oneFs = varBool("::onefs");
    s_.startScan(abs, target);
    beginBusy(target ? std::string() : abs);
}

void TkApp::startPrivilegedScan(const std::string &path)
{
    char *real = realpath(path.c_str(), nullptr);
    const std::string abs = real ? real : path;
    std::free(real);
    s_.oneFs = varBool("::onefs");
    s_.startPrivilegedScan(abs);
    beginBusy(abs);
}

void TkApp::openSnapshot(const std::string &file)
{
    char *real = realpath(file.c_str(), nullptr);
    const std::string abs = real ? real : file;
    std::free(real);
    s_.openSnapshot(abs);
    beginBusy(abs.substr(abs.rfind('/') + 1));
}

void TkApp::beginBusy(const std::string &title)
{
    setState(".tb.stop", true);
    eval("pack .sb.p -side right -padx 6; .sb.p start 10");
    if (!title.empty())
        call({S("wm"), S("title"), S("."), S("TuxDirStat — " + title)});
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
        setVar("::statusL", s_.progressText());
        pollToken_ = Tcl_CreateTimerHandler(100, pollTimer, this);
        return;
    }
    eval(".sb.p stop; pack forget .sb.p");
    setState(".tb.stop", false);
    switch (res) {
    case Session::Result::Failed:
        setVar("::statusL", "");
        call({S("showError"), S(T("The scan failed:")), S(s_.error())});
        if (s_.root())
            call({S("wm"), S("title"), S("."), S("TuxDirStat — " + s_.root()->path())});
        return;
    case Session::Result::Cancelled:
        setVar("::statusL", T("Scan stopped."));
        return;
    case Session::Result::NewTree: {
        Node *root = s_.pendingRoot();
        tm_.forgetNodes();
        tmRoot_ = root;
        selected_ = hover_ = menuNode_ = nullptr;
        s_.commitNewTree();
        call({S("wm"), S("title"), S("."), S("TuxDirStat — " + root->path())});
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
    setVar("::record", local() && historyRecording(s_.root()->path()) ? "1" : "0");
    std::string done = TF("Done: %1 items in %2 s.", {fmtInt(s_.lastCount()), fmtDecimal(s_.elapsed(), 2)});
    if (openedLast_) {
        openedLast_ = false;
        struct stat sb;
        if (::stat(lastScanFile().c_str(), &sb) == 0)
            done = TF("Showing the saved scan from %1. Press F5 to scan again.", {fmtTime(sb.st_mtime)});
    }
    setVar("::statusL", done);
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
        std::string t = T(HEADINGS[c]);
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
    setVar("::statusR", TF("Total: %1 · %2 files · %3 folders · Free: %4",
                           {fmtSize(root->sizeOf(s_.apparent)), fmtInt(root->files), fmtInt(root->dirs),
                            fmtSize(freeSpace(root->path()))}));
}

void TkApp::updateZoom()
{
    setVar("::zoomText", tmRoot_ ? std::string(T("Treemap")) + ": " + tmRoot_->path() : std::string(T("Treemap")));
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
    tm_.setColorMode(colorByAge() ? ColorMode::Age : ColorMode::Extension);
    tm_.setShape(varBool("::sunburst") ? MapShape::Rings : MapShape::Rectangles);
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
    } else if (a == "age") {
        tm_.forgetNodes();
        scheduleRender(0);
        call({S("showLegend"), S(colorByAge() ? "1" : "0")});
        saveSettings();
    } else if (a == "sunburst") {
        tm_.forgetNodes();
        scheduleRender(0);
        saveSettings();
    } else if (a == "openlast") {
        if (!varBool("::openlast"))
            ::unlink(lastScanFile().c_str());
        saveSettings();
    } else if (a == "record") {
        if (!local()) {
            setVar("::record", "0");
        } else {
            const bool on = varBool("::record");
            const std::string path = s_.root()->path();
            std::string err;
            bool ok = setHistoryRecording(path, on, &err);
            if (ok && on)
                ok = recordHistorySnapshot(s_.root(), &err);
            if (!ok)
                call({S("showError"), S(err), S("")});
            else
                setVar("::statusL", on ? TF("The size of %1 is now recorded every week.", {path}) : TF("Stopped recording %1.", {path}));
            setVar("::record", historyRecording(path) ? "1" : "0");
        }
    } else if (a == "packages") {
        runTool(T("Installed packages"), [](Job &j) { j.result = packageSizes(j); }, false);
    } else if (a == "btrfs") {
        Node *root = s_.root();
        runTool(T("Btrfs: shared and compressed data"), [root](Job &j) { j.result = btrfsUsage(root, j); });
    } else if (a == "history") {
        Node *root = s_.root();
        const bool ap = s_.apparent;
        runTool(T("Size history"), [root, ap](Job &j) { j.result = historyGrowth(root, ap, j); });
    } else if (a == "html") {
        if (toolReady()) {
            std::string name = s_.root()->path();
            name = name.substr(name.rfind('/') + 1);
            const std::string f = chooseFile(true, T("Export HTML report"), (name.empty() ? "root" : name) + "-report.html", 2);
            std::string err;
            if (!f.empty()) {
                if (!exportHtml(s_.root(), f, s_.apparent, &err))
                    call({S("showError"), S(T("Could not write the file:")), S(err)});
                else
                    setVar("::statusL", TF("Exported: %1", {f}));
            }
        }
    } else if (a == "png") {
        if (toolReady()) {
            const std::string f = chooseFile(true, T("Save map as image"), "tuxdirstat.png", 3);
            const bool ring = varBool("::sunburst");
            std::string err;
            if (!f.empty()) {
                if (!exportMapPng(tmRoot_ ? tmRoot_ : s_.root(), f, 1600, ring ? 1600 : 1000, s_.apparent,
                                  colorByAge() ? ColorMode::Age : ColorMode::Extension,
                                  ring ? MapShape::Rings : MapShape::Rectangles, &err))
                    call({S("showError"), S(T("Could not write the file:")), S(err)});
                else
                    setVar("::statusL", TF("Exported: %1", {f}));
            }
        }
    } else if (a == "lang") {
        saveSettings();
        call({S("showInfo"), S(T("The new language is used after TuxDirStat is restarted."))});
    } else if (a == "opensnap") {
        if (!s_.scanning()) {
            const std::string f = chooseFile(false, T("Open snapshot"), "", false);
            if (!f.empty())
                openSnapshot(f);
        }
    } else if (a == "savesnap") {
        if (toolReady()) {
            std::string name = s_.root()->path();
            name = name.substr(name.rfind('/') + 1);
            char day[16];
            time_t now = time(nullptr);
            struct tm tm;
            localtime_r(&now, &tm);
            std::strftime(day, sizeof day, "%Y-%m-%d", &tm);
            std::string f = chooseFile(true, T("Save snapshot"), (name.empty() ? "root" : name) + "-" + day + ".json", false);
            std::string err;
            if (!f.empty()) {
                if (f.find('.', f.rfind('/') + 1) == std::string::npos)
                    f += ".json";
                if (!saveSnapshot(s_.root(), f, &err))
                    call({S("showError"), S(T("Could not save the snapshot:")), S(err)});
                else
                    setVar("::statusL", TF("Snapshot saved: %1", {f}));
            }
        }
    } else if (a == "csv") {
        if (toolReady()) {
            const std::string f = chooseFile(true, T("Export as CSV"), "tuxdirstat.csv", true);
            std::string err;
            if (!f.empty()) {
                if (!exportCsv(s_.root(), f, &err))
                    call({S("showError"), S(T("Could not write the file:")), S(err)});
                else
                    setVar("::statusL", TF("Exported: %1", {f}));
            }
        }
    } else if (a == "find") {
        if (toolReady())
            call({S("findDialog"), S(searchHelp())});
    } else if (a == "dup") {
        Node *root = s_.root();
        runTool(T("Duplicate files"), [root](Job &j) { j.result = findDuplicates(root, j); });
    } else if (a == "cleanup") {
        Node *root = s_.root();
        const bool ap = s_.apparent;
        runTool(T("Cleanup suggestions"), [root, ap](Job &j) { j.result = findCleanup(root, ap, j); });
    } else if (a == "owners") {
        if (toolReady())
            showTable(ownerUsage(s_.root(), s_.apparent));
    } else if (a == "unowned") {
        if (!havePacman()) {
            call({S("showError"), S(T("pacman was not found; --unowned works on Arch-based systems.")), S("")});
        } else {
            Node *root = s_.root();
            const bool ap = s_.apparent;
            runTool(T("Files no package owns"), [root, ap](Job &j) { j.result = findUnowned(root, ap, j); });
        }
    } else if (a == "compare") {
        if (toolReady()) {
            const std::string f = chooseFile(false, T("Compare with snapshot"), "", false);
            if (!f.empty()) {
                Node *root = s_.root();
                const bool ap = s_.apparent;
                runTool(T("Compare with snapshot"), [root, ap, f](Job &j) { j.result = compareWithSnapshot(root, f, ap, j); });
            }
        }
    } else if (a == "about") {
        call({S("tk_messageBox"), S("-parent"), S("."), S("-icon"), S("info"), S("-title"), S("TuxDirStat"), S("-message"),
              S("TuxDirStat " TUXDIRSTAT_VERSION), S("-detail"),
              S(std::string(T("Disk usage analyzer for Linux.")) + "\n\n" +
                T("Click an item in the tree or the treemap. Double-click in the treemap to zoom into a "
                  "folder, Backspace goes back. Selecting a row in the extension list marks those files "
                  "in the treemap.") +
                "\n\n" + T("License: GPL-3.0-or-later") + "\nhttps://github.com/Gubir34/TuxDirStat")});
    } else if (a == "quit") {
        if (varBool("::openlast") && !s_.scanning() && local()) {
            setVar("::statusL", T("Saving the scan for next time…"));
            eval("update idletasks");
            std::string err;
            saveLastScan(s_.root(), &err);
        }
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
    if (a == "open" && local())
        openPath(n->path());
    else if (a == "reveal" && local())
        revealPath(n->path());
    else if (a == "copy")
        copyPath(n);
    else if (a == "zoom")
        zoomTo(n);
    else if (a == "package")
        call({S("showInfo"), S(packageOwnerMessage(n->path()))});
    else if (a == "refresh")
        refreshNode(n);
    else if (a == "trash" || a == "delete")
        deleteNode(n, a == "delete");
}

void TkApp::popupMenu(Node *n, int X, int Y)
{
    menuNode_ = n;
    call({S("showMenu"), Tcl_NewIntObj(X), Tcl_NewIntObj(Y), Tcl_NewIntObj(n->isDir), Tcl_NewIntObj((n->flags & F_MOUNT) != 0),
          Tcl_NewIntObj((n->flags & F_GROUP) != 0), Tcl_NewIntObj(n->parent != nullptr), Tcl_NewIntObj(s_.scanning()),
          Tcl_NewIntObj(havePacman()), Tcl_NewIntObj(local())});
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
    if (!local()) {
        call({S("showError"), S(T("This scan is from another computer or a snapshot of one; its files cannot be deleted from here.")), S("")});
        return;
    }
    const std::string path = n->path();
    const std::string kind = (n->flags & F_GROUP) ? TF("the %1 files in this group", {fmtInt(n->files)})
                             : n->isDir           ? T("this folder")
                                                  : T("this file");
    const std::string title = permanent ? TF("Delete %1 permanently?", {kind}) : TF("Move %1 to the trash?", {kind});
    const std::string detail = path + "\n(" + fmtSize(n->sizeOf(s_.apparent)) + ")" +
                               (permanent ? std::string("\n\n") + T("This cannot be undone.") : "");
    if (call({S("confirm"), S(title), S(detail)}) != TCL_OK || result() != "1")
        return;
    std::string err;
    if (!removeNow(n, permanent, &err))
        call({S("showError"), S(T("Could not delete (or only partly deleted):")), S(err)});
}

bool TkApp::removeNow(Node *n, bool permanent, std::string *errOut)
{
    if (!local())
        return false;
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
    return true;
}

// ───────────── araçlar ─────────────

bool TkApp::toolReady()
{
    if (s_.scanning()) {
        call({S("showInfo"), S(T("Wait until the scan has finished."))});
        return false;
    }
    if (!s_.root()) {
        call({S("showInfo"), S(T("Scan a folder first."))});
        return false;
    }
    return true;
}

std::string TkApp::chooseFile(bool save, const std::string &title, const std::string &initial, int kind)
{
    Tcl_Obj *types = Tcl_NewListObj(0, nullptr);
    auto addType = [&](const char *name, std::initializer_list<const char *> pats) {
        Tcl_Obj *t = Tcl_NewListObj(0, nullptr);
        Tcl_ListObjAppendElement(nullptr, t, S(name));
        Tcl_Obj *p = Tcl_NewListObj(0, nullptr);
        for (const char *x : pats)
            Tcl_ListObjAppendElement(nullptr, p, S(x));
        Tcl_ListObjAppendElement(nullptr, t, p);
        Tcl_ListObjAppendElement(nullptr, types, t);
    };
    if (kind == 1)
        addType(T("CSV files"), {".csv"});
    else if (kind == 2)
        addType(T("HTML files"), {".html"});
    else if (kind == 3)
        addType(T("PNG images"), {".png"});
    else
        addType(T("Snapshots (JSON)"), {".json", ".gz", ".zst", ".xz"});
    addType(T("All files"), {"*"});
    std::vector<Tcl_Obj *> args{S(save ? "tk_getSaveFile" : "tk_getOpenFile"), S("-parent"), S("."), S("-title"), S(title),
                                S("-initialdir"), S(homeDir()), S("-filetypes"), types};
    if (save && !initial.empty()) {
        args.push_back(S("-initialfile"));
        args.push_back(S(initial));
    }
    for (Tcl_Obj *o : args)
        Tcl_IncrRefCount(o);
    const int rc = Tcl_EvalObjv(in_, int(args.size()), args.data(), TCL_EVAL_GLOBAL);
    for (Tcl_Obj *o : args)
        Tcl_DecrRefCount(o);
    return rc == TCL_OK ? result() : std::string();
}

void TkApp::showTable(ResultTable t)
{
    table_ = std::move(t);
    gone_.assign(table_.rows.size(), false);
    auto list = [](const std::vector<std::string> &v) {
        Tcl_Obj *l = Tcl_NewListObj(0, nullptr);
        for (const std::string &x : v)
            Tcl_ListObjAppendElement(nullptr, l, S(x));
        return l;
    };
    std::vector<std::string> numeric, widths;
    // Genişlikler metin uzunluğundan kestirilir; en uzun metinli sütun esner.
    size_t stretch = 0, best = 0;
    for (size_t c = 0; c < table_.columns.size(); ++c) {
        const bool num = c < table_.numeric.size() && table_.numeric[c];
        numeric.push_back(num ? "1" : "0");
        size_t maxLen = table_.columns[c].size(), total = 0;
        for (const ResultRow &r : table_.rows)
            if (c < r.cells.size()) {
                maxLen = std::max(maxLen, r.cells[c].size());
                total += r.cells[c].size();
            }
        widths.push_back(std::to_string(std::clamp<size_t>(maxLen * 8 + 24, 70, 340)));
        if (!num && total > best) {
            best = total;
            stretch = c;
        }
    }
    widths[stretch] = "380";
    Tcl_Obj *rows = Tcl_NewListObj(0, nullptr);
    int lastGroup = -1;
    for (size_t i = 0; i < table_.rows.size(); ++i) {
        const ResultRow &r = table_.rows[i];
        std::string parent;
        if (table_.grouped) {
            parent = "g" + std::to_string(r.group);
            if (r.group != lastGroup) {
                int n = 0;
                for (const ResultRow &o : table_.rows)
                    n += o.group == r.group;
                std::vector<std::string> texts{TF("Set %1 · %2 × %3", {std::to_string(r.group), std::to_string(n), fmtSize(r.bytes)})};
                Tcl_Obj *g = Tcl_NewListObj(0, nullptr);
                for (Tcl_Obj *o : {S(parent), S(""), list(texts), S(""), S("1")})
                    Tcl_ListObjAppendElement(nullptr, g, o);
                Tcl_ListObjAppendElement(nullptr, rows, g);
                lastGroup = r.group;
            }
        }
        Tcl_Obj *row = Tcl_NewListObj(0, nullptr);
        for (Tcl_Obj *o : {S("r" + std::to_string(i)), S(parent), list(r.cells), S(r.command.empty() ? r.path : r.command), S("0")})
            Tcl_ListObjAppendElement(nullptr, row, o);
        Tcl_ListObjAppendElement(nullptr, rows, row);
    }
    call({S("showTable"), S(table_.title), S(table_.summary), S(table_.emptyText), list(table_.columns), list(numeric),
          list(widths), Tcl_NewIntObj(int(stretch)), rows});
}

std::vector<int> TkApp::resSelected()
{
    std::vector<int> v;
    if (call({S("resSelection")}) != TCL_OK)
        return v;
    Tcl_Obj *res = Tcl_GetObjResult(in_);
    TclSize n = 0;
    Tcl_Obj **items = nullptr;
    if (Tcl_ListObjGetElements(in_, res, &n, &items) != TCL_OK)
        return v;
    for (TclSize i = 0; i < n; ++i) {
        const char *id = Tcl_GetString(items[i]);
        if (id[0] == 'r') {
            const int r = std::atoi(id + 1);
            if (r >= 0 && size_t(r) < gone_.size() && !gone_[size_t(r)])
                v.push_back(r);
        }
    }
    return v;
}

void TkApp::resAction(const std::string &what)
{
    const std::vector<int> rows = resSelected();
    const ResultRow *one = rows.size() == 1 ? &table_.rows[size_t(rows[0])] : nullptr;
    Node *root = s_.root();
    if (what == "sel") {
        auto state = [&](const char *b, bool on) {
            call({S(std::string(".res.f.b.") + b), S("configure"), S("-state"), S(on ? "normal" : "disabled")});
        };
        state("show", one && one->node && root && one->node->isWithin(root));
        state("reveal", one && !one->path.empty() && one->path[0] == '/');
        state("copy", !rows.empty());
        call({S(".res.f.b.copy"), S("configure"), S("-text"), S(one && !one->command.empty() ? T("Copy command") : T("Copy path"))});
        bool del = !rows.empty();
        for (int r : rows)
            del = del && table_.rows[size_t(r)].deletable && table_.rows[size_t(r)].path.rfind('/', 0) == 0;
        state("trash", del);
    } else if (what == "show") {
        if (one && one->node && root && one->node->isWithin(root)) {
            Node *n = one->node;
            eval("destroy .res");
            selectNode(n);
        }
    } else if (what == "reveal") {
        if (one)
            revealPath(one->path);
    } else if (what == "copy") {
        std::string text;
        for (int r : rows) {
            const ResultRow &row = table_.rows[size_t(r)];
            text += (text.empty() ? "" : "\n") + (row.command.empty() ? row.path : row.command);
        }
        eval("clipboard clear");
        call({S("clipboard"), S("append"), S("--"), S(text)});
    } else if (what == "trash") {
        if (rows.empty())
            return;
        uint64_t bytes = 0;
        for (int r : rows)
            bytes += table_.rows[size_t(r)].bytes;
        if (call({S("confirm"), S(TF("Move %1 items (%2) to the trash?", {fmtInt(rows.size()), fmtSize(bytes)})),
                  S(deletionWarning(table_, rows))}) != TCL_OK || result() != "1")
            return;
        int failed = 0;
        std::string firstErr;
        for (int r : rows) {
            const ResultRow &row = table_.rows[size_t(r)];
            std::string err;
            bool ok;
            if (!g_file_test(row.path.c_str(), G_FILE_TEST_EXISTS) && !g_file_test(row.path.c_str(), G_FILE_TEST_IS_SYMLINK)) {
                ok = true;  // daha önce silinen bir klasörün içindeydi
            } else if (row.node && s_.root() && row.node->isWithin(s_.root())) {
                ok = removeNow(row.node, false, &err);
            } else {
                GFile *f = g_file_new_for_path(row.path.c_str());
                ok = g_file_trash(f, nullptr, nullptr) || trashWithCommand(row.path);
                g_object_unref(f);
                err = row.path;
            }
            if (ok) {
                gone_[size_t(r)] = true;
                call({S(".res.f.t"), S("delete"), S("r" + std::to_string(r))});
            } else if (!failed++) {
                firstErr = err;
            }
        }
        // Tek üyesi kalan kopya kümelerini kaldır.
        if (table_.grouped)
            eval("foreach g [.res.f.t children {}] { if {[llength [.res.f.t children $g]] < 2} { .res.f.t delete $g } }");
        resAction("sel");
        if (failed)
            call({S("showError"), S(TF("%1 items could not be moved to the trash.", {std::to_string(failed)})), S(firstErr)});
    }
}

void TkApp::runTool(const char *title, std::function<void(Job &)> fn, bool needsTree)
{
    if ((needsTree ? !toolReady() : s_.scanning()) || job_)
        return;
    job_ = std::make_unique<Job>(title, std::move(fn));
    call({S("jobDialog"), S(title)});
    job_->start();
    jobToken_ = Tcl_CreateTimerHandler(100, jobTimer, this);
}

void TkApp::jobTimer(ClientData cd)
{
    auto *a = static_cast<TkApp *>(cd);
    a->jobToken_ = nullptr;
    a->jobPoll();
}

void TkApp::jobPoll()
{
    if (!job_)
        return;
    const double f = job_->progress();
    if (f >= 0) {
        eval(".job.f.p stop; .job.f.p configure -mode determinate");
        call({S(".job.f.p"), S("configure"), S("-value"), Tcl_NewIntObj(int(f * 1000))});
    }
    const std::string st = job_->status();
    if (!st.empty())
        call({S(".job.f.l"), S("configure"), S("-text"), S(st)});
    if (!job_->done()) {
        jobToken_ = Tcl_CreateTimerHandler(100, jobTimer, this);
        return;
    }
    eval("destroy .job");
    std::unique_ptr<Job> j = std::move(job_);
    if (j->cancelled())
        return;
    if (!j->error.empty() && j->result.rows.empty())
        call({S("showError"), S(j->error), S("")});
    else
        showTable(std::move(j->result));
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
    setVar("::showtm", st.showTreemap ? "1" : "0");
    setVar("::theme", st.theme);
    setVar("::lang", st.language);
    setVar("::age", st.colorByAge ? "1" : "0");
    setVar("::sunburst", st.sunburst ? "1" : "0");
    setVar("::openlast", st.openLast ? "1" : "0");
    call({S("applyPalette"), S(wantsDark(st.theme) ? "1" : "0")});
    Tcl_Obj *legend = Tcl_NewListObj(0, nullptr);
    for (const AgeBucket &b : ageBuckets()) {
        Tcl_ListObjAppendElement(nullptr, legend, S(colorHex(b.color)));
        Tcl_ListObjAppendElement(nullptr, legend, S(T(b.label)));
    }
    call({S("buildLegend"), legend});
    call({S("showLegend"), S(st.colorByAge ? "1" : "0")});
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
    if (const char *l = Tcl_GetVar(in_, "::lang", TCL_GLOBAL_ONLY))
        st.language = l;
    st.colorByAge = varBool("::age");
    st.sunburst = varBool("::sunburst");
    st.openLast = varBool("::openlast");
    st.save();
}

// ───────────── Tcl komutu ─────────────

int TkApp::command(ClientData cd, Tcl_Interp *in, int objc, Tcl_Obj *const objv[])
{
    auto *a = static_cast<TkApp *>(cd);
    if (objc < 2) {
        Tcl_WrongNumArgs(in, 1, objv, "subcommand ?args?");
        return TCL_ERROR;
    }
    const std::string sub = Tcl_GetString(objv[1]);
    auto arg = [&](int i) -> std::string { return i < objc ? Tcl_GetString(objv[i]) : ""; };
    auto num = [&](int i) { return std::atoi(arg(i).c_str()); };

    if (sub == "tr") {
        Tcl_SetObjResult(in, S(T(Tcl_GetString(objv[2]))));
        return TCL_OK;
    }
    if (sub == "languages") {  // code name code name …
        Tcl_Obj *l = Tcl_NewListObj(0, nullptr);
        for (const i18n::Language &lang : i18n::languages()) {
            Tcl_ListObjAppendElement(in, l, S(lang.code));
            Tcl_ListObjAppendElement(in, l, S(lang.name));
        }
        Tcl_SetObjResult(in, l);
        return TCL_OK;
    }
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
        row({TF("Home folder  (%1)", {home}), home, "", "", "", "", ""});
        for (const Drive &d : listDrives()) {
            const double f = double(d.used) / double(d.total);
            row({d.mp, d.mp, d.fs, fmtSize(d.used), fmtSize(d.avail), fmtSize(d.total), textBar(f) + " " + fmtPct(f)});
        }
        Tcl_SetObjResult(in, list);
    } else if (sub == "isremote") {
        std::string host, rpath;
        Tcl_SetObjResult(in, Tcl_NewBooleanObj(parseRemote(arg(2), host, rpath)));
    } else if (sub == "home") {
        Tcl_SetObjResult(in, S(homeDir()));
    } else if (sub == "scan") {
        if (arg(3) == "1")
            a->startPrivilegedScan(arg(2));
        else
            a->startScan(arg(2));
    } else if (sub == "res") {
        a->resAction(arg(2));
    } else if (sub == "job") {
        if (a->job_)
            a->job_->cancel();
    } else if (sub == "find") {
        std::string err;
        ResultTable t = searchTree(a->s_.root(), arg(2), a->s_.apparent, &err);
        if (!err.empty()) {
            a->call({S("showError"), S(err), S("")});
        } else {
            a->eval("destroy .find");
            t.title += ": " + arg(2);
            a->showTable(std::move(t));
        }
    } else {
        Tcl_SetObjResult(in, S("unknown subcommand: " + sub));
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
        std::fprintf(stderr, "Tcl/Tk could not be started: %s\n", Tcl_GetStringResult(in));
        return 1;
    }
    auto *app = new TkApp(in);
    Tcl_CreateObjCommand(in, "dt", TkApp::command, app, nullptr);
    if (Tcl_EvalEx(in, UI_SCRIPT, -1, TCL_EVAL_GLOBAL) != TCL_OK) {
        std::fprintf(stderr, "UI script error: %s\n%s\n", Tcl_GetStringResult(in),
                     Tcl_GetVar(in, "errorInfo", TCL_GLOBAL_ONLY));
        return 1;
    }
    app->applySettings();
    Tcl_SetVar(in, "::iconFile", TUXDIRSTAT_DATADIR "/icons/hicolor/48x48/apps/tuxdirstat.png", TCL_GLOBAL_ONLY);
    Tcl_EvalEx(in, "catch {wm iconphoto . -default [image create photo -file $::iconFile]}", -1, TCL_EVAL_GLOBAL);
    if (!path.empty() && g_file_test(path.c_str(), G_FILE_TEST_IS_REGULAR))
        app->openSnapshot(path);
    else if (!path.empty())
        app->startScan(path);
    else if (Settings::load().openLast && g_file_test(lastScanFile().c_str(), G_FILE_TEST_IS_REGULAR))
        app->openLast();
    else
        app->showStartDialog();
    Tk_MainLoop();
    delete app;
    Tcl_DeleteInterp(in);
    return 0;
}
