// Terminal interface (ncurses): browse folders by size, search, file types, delete.
// Shares the scanner, settings, translations and tools with the windowed interfaces.

#include <algorithm>
#include <clocale>
#include <csignal>
#include <cstring>
#include <cwchar>
#include <string>
#include <cstdlib>
#include <langinfo.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <ncurses.h>

#include "extinfo.h"
#include "i18n.h"
#include "report.h"
#include "session.h"
#include "tools.h"

namespace {

// Display width of a UTF-8 string, and the string cut to `cols` columns ("…" when cut).
int width(const std::string &s)
{
    std::mbstate_t st{};
    int w = 0;
    const char *p = s.c_str();
    const char *end = p + s.size();
    while (p < end) {
        wchar_t wc;
        const size_t n = std::mbrtowc(&wc, p, size_t(end - p), &st);
        if (n == size_t(-1) || n == size_t(-2)) {
            ++p;
            ++w;
            st = {};
            continue;
        }
        if (n == 0)
            break;
        const int cw = wcwidth(wc);
        w += cw > 0 ? cw : 0;
        p += n;
    }
    return w;
}

std::string fit(const std::string &s, int cols, bool left = true)
{
    if (cols <= 0)
        return {};
    if (width(s) <= cols)
        return left ? s + std::string(size_t(cols - width(s)), ' ') : std::string(size_t(cols - width(s)), ' ') + s;
    // keep the end of paths readable: cut from the start for right-aligned text
    std::string out;
    std::mbstate_t st{};
    const char *p = s.c_str();
    const char *end = p + s.size();
    int w = 0;
    while (p < end) {
        wchar_t wc;
        const size_t n = std::mbrtowc(&wc, p, size_t(end - p), &st);
        if (n == size_t(-1) || n == size_t(-2) || n == 0)
            break;
        const int cw = std::max(0, wcwidth(wc));
        if (w + cw > cols - 1)
            break;
        out.append(p, n);
        w += cw;
        p += n;
    }
    out += "…";
    return out + std::string(size_t(std::max(0, cols - w - 1)), ' ');
}

// Keeps the end of long paths visible: "…/projects/photos".
std::string fitTail(const std::string &s, int cols)
{
    if (width(s) <= cols)
        return fit(s, cols);
    size_t start = 0;
    while (start < s.size() && width(s.substr(start)) > cols - 1) {
        ++start;
        while (start < s.size() && (static_cast<unsigned char>(s[start]) & 0xC0) == 0x80)
            ++start;
    }
    return "…" + s.substr(start);
}

std::string bar(double f, int cols, bool utf8)
{
    if (cols <= 0)
        return {};
    f = std::clamp(f, 0.0, 1.0);
    if (!utf8) {
        const int n = int(f * cols + 0.5);
        return std::string(size_t(n), '#') + std::string(size_t(cols - n), ' ');
    }
    static const char *parts[] = {"", "▏", "▎", "▍", "▌", "▋", "▊", "▉"};
    const int eighths = int(f * cols * 8 + 0.5);
    std::string s;
    for (int i = 0; i < eighths / 8; ++i)
        s += "█";
    s += parts[eighths % 8];
    const int used = eighths / 8 + (eighths % 8 ? 1 : 0);
    return s + std::string(size_t(std::max(0, cols - used)), ' ');
}

enum Pair { P_HEAD = 1, P_SEL, P_DIR, P_BAR, P_DIM, P_WARN };

class Tui {
public:
    explicit Tui(std::string path) : path_(std::move(path)) {}
    int run();

private:
    enum class Mode { Browse, Types, Results, Help };

    void start(const std::string &path, Node *target = nullptr);
    void poll();
    void draw();
    void drawBrowse(int top, int rows);
    void drawTypes(int top, int rows);
    void drawResults(int top, int rows);
    void drawHelp(int top, int rows);
    void key(int ch);
    void sortRows();
    void enter(Node *dir, Node *select = nullptr);
    Node *current() const { return rows_.empty() ? nullptr : rows_[size_t(sel_)]; }
    std::string prompt(const std::string &question);
    bool ask(const std::string &question);
    void message(const std::string &m) { msg_ = m; }
    void removeSelected(bool permanent);
    void search();
    void moveSel(int delta);

    std::string path_;
    Session s_;
    Settings st_ = Settings::load();
    Node *dir_ = nullptr;
    std::vector<Node *> rows_;
    int sel_ = 0, top_ = 0;
    SortKey sort_ = SortKey::Size;
    bool desc_ = true;
    Mode mode_ = Mode::Browse;
    ResultTable results_;
    std::string msg_;
    bool utf8_ = false, quit_ = false;
    int spin_ = 0;
};

void Tui::start(const std::string &path, Node *target)
{
    s_.apparent = st_.apparent;
    s_.oneFs = st_.oneFs;
    s_.grouping = st_.grouping;
    s_.groupMin = st_.groupMin;
    struct stat sb;
    if (!target && ::stat(path.c_str(), &sb) == 0 && S_ISREG(sb.st_mode)) {  // a saved snapshot
        s_.openSnapshot(path);
        return;
    }
    s_.startScan(path, target);
}

void Tui::poll()
{
    switch (s_.poll()) {
    case Session::Result::NewTree:
        s_.commitNewTree();
        mode_ = Mode::Browse;
        enter(s_.root());
        message(TF("Done: %1 items in %2 s.", {fmtInt(s_.lastCount()), fmtDecimal(s_.elapsed(), 2)}));
        break;
    case Session::Result::Refresh: {
        Node *t = s_.refreshTarget();
        takeChildren(t, s_.pendingRoot());
        s_.commitRefresh();
        enter(dir_ && dir_->isWithin(s_.root()) ? dir_ : t);
        message(TF("Done: %1 items in %2 s.", {fmtInt(s_.lastCount()), fmtDecimal(s_.elapsed(), 2)}));
        break;
    }
    case Session::Result::Failed:
        message(std::string(T("The scan failed:")) + " " + s_.error());
        if (!s_.root())
            quit_ = true;
        break;
    case Session::Result::Cancelled:
        message(T("Scan stopped."));
        if (!s_.root())
            quit_ = true;
        break;
    default:
        break;
    }
}

void Tui::sortRows()
{
    Node *keep = current();
    rows_.assign(dir_->children.begin(), dir_->children.end());
    const bool ap = s_.apparent;
    const SortKey k = sort_;
    const bool d = desc_;
    std::stable_sort(rows_.begin(), rows_.end(), [ap, k, d](const Node *a, const Node *b) {
        auto cmp = [&](auto x, auto y) { return d ? x > y : x < y; };
        switch (k) {
        case SortKey::Name: {
            const int c = std::string(a->nameView()).compare(std::string(b->nameView()));
            return d ? c > 0 : c < 0;
        }
        case SortKey::Items:
            return cmp(uint64_t(a->files) + a->dirs, uint64_t(b->files) + b->dirs);
        case SortKey::MTime:
            return cmp(a->mtime, b->mtime);
        default:
            return cmp(a->sizeOf(ap), b->sizeOf(ap));
        }
    });
    sel_ = 0;
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i] == keep)
            sel_ = int(i);
}

void Tui::enter(Node *dir, Node *select)
{
    if (!dir)
        return;
    dir_ = dir;
    rows_.clear();
    sortRows();
    sel_ = top_ = 0;
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i] == select)
            sel_ = int(i);
}

void Tui::moveSel(int delta)
{
    const int n = mode_ == Mode::Results ? int(results_.rows.size()) : mode_ == Mode::Types ? int(s_.ext.size()) : int(rows_.size());
    if (n == 0)
        return;
    sel_ = std::clamp(sel_ + delta, 0, n - 1);
}

std::string Tui::prompt(const std::string &question)
{
    const int H = LINES;
    move(H - 1, 0);
    clrtoeol();
    attron(A_BOLD);
    addstr(question.c_str());
    attroff(A_BOLD);
    addstr(" ");
    echo();
    curs_set(1);
    timeout(-1);
    char buf[512] = "";
    getnstr(buf, sizeof buf - 1);
    noecho();
    curs_set(0);
    timeout(100);
    return buf;
}

bool Tui::ask(const std::string &question)
{
    const int H = LINES;
    move(H - 1, 0);
    clrtoeol();
    attron(COLOR_PAIR(P_WARN) | A_BOLD);
    addstr(fit(question + " [y/N]", COLS - 1).c_str());
    attroff(COLOR_PAIR(P_WARN) | A_BOLD);
    timeout(-1);
    const int c = getch();
    timeout(100);
    return c == 'y' || c == 'Y' || c == 'e' || c == 'E';  // e: evet
}

void Tui::removeSelected(bool permanent)
{
    Node *n = current();
    if (!n || s_.scanning())
        return;
    if (!isLocalTree(s_.root())) {
        message(T("This scan is from another computer or a snapshot of one; its files cannot be deleted from here."));
        return;
    }
    const std::string kind = (n->flags & F_GROUP) ? TF("the %1 files in this group", {fmtInt(n->files)})
                             : n->isDir           ? T("this folder")
                                                  : T("this file");
    const std::string q = (permanent ? TF("Delete %1 permanently?", {kind}) : TF("Move %1 to the trash?", {kind})) + " " +
                          n->path() + " (" + fmtSize(n->sizeOf(s_.apparent)) + ")";
    if (!ask(q))
        return;
    const std::string done = TF("Deleted: %1 (%2)", {n->path(), fmtSize(n->sizeOf(s_.apparent))});
    std::string err, failed;
    for (const std::string &p : diskPaths(n)) {
        const bool ok = permanent ? removePath(p, &err) : trashWithCommand(p);
        if (!ok && failed.empty())
            failed = p;
    }
    if (!failed.empty()) {
        message(std::string(T("Could not delete (or only partly deleted):")) + " " + failed + (err.empty() ? "" : " — " + err));
        start(realNode(n->parent)->path(), realNode(n->parent));
        return;
    }
    Node *t = s_.removalTarget(n);
    Node *parent = t->parent;
    s_.remove(t);
    if (dir_ == t || dir_->isWithin(t))
        dir_ = parent;
    const int keep = sel_;
    enter(dir_);
    sel_ = std::min(keep, std::max(0, int(rows_.size()) - 1));
    message(done);
}

void Tui::search()
{
    const std::string q = prompt(T("Find (e.g. *.iso >1G older:1y):"));
    if (q.empty())
        return;
    std::string err;
    ResultTable t = searchTree(s_.root(), q, s_.apparent, &err, 2000);
    if (!err.empty()) {
        message(err);
        return;
    }
    results_ = std::move(t);
    results_.title += ": " + q;
    mode_ = Mode::Results;
    sel_ = top_ = 0;
}

void Tui::draw()
{
    erase();
    const int W = COLS, H = LINES;
    Node *root = s_.root();
    // header
    attron(COLOR_PAIR(P_HEAD) | A_BOLD);
    const std::string head = " TuxDirStat " TUXDIRSTAT_VERSION "  ";
    mvaddstr(0, 0, head.c_str());
    addstr(fitTail(root ? (dir_ ? dir_->path() : root->path()) : std::string(), W - width(head)).c_str());
    attroff(COLOR_PAIR(P_HEAD) | A_BOLD);
    if (root) {
        std::string info = TF("Total: %1 · %2 files · %3 folders", {fmtSize(root->sizeOf(s_.apparent)), fmtInt(root->files), fmtInt(root->dirs)});
        info += "  ·  " + std::string(s_.apparent ? T("apparent size") : T("size on disk"));
        uint64_t tot = 0, av = 0;
        if (isLocalTree(root) && diskUsage(root->path(), tot, av))
            info += "  ·  " + TF("%1 free", {fmtSize(av)});
        attron(COLOR_PAIR(P_DIM));
        mvaddstr(1, 0, fit(" " + info, W).c_str());
        attroff(COLOR_PAIR(P_DIM));
    }
    const int top = 2, rows = H - top - 2;
    if (s_.scanning()) {
        static const char *sp = "|/-\\";
        const std::string t = std::string(1, sp[spin_++ % 4]) + " " + s_.progressText();
        mvaddstr(H / 2, 2, fit(t, W - 4).c_str());
        attron(COLOR_PAIR(P_DIM));
        mvaddstr(H / 2 + 2, 2, fit(T("Press Esc or q to stop."), W - 4).c_str());
        attroff(COLOR_PAIR(P_DIM));
    } else if (root) {
        switch (mode_) {
        case Mode::Browse: drawBrowse(top, rows); break;
        case Mode::Types: drawTypes(top, rows); break;
        case Mode::Results: drawResults(top, rows); break;
        case Mode::Help: drawHelp(top, rows); break;
        }
    }
    // footer
    attron(COLOR_PAIR(P_HEAD));
    const char *keys = mode_ == Mode::Browse
                           ? N_("↑↓ move  →/Enter open  ← back  s/n/c/m sort  d trash  D delete  / find  t types  i info  ? help  q quit")
                           : N_("↑↓ move  Enter show in tree  Esc back  q quit");
    std::string k = T(keys);
    if (!utf8_)
        for (auto [from, to] : {std::pair{"↑↓", "Up/Down"}, {"→", "Right"}, {"←", "Left"}})
            for (size_t at; (at = k.find(from)) != std::string::npos;)
                k.replace(at, std::strlen(from), to);
    mvaddstr(H - 1, 0, fit(" " + k, W).c_str());
    attroff(COLOR_PAIR(P_HEAD));
    if (!msg_.empty()) {
        attron(A_BOLD);
        mvaddstr(H - 2, 0, fit(" " + msg_, W).c_str());
        attroff(A_BOLD);
    }
    refresh();
}

void Tui::drawBrowse(int top, int rows)
{
    const int W = COLS;
    const bool ap = s_.apparent;
    const uint64_t total = std::max<uint64_t>(1, dir_->sizeOf(ap));
    if (sel_ < top_)
        top_ = sel_;
    if (sel_ >= top_ + rows)
        top_ = sel_ - rows + 1;
    int y = top;
    if (dir_->parent) {
        attron(COLOR_PAIR(P_DIM));
        mvaddstr(y++, 0, fit("  ../", W).c_str());
        attroff(COLOR_PAIR(P_DIM));
        --rows;
        if (sel_ >= top_ + rows)
            top_ = sel_ - rows + 1;
    }
    if (rows_.empty()) {
        attron(COLOR_PAIR(P_DIM));
        mvaddstr(y, 2, T("(empty folder)"));
        attroff(COLOR_PAIR(P_DIM));
        return;
    }
    const int barW = std::clamp(W / 6, 8, 24);
    for (int i = top_; i < int(rows_.size()) && y < top + rows + (dir_->parent ? 1 : 0); ++i, ++y) {
        Node *n = rows_[size_t(i)];
        const double f = double(n->sizeOf(ap)) / double(total);
        const std::string size = fit(fmtSize(n->sizeOf(ap)), 10, false);
        const std::string pct = fit(fmtPct(f), 7, false);
        const std::string items = n->isDir ? fit(fmtInt(uint64_t(n->files) + n->dirs), 9, false) : std::string(9, ' ');
        std::string name = displayName(n, false);
        if (n->isDir && !(n->flags & F_GROUP))
            name += "/";
        if (n->flags & F_ERROR)
            name = "! " + name;
        else if (n->flags & F_MOUNT)
            name = "> " + name;
        else if (n->flags & F_LINK)
            name = "@ " + name;
        const bool selRow = i == sel_;
        if (selRow)
            attron(COLOR_PAIR(P_SEL) | A_BOLD);
        mvaddstr(y, 0, (" " + size + " " + pct + " ").c_str());
        if (!selRow)
            attron(COLOR_PAIR(P_BAR));
        addstr(("[" + bar(f, barW, utf8_) + "]").c_str());
        if (!selRow)
            attroff(COLOR_PAIR(P_BAR));
        addstr((" " + items + "  ").c_str());
        const int used = 1 + 10 + 1 + 7 + 1 + barW + 2 + 1 + 9 + 2;
        if (!selRow && n->isDir)
            attron(COLOR_PAIR(P_DIR) | A_BOLD);
        addstr(fit(name, W - used).c_str());
        if (!selRow && n->isDir)
            attroff(COLOR_PAIR(P_DIR) | A_BOLD);
        if (selRow)
            attroff(COLOR_PAIR(P_SEL) | A_BOLD);
    }
}

void Tui::drawTypes(int top, int rows)
{
    const int W = COLS;
    const uint64_t total = std::max<uint64_t>(1, s_.root()->sizeOf(s_.apparent));
    attron(A_BOLD);
    mvaddstr(top, 0, fit(std::string(" ") + T("File types"), W).c_str());
    attroff(A_BOLD);
    ++top;
    --rows;
    if (sel_ < top_)
        top_ = sel_;
    if (sel_ >= top_ + rows)
        top_ = sel_ - rows + 1;
    const int barW = std::clamp(W / 6, 8, 24);
    for (int i = top_, y = top; i < int(s_.ext.size()) && y < top + rows; ++i, ++y) {
        const ExtRow &r = s_.ext[size_t(i)];
        const double f = double(r.bytes) / double(total);
        if (i == sel_)
            attron(COLOR_PAIR(P_SEL) | A_BOLD);
        const std::string line = " " + fit(fmtSize(r.bytes), 10, false) + " " + fit(fmtPct(f), 7, false) + " [" +
                                 bar(f, barW, utf8_) + "] " + fit(fmtInt(r.count), 9, false) + "  " +
                                 fit(s_.extLabel(r.ext), 14) + " ";
        mvaddstr(y, 0, line.c_str());
        addstr(fit(describeExt(r.ext), W - width(line) - 1).c_str());
        if (i == sel_)
            attroff(COLOR_PAIR(P_SEL) | A_BOLD);
    }
}

void Tui::drawResults(int top, int rows)
{
    const int W = COLS;
    attron(A_BOLD);
    mvaddstr(top, 0, fit(" " + results_.title + " — " + fmtInt(results_.rows.size()), W).c_str());
    attroff(A_BOLD);
    ++top;
    --rows;
    if (results_.rows.empty()) {
        mvaddstr(top + 1, 2, fit(results_.emptyText.empty() ? T("Nothing found.") : results_.emptyText, W - 4).c_str());
        return;
    }
    if (sel_ < top_)
        top_ = sel_;
    if (sel_ >= top_ + rows)
        top_ = sel_ - rows + 1;
    for (int i = top_, y = top; i < int(results_.rows.size()) && y < top + rows; ++i, ++y) {
        const ResultRow &r = results_.rows[size_t(i)];
        if (i == sel_)
            attron(COLOR_PAIR(P_SEL) | A_BOLD);
        const std::string base = s_.root()->path();
        std::string rel = r.path;
        if (rel.rfind(base, 0) == 0 && rel.size() > base.size())
            rel = rel.substr(base.size() + (base == "/" ? 0 : 1));
        mvaddstr(y, 0, (" " + fit(fmtSize(r.bytes), 10, false) + "  " + fitTail(rel, W - 14)).c_str());
        if (i == sel_)
            attroff(COLOR_PAIR(P_SEL) | A_BOLD);
    }
}

void Tui::drawHelp(int top, int)
{
    static const std::pair<const char *, const char *> keys[] = {
        {"Up/Down, PgUp/PgDn, Home/End", N_("move")},
        {"Right, Enter, l", N_("open the folder")},
        {"Left, Backspace, h", N_("back to the parent folder")},
        {"s / n / c / m", N_("sort by size / name / items / date (again: reverse)")},
        {"a", N_("apparent size or size on disk")},
        {"g", N_("group similar items")},
        {"d", N_("move to trash")},
        {"D", N_("delete permanently")},
        {"r / R", N_("rescan this folder / everything")},
        {"/", N_("find files (same syntax as the windowed versions)")},
        {"t", N_("file types")},
        {"i", N_("information about the selected item")},
        {"q", N_("quit")},
    };
    attron(A_BOLD);
    mvaddstr(top, 2, T("Keys"));
    attroff(A_BOLD);
    int y = top + 2;
    for (const auto &[k, what] : keys)
        mvaddstr(y++, 2, fit(fit(k, 32) + T(what), COLS - 4).c_str());
}

void Tui::key(int ch)
{
    msg_.clear();
    if (s_.scanning()) {
        if (ch == 27 || ch == 'q')
            s_.cancelScan();
        return;
    }
    switch (ch) {
    case 'q':
        if (mode_ == Mode::Browse)
            quit_ = true;
        else
            mode_ = Mode::Browse;
        return;
    case 27:
    case KEY_LEFT:
    case 'h':
    case KEY_BACKSPACE:
    case 127:
        if (mode_ != Mode::Browse) {
            mode_ = Mode::Browse;
            enter(dir_);
        } else if (dir_ && dir_->parent) {
            Node *from = dir_;
            Node *up = dir_->parent;
            enter(up, from);
        }
        return;
    case KEY_UP:
    case 'k':
        moveSel(-1);
        return;
    case KEY_DOWN:
    case 'j':
        moveSel(1);
        return;
    case KEY_PPAGE:
        moveSel(-(LINES - 5));
        return;
    case KEY_NPAGE:
        moveSel(LINES - 5);
        return;
    case KEY_HOME:
        sel_ = 0;
        return;
    case KEY_END:
        moveSel(1 << 30);
        return;
    case '?':
        mode_ = mode_ == Mode::Help ? Mode::Browse : Mode::Help;
        return;
    case 't':
        mode_ = Mode::Types;
        sel_ = top_ = 0;
        return;
    case '/':
        search();
        return;
    case 'R':
    case KEY_F(5):
        if (s_.root())
            start(s_.root()->path());
        return;
    default:
        break;
    }
    if (mode_ == Mode::Results) {
        if ((ch == '\n' || ch == KEY_ENTER || ch == KEY_RIGHT) && !results_.rows.empty()) {
            Node *n = results_.rows[size_t(sel_)].node;
            mode_ = Mode::Browse;
            if (n && n->parent)
                enter(n->parent->isDir ? n->parent : s_.root(), n);
        }
        return;
    }
    if (mode_ != Mode::Browse)
        return;
    Node *n = current();
    switch (ch) {
    case '\n':
    case KEY_ENTER:
    case KEY_RIGHT:
    case 'l':
        if (n && n->isDir && !n->children.empty())
            enter(n);
        break;
    case 's':
    case 'n':
    case 'c':
    case 'm': {
        const SortKey k = ch == 's' ? SortKey::Size : ch == 'n' ? SortKey::Name : ch == 'c' ? SortKey::Items : SortKey::MTime;
        desc_ = sort_ == k ? !desc_ : k != SortKey::Name;
        sort_ = k;
        sortRows();
        break;
    }
    case 'a':
        s_.apparent = !s_.apparent;
        st_.apparent = s_.apparent;
        st_.save();
        s_.recomputeExt();
        sortRows();
        break;
    case 'g':
        s_.setGrouping(!s_.grouping, s_.groupMin);
        st_.grouping = s_.grouping;
        st_.save();
        if (dir_)
            dir_ = realNode(dir_);
        enter(dir_);
        break;
    case 'd':
        removeSelected(false);
        break;
    case 'D':
        removeSelected(true);
        break;
    case 'r':
        if (dir_)
            start(realNode(dir_)->path(), realNode(dir_));
        break;
    case 'i':
        if (n)
            message(n->path() + " — " + fmtSize(n->size) + " / " + fmtSize(n->alloc) + " · " + fmtTime(n->mtime) + " · " +
                    userName(n->uid) + (n->isDir ? " · " + TF("%1 files", {fmtInt(n->files)}) : ""));
        break;
    default:
        break;
    }
}

int Tui::run()
{
    std::setlocale(LC_ALL, "");
    std::setlocale(LC_NUMERIC, "C");
    utf8_ = std::strcmp(nl_langinfo(CODESET), "UTF-8") == 0;
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    set_escdelay(25);
    timeout(100);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(P_HEAD, COLOR_BLACK, COLOR_CYAN);
        init_pair(P_SEL, COLOR_BLACK, COLOR_WHITE);
        init_pair(P_DIR, COLOR_BLUE, -1);
        init_pair(P_BAR, COLOR_GREEN, -1);
        init_pair(P_DIM, COLOR_CYAN, -1);
        init_pair(P_WARN, COLOR_RED, -1);
    }
    start(path_);
    while (!quit_) {
        poll();
        if (quit_)
            break;
        draw();
        const int ch = getch();
        if (ch != ERR)
            key(ch);
    }
    endwin();
    if (!s_.root() && !s_.error().empty()) {
        std::fprintf(stderr, "tuxdirstat-tui: %s\n", s_.error().c_str());
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char **argv)
{
    if (int rc = runCommandLine(argc, argv); rc >= 0)
        return rc;
    std::string path = pathArgument(argc, argv);
    if (path.empty())
        path = ".";
    std::string host, rpath;
    if (!parseRemote(path, host, rpath)) {
        if (char *real = realpath(path.c_str(), nullptr)) {
            path = real;
            std::free(real);
        }
    }
    std::signal(SIGPIPE, SIG_IGN);
    Tui t(path);
    return t.run();
}
