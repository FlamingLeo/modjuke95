#include "common.h"
#include "engine.h"
#include "model.h"
#include "mptwrap.h"
#include "res.h"

#include <cderr.h> /* FNERR_BUFFERTOOSMALL */
#include <deque>
#include <map>
#include <math.h>
#include <set>

struct App
{
    HWND hwnd = NULL;
    HWND hList = NULL, hSearch = NULL;
    HWND hSrc = NULL, hOrder = NULL, hShuf = NULL, hFilter = NULL;
    HWND hAddPl = NULL, hClearPl = NULL, hOpenFld = NULL, hRescan = NULL;
    HWND hTitle = NULL, hFmt = NULL, hTrk = NULL, hLen = NULL, hPos = NULL, hSeq = NULL,
         hOut = NULL;
    HWND hSub = NULL, hOf = NULL, hPlayAll = NULL, hLog = NULL;
    /* tracker (pattern) view */
    HWND hPat = NULL, hTab = NULL;
    HFONT hTrkFont = NULL;
    HBRUSH trkBg = NULL, trkHi = NULL;
    int trkCharW = 6, trkRowH = 12;
    std::vector<std::string> trkRows;
    int trkHalf = 0;
    int trkChOff = 0;             /* requested channel paging offset */
    int trkOff = 0, trkNchView = 0, trkTotal = 0; /* from the last RowsMsg */
    int trkShownOrder = -1, trkShownRow = -1; /* row currently on screen */
    HWND hPrev = NULL, hPlay = NULL, hNext = NULL, hStop = NULL;
    HWND hLoop = NULL, hRepeat = NULL, hMute = NULL, hVol = NULL, hVolPct = NULL,
         hTime = NULL, hSeek = NULL, hDur = NULL;
    HWND hStatus = NULL;
    HACCEL hAcc = NULL;
    HFONT hFont = NULL;

    Settings set;
    std::vector<Track> lib, fav;
    std::vector<Playlist> pls;
    std::vector<std::string> ign; // ignored file paths (lowercase)
    /* persistent shuffle orders, keyed per source ("library", "favorites",
     * "playlist:<name>") - like the original, a drawn order survives sort
     * changes, source switches and restarts */
    std::map<std::string, std::vector<std::string>> plans;
    std::vector<int> view;   // into active source
    int playSrc = -1, playIdx = -1; // row currently playing (for the list marker)
    int curSub = 1;
    int srcSel = 0;
    std::string lastDir;
    std::vector<std::string> exts;
    std::vector<int> shuf;

    Engine engine;

    HANDLE anaThr = NULL, aEv = NULL;
    CRITICAL_SECTION acs;
    std::deque<std::pair<int, int>> aq; /* FIFO: parse in queued (=view) order */
    bool aquit = false;
    bool anaPause = false; /* background mode: analyzer idles (under acs) */
    int powerMode = 0;     /* 0 focused, 1 visible-unfocused, 2 minimized */
    bool threadsGone = true; /* exit: engine + analyzer both joined */
    int viewSrc = -1;        /* source the list rows currently show */
    bool panelHidden = false; /* window too narrow: info/tracker panel hidden */

    std::vector<Track> &srcBy(int sel)
    {
        static std::vector<Track> empty;
        if (sel == 0)
            return lib;
        if (sel == 1)
            return fav;
        int p = sel - 2;
        if (p >= 0 && p < (int)pls.size())
            return pls[p].tr;
        return empty;
    }
    std::vector<Track> &src() { return srcBy(srcSel); }
};

static App G;
/* tick count of the last seekbar drag; refresh_ui() holds back its automatic
 * thumb updates for a moment afterwards so it never fights the user's drag */
static DWORD g_seekDrag = 0;

static void logline(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    m95_log(G.hLog, "%s", buf);
}

/* thread-safe variant: posts to the UI thread (see WM_APP_LOG) */
static void postlog(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    char *s = (char *)malloc(strlen(buf) + 1);
    if (!s)
        return; /* heap exhaustion: drop the line, don't crash */
    strcpy(s, buf);
    PostMessageA(G.hwnd, WM_APP_LOG, 0, (LPARAM)s);
}

static void setfont(HWND h)
{
    SendMessageA(h, WM_SETFONT, (WPARAM)G.hFont, TRUE);
}

static HWND mkctrl(const char *cls, const char *text, DWORD style, int x, int y, int w,
                   int h, int id)
{
    HWND hw = CreateWindowExA(0, cls, text, style | WS_CHILD | WS_VISIBLE, x, y, w, h,
                              G.hwnd, (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
    setfont(hw);
    return hw;
}

static void refresh_count()
{
    if (G.hStatus) {
        /* right-hand status segment: which list is shown and how many of its
         * tracks are currently visible (search/filter/ignore can hide some) */
        const char *nm = "Library";
        if (G.srcSel == 1)
            nm = "Favorites";
        else if (G.srcSel >= 2 && G.srcSel - 2 < (int)G.pls.size())
            nm = G.pls[G.srcSel - 2].name.c_str();
        unsigned total = (unsigned)G.src().size();
        unsigned vis = (unsigned)G.view.size();
        /* playlist names can be up to 255 chars - build in a std::string,
         * a fixed 96-byte buffer here would overflow */
        std::string rb;
        {
            char nb[80];
            if (vis == total)
                sprintf(nb, "%u tracks", total);
            else
                sprintf(nb, "%u of %u tracks", vis, total);
            rb = std::string(nm) + ": " + nb;
        }
        /* size the right part to the text so even 4-digit counts never clip
         * on narrow windows; the left part keeps at least 240px for status */
        HDC dc = GetDC(G.hStatus);
        SIZE sz;
        sz.cx = (int)rb.size() * 7; /* fallback width if GDI is exhausted */
        sz.cy = 0;
        if (dc) {
            HFONT of = (HFONT)SelectObject(dc, G.hFont);
            GetTextExtentPoint32A(dc, rb.c_str(), (int)rb.size(), &sz);
            SelectObject(dc, of);
            ReleaseDC(G.hStatus, dc);
        }
        RECT cr;
        GetClientRect(G.hStatus, &cr);
        int split = cr.right - (sz.cx + 10);
        if (split < 240)
            split = 240;
        if (split > cr.right - 70)
            split = cr.right - 70;
        int parts[2] = { split, -1 };
        SendMessageA(G.hStatus, SB_SETPARTS, 2, (LPARAM)parts);
        SendMessageA(G.hStatus, SB_SETTEXTA, 1, (LPARAM)rb.c_str());
    }
}

static std::string ext_of(const std::string &path)
{
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos)
        return "";
    return m95_lower(path.substr(dot + 1));
}

static bool types_contains(const std::string &ext)
{
    const std::string &ts = G.set.types;
    size_t p = 0;
    while (p < ts.size()) {
        size_t e = ts.find(';', p);
        if (e == std::string::npos)
            e = ts.size();
        if (e > p && ts.compare(p, e - p, ext) == 0)
            return true;
        p = e + 1;
    }
    return false;
}

static bool type_enabled(const std::string &ext)
{
    return G.set.types.empty() || types_contains(ext);
}

static std::string shuf_key(int src)
{
    if (src == 0)
        return "library";
    if (src == 1)
        return "favorites";
    int p = src - 2;
    if (p >= 0 && p < (int)G.pls.size())
        return "playlist:" + G.pls[p].name;
    return "";
}

static void save_plan_file()
{
    /* write temp + swap so a crash/power loss mid-write cannot truncate
     * the existing plan file (a torn file would load as partial plans).
     * Single-threaded at every call site, so remove+rename is safe. */
    std::string dst = m95_exe_dir() + "shuffles.txt";
    FILE *f = m95_tmp_open(dst);
    if (!f)
        return;
    for (std::map<std::string, std::vector<std::string>>::const_iterator it =
             G.plans.begin();
         it != G.plans.end(); ++it) {
        fprintf(f, "[%s]\n", it->first.c_str());
        for (size_t i = 0; i < it->second.size(); i++)
            fprintf(f, "%s\n", it->second[i].c_str());
    }
    m95_tmp_commit(f, dst); /* a failed write keeps the old file */
}

static void load_plan_file()
{
    std::string fn = m95_exe_dir() + "shuffles.txt";
    FILE *f = fopen(fn.c_str(), "r");
    if (!f) /* crash between remove and rename: only the .tmp is left */
        f = fopen((fn + ".tmp").c_str(), "r");
    if (!f)
        return;
    char ln[1024];
    std::string cur;
    while (fgets(ln, sizeof(ln), f)) {
        if (!strchr(ln, '\n') && !feof(f)) {
            int ch; /* over-long line: no path is that long, skip it */
            while ((ch = fgetc(f)) != EOF && ch != '\n') {
            }
            continue;
        }
        std::string s = m95_trim(ln);
        if (s.empty())
            continue;
        if (s[0] == '[' && s[s.size() - 1] == ']') {
            cur = s.substr(1, s.size() - 2);
            G.plans[cur];
            continue;
        }
        if (!cur.empty())
            G.plans[cur].push_back(s);
    }
    fclose(f);
}

/* draw a fresh shuffle order for the active source. `first` is pinned to the
 * front ("Shuffle now": the playing song first); `avoid` is kept away from
 * the front (queue-end repeat: don't reopen with the song that just ended).
 * Like the original, drawing a new order persists it for this source. */
/* uniform-enough index below n: the CRT's RAND_MAX is 32767, so rand() % n
 * can't reach indexes past 32767 in a bigger library; two calls give 30 bits */
static unsigned rand_below(unsigned n)
{
    return (((unsigned)rand() << 15) | (unsigned)rand()) % n;
}

static void draw_shuffle_now(int first, int avoid)
{
    std::vector<Track> &s = G.src();
    int n = (int)s.size();
    G.shuf.clear();
    for (int i = 0; i < n; i++)
        if (i != first)
            G.shuf.push_back(i);
    srand(GetTickCount());
    for (int i = (int)G.shuf.size(); i > 1; i--)
        std::swap(G.shuf[i - 1], G.shuf[rand_below((unsigned)i)]);
    if (avoid >= 0 && G.shuf.size() > 1 && G.shuf[0] == avoid)
        std::swap(G.shuf[0], G.shuf[1 + rand_below((unsigned)(G.shuf.size() - 1))]);
    if (first >= 0 && first < n)
        G.shuf.insert(G.shuf.begin(), first);
    std::string key = shuf_key(G.srcSel);
    if (!key.empty()) {
        std::vector<std::string> p;
        for (size_t i = 0; i < G.shuf.size(); i++)
            if (G.shuf[i] >= 0 && G.shuf[i] < n)
                p.push_back(s[G.shuf[i]].path);
        G.plans[key] = p;
        save_plan_file();
    }
}

/* derive G.shuf (indices) from the stored path plan: planned tracks keep
 * their drawn order, tracks added since are appended at the end */
static void plan_to_shuf()
{
    std::vector<Track> &s = G.src();
    G.shuf.clear();
    const std::vector<std::string> &plan = G.plans[shuf_key(G.srcSel)];
    std::map<std::string, int> rank;
    for (size_t i = 0; i < plan.size(); i++)
        rank[plan[i]] = (int)i;
    std::vector<std::pair<int, int>> known; /* (rank, index) */
    for (size_t i = 0; i < s.size(); i++) {
        std::map<std::string, int>::const_iterator r = rank.find(s[i].path);
        if (r != rank.end())
            known.push_back(std::make_pair(r->second, (int)i));
        else
            G.shuf.push_back((int)i); /* unknown: appended after the plan */
    }
    std::sort(known.begin(), known.end());
    std::vector<int> out;
    out.reserve(s.size());
    for (size_t i = 0; i < known.size(); i++)
        out.push_back(known[i].second);
    for (size_t i = 0; i < G.shuf.size(); i++)
        out.push_back(G.shuf[i]);
    G.shuf.swap(out);
}

/* like the original: reuse this source's drawn order; draw one only when the
 * source never had one */
static void ensure_shuffle_plan()
{
    std::string key = shuf_key(G.srcSel);
    if (key.empty())
        return;
    std::map<std::string, std::vector<std::string>>::const_iterator it =
        G.plans.find(key);
    if (it != G.plans.end() && !it->second.empty()) {
        /* a plan that shares no track with the list (another folder was
         * scanned since) is stale: draw a fresh order instead of falling
         * back to scan order */
        std::set<std::string> inPlan(it->second.begin(), it->second.end());
        const std::vector<Track> &s = G.src();
        for (size_t i = 0; i < s.size(); i++)
            if (inPlan.count(s[i].path)) {
                plan_to_shuf();
                return;
            }
    }
    if (G.src().empty())
        return;
    /* never drawn for this source: anchor on the playing (or selected) row */
    int first = -1;
    if (G.playSrc == G.srcSel)
        first = G.playIdx;
    if (first < 0) {
        int sel = ListView_GetNextItem(G.hList, -1, LVNI_SELECTED);
        if (sel >= 0) {
            LVITEMA it2;
            memset(&it2, 0, sizeof(it2));
            it2.mask = LVIF_PARAM;
            it2.iItem = sel;
            if (ListView_GetItem(G.hList, &it2))
                first = (int)it2.lParam;
        }
    }
    draw_shuffle_now(first, -1);
}

/* "* " prefix marks the playing row (no custom-draw on Win95 comctl32 4.0) */
static std::string disp_title(const std::vector<Track> &s, int idx)
{
    if (idx < 0 || idx >= (int)s.size())
        return std::string(); /* list rows not rebuilt yet after a clear */
    if (G.playSrc == G.srcSel && idx == G.playIdx)
        return "* " + s[idx].title;
    return s[idx].title;
}

static void refresh_titles()
{
    std::vector<Track> &s = G.src();
    int n = ListView_GetItemCount(G.hList);
    for (int r = 0; r < n; r++) {
        LVITEMA it;
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_PARAM;
        it.iItem = r;
        if (!ListView_GetItem(G.hList, &it))
            continue;
        std::string d = disp_title(s, (int)it.lParam);
        ListView_SetItemText(G.hList, r, 0, (LPSTR)d.c_str());
    }
}

static void clear_playing()
{
    if (G.playIdx < 0)
        return;
    G.playIdx = -1;
    G.playSrc = -1;
    refresh_titles();
}

static bool path_ignored(const std::string &path);
static void rebuild_sources();

static bool search_match(const Track &t, const std::vector<std::string> &terms)
{
    if (terms.empty())
        return true;
    /* like the original: every whitespace-separated term must match somewhere
     * in folder/filename/format/title (including the internal module title);
     * the haystack is cached on the track (no per-keystroke allocations) */
    for (size_t k = 0; k < terms.size(); k++)
        if (t.lhay.find(terms[k]) == std::string::npos)
            return false;
    return true;
}

static void rebuild_view()
{
    EnterCriticalSection(&G.acs);
    std::vector<Track> &s = G.src();
    char buf[256] = "";
    GetWindowTextA(G.hSearch, buf, sizeof(buf));
    std::string q = m95_lower(m95_trim(buf));
    std::vector<std::string> terms;
    {
        size_t p = 0;
        while (p < q.size()) {
            size_t e = q.find(' ', p);
            if (e == std::string::npos)
                e = q.size();
            if (e > p)
                terms.push_back(q.substr(p, e - p));
            p = e + 1;
        }
    }
    G.view.clear();
    G.view.reserve(s.size()); /* one allocation, not a growth chain */
    for (size_t i = 0; i < s.size(); i++) {
        const Track &t = s[i];
        if (path_ignored(t.path))
            continue;
        if (!type_enabled(ext_of(t.path)))
            continue;
        if (G.set.playableOnly && t.broken)
            continue;
        if (G.set.minLen > 0 && t.dur >= 0 && t.dur < G.set.minLen)
            continue;
        if (G.set.maxLen > 0 && t.dur >= 0 && t.dur > G.set.maxLen)
            continue;
        if (!search_match(t, terms))
            continue;
        G.view.push_back((int)i);
    }
    if (G.set.order == ORDER_ALPHA) {
        /* cached lowercase keys: no allocations inside the comparator */
        std::sort(G.view.begin(), G.view.end(), [&](int a, int b) {
            return s[a].ltitle < s[b].ltitle;
        });
    } else if (G.set.order == ORDER_PATH) {
        /* group by folder, then by title */
        std::sort(G.view.begin(), G.view.end(), [&](int a, int b) {
            if (s[a].lfolder != s[b].lfolder)
                return s[a].lfolder < s[b].lfolder;
            return s[a].ltitle < s[b].ltitle;
        });
    } else {
        ensure_shuffle_plan();
        std::vector<int> pos(s.size(), 0);
        for (size_t i = 0; i < G.shuf.size(); i++)
            if (G.shuf[i] < (int)s.size())
                pos[G.shuf[i]] = (int)i;
        std::sort(G.view.begin(), G.view.end(),
                  [&](int a, int b) { return pos[a] < pos[b]; });
    }

    /* keep selection and scroll position when the same source is rebuilt
     * (edits, analysis-driven refilters): the selected track when its row
     * still shows that track (a removal shifts indexes), else the same row */
    bool sameSrc = G.viewSrc == G.srcSel;
    int keepTrack = -1, keepRow = -1, keepTop = 0;
    if (sameSrc) {
        keepTop = ListView_GetTopIndex(G.hList);
        keepRow = ListView_GetNextItem(G.hList, -1, LVNI_SELECTED);
        if (keepRow >= 0) {
            LVITEMA it;
            char tb[300] = "";
            memset(&it, 0, sizeof(it));
            it.mask = LVIF_PARAM | LVIF_TEXT;
            it.iItem = keepRow;
            it.pszText = tb;
            it.cchTextMax = sizeof(tb);
            if (ListView_GetItem(G.hList, &it) && it.lParam >= 0 &&
                it.lParam < (LPARAM)s.size()) {
                std::string shown = tb;
                if (shown.compare(0, 2, "* ") == 0)
                    shown = shown.substr(2);
                if (shown == s[it.lParam].title.substr(0, sizeof(tb) - 3))
                    keepTrack = (int)it.lParam;
            }
        }
    }
    G.viewSrc = G.srcSel;

    /* batch the fill: no per-row repaints on the slow common controls, and
     * pre-allocate the internal item array */
    SendMessageA(G.hList, WM_SETREDRAW, FALSE, 0);
    ListView_SetItemCount(G.hList, (int)G.view.size());
    ListView_DeleteAllItems(G.hList);
    for (size_t r = 0; r < G.view.size(); r++) {
        const Track &t = s[G.view[r]];
        std::string d0 = disp_title(s, G.view[r]);
        LVITEMA it;
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = (int)r;
        it.pszText = (LPSTR)d0.c_str();
        it.lParam = G.view[r];
        int row = ListView_InsertItem(G.hList, &it);
        ListView_SetItemText(G.hList, row, 1, (LPSTR)t.folder.c_str());
        std::string d = t.analyzed ? m95_fmt_time(t.dur) : "";
        ListView_SetItemText(G.hList, row, 2, (LPSTR)d.c_str());
        ListView_SetItemText(G.hList, row, 3, (LPSTR)t.fmt.c_str());
        char nb[16];
        sprintf(nb, "%d", t.ch);
        ListView_SetItemText(G.hList, row, 4, (LPSTR)(t.analyzed ? nb : ""));
        sprintf(nb, "%d", t.ss);
        ListView_SetItemText(G.hList, row, 5, (LPSTR)(t.analyzed ? nb : ""));
    }
    int n = (int)G.view.size();
    if (sameSrc && n > 0) {
        int selRow = -1;
        if (keepTrack >= 0) {
            for (int r = 0; r < n; r++)
                if (G.view[r] == keepTrack) {
                    selRow = r;
                    break;
                }
        } else if (keepRow >= 0) {
            selRow = keepRow < n ? keepRow : n - 1;
        }
        /* bottom of the old page first, then its top: leaves keepTop at
         * the top (LVM_SCROLL units differ between comctl32 versions) */
        if (keepTop > 0 && keepTop < n) {
            int per = ListView_GetCountPerPage(G.hList);
            ListView_EnsureVisible(G.hList, std::min(n - 1, keepTop + std::max(per, 1) - 1), FALSE);
            ListView_EnsureVisible(G.hList, keepTop, FALSE);
        }
        if (selRow >= 0)
            ListView_SetItemState(G.hList, selRow, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
    }
    SendMessageA(G.hList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(G.hList, NULL, TRUE);
    refresh_count();
    LeaveCriticalSection(&G.acs);
}

static void update_row(int srcSel, int idx)
{
    if (srcSel != G.srcSel)
        return;
    if (idx < 0 || idx >= (int)G.src().size())
        return;
    EnterCriticalSection(&G.acs);
    std::vector<Track> &s = G.src();
    const Track &t = s[idx];
    /* find the row via the in-memory view instead of scanning the ListView
     * (one GetItem per row gets expensive across a whole analysis run) */
    int row = -1;
    for (size_t r = 0; r < G.view.size(); r++)
        if (G.view[r] == idx) {
            row = (int)r;
            break;
        }
    if (row >= 0) {
        std::string d0 = disp_title(s, idx);
        ListView_SetItemText(G.hList, row, 0, (LPSTR)d0.c_str());
        std::string d = m95_fmt_time(t.dur);
        ListView_SetItemText(G.hList, row, 2, (LPSTR)d.c_str());
        ListView_SetItemText(G.hList, row, 3, (LPSTR)t.fmt.c_str());
        char nb[16];
        sprintf(nb, "%d", t.ch);
        ListView_SetItemText(G.hList, row, 4, (LPSTR)nb);
        sprintf(nb, "%d", t.ss);
        ListView_SetItemText(G.hList, row, 5, (LPSTR)nb);
    }
    LeaveCriticalSection(&G.acs);
}

/* ---------------- analyzer ---------------- */
/* ---------------- analysis disk cache ------------------------------------
 * Re-parsing every module at each start costs minutes of disk+CPU on Win95
 * hardware. analysis.cache maps lowercase path + size + mtime to the parsed
 * fields; entries are appended while running and rewritten compactly at
 * exit. A big library only costs a few hundred KB - tiny compared to
 * re-reading every module file. */
struct AnaRec
{
    unsigned long sz = 0, mhi = 0, mlo = 0;
    double dur = -1;
    int ch = 0, ss = 1;
    bool broken = false;
    std::string fmt, trk, mt;
};
/* parsed-module result shipped from the analyzer thread to the UI thread;
 * ownership passes with the WM_APP_ANALYZED message */
struct AnaResult
{
    int src = 0, idx = 0;
    bool valid = false;
    double dur = -1;
    int ch = 0, ss = 1;
    bool loaded = false;
    std::string path, fmt, trk, mt;
};
static std::map<std::string, AnaRec> g_acache;
static bool g_acacheCapped = false; /* map no longer mirrors the file */

/* bound the in-memory analysis cache: each entry carries several strings,
 * and a session that churns through many files would grow it without
 * limit. Past the cap it drops wholesale - the on-disk cache file still
 * holds everything, so dropped entries only cost one re-parse if their
 * file comes back. */
static void acache_put(const std::string &key, const AnaRec &r)
{
    if (g_acache.size() >= 16384) {
        g_acache.clear();
        g_acacheCapped = true; /* compaction would now lose entries */
    }
    g_acache[key] = r;
}

static std::string acache_path() { return m95_exe_dir() + "analysis.cache"; }

static void acache_write(FILE *f, const std::string &path, const AnaRec &r)
{
    fprintf(f, "%s\t%lu\t%lu\t%lu\t%d\t%.3f\t%d\t%d\t%s\t%s\t%s\n", path.c_str(),
            r.sz, r.mhi, r.mlo, r.broken ? 1 : 0, r.dur, r.ch, r.ss, r.fmt.c_str(),
            r.trk.c_str(), r.mt.c_str());
}

static void acache_load()
{
    FILE *f = fopen(acache_path().c_str(), "r");
    if (!f)
        return;
    char ln[2048];
    while (fgets(ln, sizeof(ln), f)) {
        if (!strchr(ln, '\n') && !feof(f)) {
            /* over-long (corrupt) line: skip all of it */
            int ch;
            while ((ch = fgetc(f)) != EOF && ch != '\n') {
            }
            continue;
        }
        /* path \t sz \t mhi \t mlo \t broken \t dur \t ch \t ss \t fmt \t trk \t mt */
        char *p[11];
        int n = 0;
        char *c = ln;
        p[0] = c;
        while (*c && n < 10) {
            if (*c == '\t') {
                *c = 0;
                p[++n] = c + 1;
            }
            c++;
        }
        if (n < 10 || !p[0][0])
            continue;
        p[10][strcspn(p[10], "\r\n")] = 0;
        AnaRec r;
        r.sz = strtoul(p[1], NULL, 10);
        r.mhi = strtoul(p[2], NULL, 10);
        r.mlo = strtoul(p[3], NULL, 10);
        r.broken = atoi(p[4]) != 0;
        /* a garbled line must not reach fixed buffers, loops or casts:
         * clamp like the analyzer's own results (libopenmpt's strings are
         * short; songs over a few hours don't exist) */
        r.dur = atof(p[5]);
        if (!(r.dur >= 0 && r.dur < 1e6))
            r.dur = -1;
        r.ch = atoi(p[6]);
        if (r.ch < 0 || r.ch > 256)
            r.ch = 0;
        r.ss = atoi(p[7]);
        if (r.ss < 1 || r.ss > 256)
            r.ss = 1;
        /* older builds cached libopenmpt's UTF-8 as is */
        r.fmt = m95_utf8_to_ansi(std::string(p[8]).substr(0, 16), true);
        r.trk = m95_utf8_to_ansi(std::string(p[9]).substr(0, 96), true);
        r.mt = m95_utf8_to_ansi(std::string(p[10]).substr(0, 128), true);
        if (strlen(p[0]) >= MAX_PATH)
            continue;
        /* re-lowered: older builds folded ASCII letters only */
        acache_put(m95_lower(p[0]), r); /* appended lines override older ones */
    }
    fclose(f);
}

static void acache_rewrite()
{
    if (g_acacheCapped)
        return; /* the map was capped: the append-only file holds more */
    FILE *f = m95_tmp_open(acache_path());
    if (!f)
        return;
    for (std::map<std::string, AnaRec>::iterator it = g_acache.begin();
         it != g_acache.end(); ++it)
        acache_write(f, it->first, it->second);
    m95_tmp_commit(f, acache_path());
}

static bool stat_file(const std::string &path, unsigned long &sz, unsigned long &mhi,
                      unsigned long &mlo)
{
    HANDLE hf = CreateFileA(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0,
                            NULL);
    if (hf == INVALID_HANDLE_VALUE)
        return false;
    sz = GetFileSize(hf, NULL);
    FILETIME ft;
    BOOL ok = GetFileTime(hf, NULL, NULL, &ft);
    CloseHandle(hf);
    if (!ok)
        return false;
    mhi = ft.dwHighDateTime;
    mlo = ft.dwLowDateTime;
    return true;
}

static void queue_analysis(int src)
{
    EnterCriticalSection(&G.acs);
    std::vector<Track> &s = G.srcBy(src); /* under the lock: see analyzer */
    /* replace this source's queued jobs: after a removal their indexes
     * point at other tracks (the analyzer's path check then drops them and
     * those tracks would stay unanalyzed); also avoids duplicates */
    for (std::deque<std::pair<int, int>>::iterator it = G.aq.begin(); it != G.aq.end();)
        it = it->first == src ? G.aq.erase(it) : it + 1;
    for (size_t i = 0; i < s.size(); i++)
        if (!s[i].analyzed)
            G.aq.push_back(std::make_pair(src, (int)i));
    LeaveCriticalSection(&G.acs);
    SetEvent(G.aEv);
}

/* perf-note counters (opt run 7): written only by the analyzer thread,
 * read once at exit after the thread has been joined */
static unsigned g_parses = 0, g_cacheHits = 0;

static DWORD WINAPI analyzer_main(LPVOID)
{
    /* beneath the UI/audio threads: on slow boxes the music and the
     * interface always win the CPU against background parsing */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    /* one append handle for the whole session instead of an open/close per
     * parse; flushed after each entry so a crash still keeps the cache.
     * acache_rewrite() only runs after this thread is joined, so nobody
     * else touches the file while the handle is open. */
    FILE *cacheF = NULL;
    for (;;) {
        std::pair<int, int> job(-1, -1);
        for (;;) {
            EnterCriticalSection(&G.acs);
            /* quit wins over queued work: otherwise a backlog keeps the
             * thread parsing past the exit wait, into static destruction */
            if (G.aquit) {
                LeaveCriticalSection(&G.acs);
                if (cacheF)
                    fclose(cacheF);
                return 0;
            }
            /* background mode (unfocused/minimized): do no non-audio work;
             * the queue simply keeps filling until the app is used again */
            if (!G.anaPause && !G.aq.empty()) {
                job = G.aq.front();
                G.aq.pop_front();
                LeaveCriticalSection(&G.acs);
                break;
            }
            bool quit = G.aquit;
            LeaveCriticalSection(&G.acs);
            if (quit) {
                if (cacheF)
                    fclose(cacheF);
                return 0;
            }
            WaitForSingleObject(G.aEv, 100);
        }
        /* copy what we need under the lock; the UI thread may clear/resize
         * the vector at any time (rescan, playlist edit). The reference must
         * be (re-)fetched INSIDE the lock: a playlist delete can invalidate
         * any vector reference taken before it. */
        int idx = job.second;
        std::string path, title;
        EnterCriticalSection(&G.acs);
        std::vector<Track> &s = G.srcBy(job.first);
        bool ok = idx >= 0 && idx < (int)s.size();
        if (ok) {
            path = s[idx].path;
            title = s[idx].title;
        }
        LeaveCriticalSection(&G.acs);
        if (!ok)
            continue;
        bool loaded = false;
        double dur = -1;
        std::string fmt, trk, modTitle;
        int ch = 0, ss = 1;
        /* cache hit (same path/size/mtime)? then skip the module I/O+parse */
        unsigned long fsz = 0, fhi = 0, flo = 0;
        bool haveStat = stat_file(path, fsz, fhi, flo);
        bool cached = false;
        if (haveStat) {
            std::map<std::string, AnaRec>::iterator ci =
                g_acache.find(m95_lower(path));
            if (ci != g_acache.end() && ci->second.sz == fsz &&
                ci->second.mhi == fhi && ci->second.mlo == flo) {
                const AnaRec &r = ci->second;
                loaded = !r.broken;
                dur = r.dur;
                fmt = r.fmt;
                trk = r.trk;
                modTitle = r.mt;
                ch = r.ch;
                ss = r.ss;
                cached = true;
            }
        }
        g_parses++;
        if (cached)
            g_cacheHits++;
        if (!cached) {
            bool transient = false; /* read error / out of memory: retry later */
            try {
                MptModule m;
                std::string err;
                if (m.loadFile(path, 1, &err, &transient)) {
                    dur = m.duration();
                    fmt = m95_lower(m.meta("type"));
                    if (fmt.empty())
                        fmt = m95_lower(m.meta("format"));
                    trk = m.meta("tracker");
                    modTitle = m.meta("title");
                    ch = m.channels();
                    ss = std::max(1, m.subsongs());
                    loaded = true;
                }
            } catch (const std::exception &e) {
                postlog("analyzer exception (%s): %s", title.c_str(), e.what());
                transient = true;
            } catch (...) {
                postlog("analyzer exception (%s): unknown", title.c_str());
                transient = true;
            }
            if (haveStat && !transient) {
                AnaRec r;
                r.sz = fsz;
                r.mhi = fhi;
                r.mlo = flo;
                r.dur = dur;
                r.ch = ch;
                r.ss = ss;
                r.broken = !loaded;
                r.fmt = fmt;
                r.trk = trk;
                r.mt = modTitle;
                std::string key = m95_lower(path);
                acache_put(key, r);
                if (!cacheF)
                    cacheF = fopen(acache_path().c_str(), "a");
                if (cacheF) {
                    acache_write(cacheF, key, r);
                    fflush(cacheF);
                }
            }
        }
        /* ship the findings to the UI thread as a message payload: this
         * thread must not write Track fields - the interface reads several
         * of them lock-free. Ownership passes with the message. */
        AnaResult *ar;
        try {
            ar = new AnaResult;
        } catch (...) {
            continue; /* heap exhausted: skip the UI update, keep analyzing */
        }
        ar->src = job.first;
        ar->idx = idx;
        ar->valid = true;
        ar->dur = dur;
        ar->ch = ch;
        ar->ss = ss;
        ar->loaded = loaded;
        ar->path = path;
        ar->fmt = fmt;
        ar->trk = trk;
        ar->mt = modTitle;
        if (!PostMessageA(G.hwnd, WM_APP_ANALYZED, 0, (LPARAM)ar))
            delete ar; /* post queue full - drop the payload, not leak it */
    }
    return 0;
}

/* ---------------- playback ---------------- */
static AudioSettings as_from_set()
{
    AudioSettings a;
    a.rate = G.set.rate;
    a.interpolation = G.set.interpolation;
    a.bufferMs = G.set.bufferMs;
    a.bufCount = G.set.bufCount;
    return a;
}

static std::string disp_mod_title(const Track &t)
{
    return t.modTitle.empty() ? t.title : t.modTitle;
}

/* window caption content, per the Settings choice */
static std::string caption_for(const Track &t)
{
    return G.set.titleMode == 1 ? m95_basename(t.path) : disp_mod_title(t);
}

static Track *current_track()
{
    /* playSrc ranges over library(0), favorites(1) and playlists (2+);
     * srcBy() below validates the index */
    if (G.playSrc < 0)
        return nullptr;
    std::vector<Track> &s = G.srcBy(G.playSrc);
    if (G.playIdx < 0 || G.playIdx >= (int)s.size())
        return nullptr;
    return &s[G.playIdx];
}

static void trk_push(bool on); /* defined with the tracker view below */

/* seek bar range in 1/10 s. TBM_SETRANGE packs min/max into 16-bit words
 * (wraps after ~109 min); TBM_SETRANGEMIN/MAX take a full LONG and exist
 * in Win95's comctl32 too */
static void set_seek_range(double dur)
{
    LONG range = (dur > 0 && dur < 1e6) ? (LONG)(dur * 10) : 0;
    if (range == (LONG)SendMessageA(G.hSeek, TBM_GETRANGEMAX, 0, 0))
        return;
    SendMessageA(G.hSeek, TBM_SETRANGEMIN, FALSE, 0);
    SendMessageA(G.hSeek, TBM_SETRANGEMAX, TRUE, range);
}

/* subsong drop-down: 1..ss, cur selected */
static void fill_subsongs(int ss, int cur)
{
    ss = std::max(1, std::min(ss, 256));
    if ((int)SendMessageA(G.hSub, CB_GETCOUNT, 0, 0) != ss) {
        SendMessageA(G.hSub, CB_RESETCONTENT, 0, 0);
        for (int i = 1; i <= ss; i++) {
            char n[16];
            sprintf(n, "%d", i);
            SendMessageA(G.hSub, CB_ADDSTRING, 0, (LPARAM)n);
        }
    }
    SendMessageA(G.hSub, CB_SETCURSEL, cur - 1, 0);
    EnableWindow(G.hSub, ss > 1);
    char ob[32];
    sprintf(ob, "of %d", ss);
    SetWindowTextA(G.hOf, ob);
}

/* startPaused/startPos: session resume (the song sits paused at startPos
 * without any of its start reaching the speakers) */
static void load_current(int subsong, double startPos = 0, bool startPaused = false)
{
    Track *t = current_track();
    if (!t)
        return;
    DWORD at = GetFileAttributesA(t->path.c_str());
    if (at == 0xFFFFFFFF || (at & FILE_ATTRIBUTE_DIRECTORY)) {
        /* the marker already moved here: stop whatever played before, so
         * marker, title, status and the saved session all agree */
        G.engine.stop();
        bool dir = at != 0xFFFFFFFF;
        logline("%s: %s", dir ? "Not a file" : "File not found", t->path.c_str());
        std::string st = std::string(dir ? "Not a file: " : "File missing: ") +
                         m95_basename(t->path);
        SendMessageA(G.hStatus, SB_SETTEXTA, 0, (LPARAM)st.c_str());
        SetWindowTextA(G.hTitle, m95_basename(t->path).c_str());
        EnterCriticalSection(&G.acs);
        t->broken = true;
        LeaveCriticalSection(&G.acs);
        refresh_titles();
        update_row(G.playSrc, G.playIdx);
        return;
    }
    G.curSub = subsong;
    refresh_titles();
    G.trkChOff = 0; /* channel paging starts at the first channels per song */
    G.engine.load(t->path, subsong, as_from_set(), startPos, startPaused);
    if (G.set.tracker)
        trk_push(true); /* the engine still has the last song's channel page */
    SetWindowTextA(G.hTitle, disp_mod_title(*t).c_str());
    std::string cap = "modjuke95 - " + caption_for(*t); /* no fixed-size cap */
    SetWindowTextA(G.hwnd, cap.c_str());
    char buf[512];
    fill_subsongs(t->ss, subsong);
    snprintf(buf, sizeof(buf), "Format: %s", t->fmt.empty() ? "?" : t->fmt.c_str());
    SetWindowTextA(G.hFmt, buf);
    snprintf(buf, sizeof(buf), "Tracker: %s", t->tracker.empty() ? "?" : t->tracker.c_str());
    SetWindowTextA(G.hTrk, buf);
    /* first guess from the analysis (subsong 1); refresh_ui switches to the
     * loaded subsong's real length as soon as the engine has it */
    snprintf(buf, sizeof(buf), "Length: %s", m95_fmt_time(t->dur).c_str());
    SetWindowTextA(G.hLen, buf);
    SetWindowTextA(G.hDur, m95_fmt_time(t->dur).c_str());
    set_seek_range(t->dur);
}

static void play_view_row(int row)
{
    if (row < 0 || row >= (int)G.view.size())
        return;
    G.playSrc = G.srcSel;
    G.playIdx = G.view[row];
    load_current(1);
}

static void scroll_to_playing()
{
    if (G.playSrc != G.srcSel)
        return;
    int n = ListView_GetItemCount(G.hList);
    for (int r = 0; r < n; r++) {
        LVITEMA it;
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_PARAM;
        it.iItem = r;
        if (ListView_GetItem(G.hList, &it) && (int)it.lParam == G.playIdx) {
            ListView_EnsureVisible(G.hList, r, FALSE);
            return;
        }
    }
}

static void cmd_playpause()
{
    EngineSnap s = G.engine.snap();
    if (s.playing) {
        G.engine.pauseToggle();
        return;
    }
    if (s.loaded && s.paused) {
        G.engine.pauseToggle();
        return;
    }
    if (s.loaded) { // ended: replay
        load_current(G.curSub);
        return;
    }
    int sel = ListView_GetNextItem(G.hList, -1, LVNI_SELECTED);
    if (sel < 0)
        sel = 0;
    play_view_row(sel);
}

/* next/prev always follow the *current* view order: alphabetical order steps
 * alphabetically, shuffle order steps through the shuffled queue */
static void advance(int dir)
{
    if (G.view.empty())
        return;
    int cur = -1;
    if (G.playSrc == G.srcSel)
        for (size_t i = 0; i < G.view.size(); i++)
            if (G.view[i] == G.playIdx) {
                cur = (int)i;
                break;
            }
    int np;
    if (cur < 0)
        np = dir > 0 ? 0 : (int)G.view.size() - 1;
    else
        np = cur + dir;
    if (np < 0)
        np = (int)G.view.size() - 1; /* going back always wraps, like the original */
    if (np >= (int)G.view.size()) {
        if (!G.set.repeat) {
            /* like the original: do NOT stop the current song, just advise */
            SendMessageA(G.hStatus, SB_SETTEXTA, 0,
                         (LPARAM)"End of the queue - enable 'Repeat queue' (R) to start over");
            return;
        }
        if (dir > 0 && G.set.order == ORDER_SHUFFLE && G.playSrc == G.srcSel && cur >= 0 &&
            G.view.size() > 1) {
            /* queue ends in shuffle mode: draw a new order that does NOT open
             * with the song that just ended, then play its first entry */
            draw_shuffle_now(-1, G.playIdx);
            rebuild_view();
            np = 0;
        } else {
            np = 0;
        }
    }
    G.playSrc = G.srcSel;
    G.playIdx = G.view[np];
    load_current(1);
    scroll_to_playing();
}

/* previous button, like the original: more than 3s into a song restarts it.
 * The rule uses the HEARD position, not the render position (which runs a
 * buffer queue ahead of the speakers). */
static void cmd_prev()
{
    EngineSnap s = G.engine.snap();
    double p = s.heard > 0 ? s.heard : s.pos;
    if (s.playing && !s.paused && p > 3.0) {
        G.engine.seekTo(0.0);
        return;
    }
    advance(-1);
}

/* "Shuffle now" (button / Ctrl+S): brand new order, playing song first (or
 * the selected row when nothing plays) */
static void cmd_shufflenow()
{
    int first = -1;
    if (G.playSrc == G.srcSel)
        first = G.playIdx;
    if (first < 0) {
        int sel = ListView_GetNextItem(G.hList, -1, LVNI_SELECTED);
        if (sel >= 0) {
            LVITEMA it;
            memset(&it, 0, sizeof(it));
            it.mask = LVIF_PARAM;
            it.iItem = sel;
            if (ListView_GetItem(G.hList, &it))
                first = (int)it.lParam;
        }
    }
    draw_shuffle_now(first, -1);
    if (G.set.order != ORDER_SHUFFLE) {
        G.set.order = ORDER_SHUFFLE;
        SendMessageA(G.hOrder, CB_SETCURSEL, ORDER_SHUFFLE, 0);
    }
    rebuild_view();
    scroll_to_playing();
    logline("New shuffle order drawn (Ctrl+S)");
}

/* the engine couldn't play the current track: like the original, the
 * queue stops there (no auto-skip); the marker stays on the track, a load
 * failure marks it broken, and the status says why */
static void on_play_failed(int reason)
{
    Track *t = current_track();
    std::string name = t ? m95_basename(t->path) : std::string();
    if (reason == 1 && t) {
        EnterCriticalSection(&G.acs);
        t->broken = true;
        LeaveCriticalSection(&G.acs);
        update_row(G.playSrc, G.playIdx);
    }
    std::string st = reason == 1 ? "Load failed: " + name : std::string("No sound output");
    SendMessageA(G.hStatus, SB_SETTEXTA, 0, (LPARAM)st.c_str());
    refresh_titles();
}

static void on_ended()
{
    Track *t = current_track();
    if (t && G.set.playAll && G.curSub < t->ss) {
        load_current(G.curSub + 1);
        return;
    }
    if (G.set.loop && t) {
        load_current(G.curSub);
        return;
    }
    advance(1);
}

/* ---------------- playlists / favorites / ignore ---------------- */

/* G.ign is kept sorted (lowercased paths), so this is a binary search:
 * it runs once per track on every rebuild_view */
static bool path_ignored(const std::string &path)
{
    std::string lp = m95_lower(path);
    return std::binary_search(G.ign.begin(), G.ign.end(), lp);
}

static bool vec_has_path(const std::vector<Track> &v, const std::string &path)
{
    for (size_t i = 0; i < v.size(); i++)
        if (v[i].path == path)
            return true;
    return false;
}

/* drop duplicate paths (case-insensitive, like lib_has_path), keeping the
 * first occurrence; call with G.acs held */
/* drop duplicate paths (case-insensitive, like the add-file checks),
 * keeping the first occurrence; call with G.acs held. One pass over a
 * seen-set of lowered paths: the old version re-lowercased every earlier
 * entry for every entry, which went quadratic on big drops. */
static void dedup_lib()
{
    std::set<std::string> seen;
    size_t w = 0;
    for (size_t i = 0; i < G.lib.size(); i++) {
        if (seen.insert(m95_lower(G.lib[i].path)).second) {
            if (w != i)
                G.lib[w] = G.lib[i];
            w++;
        }
    }
    G.lib.resize(w);
}

static void rebuild_sources()
{
    SendMessageA(G.hSrc, CB_RESETCONTENT, 0, 0);
    SendMessageA(G.hSrc, CB_ADDSTRING, 0, (LPARAM) "Library");
    char fb[64];
    sprintf(fb, "Favorites (%u)", (unsigned)G.fav.size());
    SendMessageA(G.hSrc, CB_ADDSTRING, 0, (LPARAM)fb);
    for (size_t i = 0; i < G.pls.size(); i++)
        SendMessageA(G.hSrc, CB_ADDSTRING, 0, (LPARAM)G.pls[i].name.c_str());
    if (G.srcSel < 0 || G.srcSel >= 2 + (int)G.pls.size())
        G.srcSel = 0;
    SendMessageA(G.hSrc, CB_SETCURSEL, G.srcSel, 0);
}

static char g_input[256];

static INT_PTR CALLBACK InputProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_INITDIALOG:
        SendDlgItemMessageA(h, IDC_INPUT_TEXT, EM_LIMITTEXT, 120, 0);
        SetDlgItemTextA(h, IDC_INPUT_TEXT, g_input);
        SendDlgItemMessageA(h, IDC_INPUT_TEXT, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(h, IDC_INPUT_TEXT));
        return 0;
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            GetDlgItemTextA(h, IDC_INPUT_TEXT, g_input, sizeof(g_input));
            EndDialog(h, 1);
            return 1;
        }
        if (LOWORD(w) == IDCANCEL) {
            EndDialog(h, 0);
            return 1;
        }
        break;
    }
    return 0;
}

/* playlist names: trimmed, no surrounding quotes (the ini API strips
 * those on reload), at most 120 chars, never empty, unique (case-
 * insensitive) - a duplicate would share the other list's shuffle plan */
static std::string unique_pl_name(const std::string &want, int except)
{
    std::string n = m95_trim(want);
    while (!n.empty() && (n[0] == '"' || n[n.size() - 1] == '"')) {
        n = m95_trim(n[0] == '"' ? n.substr(1) : n.substr(0, n.size() - 1));
    }
    if (n.size() > 120)
        n = m95_trim(n.substr(0, 120));
    if (n.empty())
        n = "Playlist";
    std::string cand = n;
    for (int k = 2;; k++) {
        bool clash = false;
        for (size_t i = 0; i < G.pls.size() && !clash; i++)
            clash = (int)i != except && m95_lower(G.pls[i].name) == m95_lower(cand);
        if (!clash)
            return cand;
        char suf[16];
        sprintf(suf, " (%d)", k);
        cand = n + suf;
    }
}

static bool input_box(const char *defval)
{
    strncpy(g_input, defval ? defval : "", sizeof(g_input) - 1);
    g_input[sizeof(g_input) - 1] = 0;
    return DialogBoxParamA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDD_INPUT), G.hwnd,
                           InputProc, 0) == 1 &&
           g_input[0] != 0;
}

/* append selected rows of the current list to dst, skipping duplicates */
static int add_selected_to(std::vector<Track> &dst)
{
    std::vector<Track> picks;
    EnterCriticalSection(&G.acs);
    std::vector<Track> &s = G.src();
    int sel = -1;
    while ((sel = ListView_GetNextItem(G.hList, sel, LVNI_SELECTED)) >= 0) {
        LVITEMA it;
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_PARAM;
        it.iItem = sel;
        if (ListView_GetItem(G.hList, &it) && it.lParam >= 0 &&
            it.lParam < (LPARAM)s.size())
            picks.push_back(s[it.lParam]);
    }
    int added = 0;
    for (size_t i = 0; i < picks.size(); i++)
        if (!vec_has_path(dst, picks[i].path)) {
            dst.push_back(picks[i]);
            added++;
        }
    LeaveCriticalSection(&G.acs);
    return added;
}

/* Persist every list right away so that a crash, a task kill or a VM reset
 * cannot lose favorites/playlists/ignored entries added since the last run;
 * also called once on clean exit. */
static void save_all_lists()
{
    EnterCriticalSection(&G.acs);
    std::string dir = m95_exe_dir();
    std::string ini = dir + "modjuke95.ini";
    /* every file goes through temp + swap (m3u_export), and nothing is
     * deleted before its replacement exists: a crash, power loss or full
     * disk at any point leaves the previous complete set */
    bool ok = m3u_export(dir + "last.m3u", G.lib);
    ok = m3u_export(dir + "favorites.m3u", G.fav) && ok;
    for (size_t i = 0; i < G.pls.size(); i++) {
        char fn[64];
        sprintf(fn, "playlist-%d.m3u", (int)i + 1);
        ok = m3u_export(dir + fn, G.pls[i].tr) && ok;
    }
    char nb[16];
    sprintf(nb, "%u", (unsigned)G.pls.size());
    WritePrivateProfileStringA("playlists", "count", nb, ini.c_str());
    for (size_t i = 0; i < G.pls.size(); i++) {
        char key[16];
        sprintf(key, "name%d", (int)i + 1);
        WritePrivateProfileStringA("playlists", key, G.pls[i].name.c_str(),
                                   ini.c_str());
    }
    /* files of playlists that no longer exist (after a delete) */
    for (int i = (int)G.pls.size() + 1;; i++) {
        char fn[64];
        sprintf(fn, "playlist-%d.m3u", i);
        if (!DeleteFileA((dir + fn).c_str()))
            break;
    }
    std::string ignf = dir + "ignored.txt";
    FILE *fo = m95_tmp_open(ignf);
    if (fo) {
        for (size_t i = 0; i < G.ign.size(); i++)
            fprintf(fo, "%s\n", G.ign[i].c_str());
        ok = m95_tmp_commit(fo, ignf) && ok;
    } else {
        ok = false;
    }
    if (!ok)
        logline("Could not save the lists (disk full or write-protected?)");
    LeaveCriticalSection(&G.acs);
}

static void fav_add_selected()
{
    int added = add_selected_to(G.fav);
    if (added) {
        logline("Added %d track(s) to favorites", added);
        rebuild_sources();
        if (G.srcSel == 1)
            rebuild_view();
        queue_analysis(1);
        save_all_lists();
    } else {
        logline("Favorites: nothing added (no selection or already present)");
    }
}

/* remove selected rows from the active list (favorites or a playlist) */
static void remove_selected_from_current()
{
    if (G.srcSel < 1)
        return;
    std::vector<int> rows;
    int sel = -1;
    while ((sel = ListView_GetNextItem(G.hList, sel, LVNI_SELECTED)) >= 0) {
        LVITEMA it;
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_PARAM;
        it.iItem = sel;
        if (ListView_GetItem(G.hList, &it))
            rows.push_back((int)it.lParam);
    }
    if (rows.empty())
        return;
    std::sort(rows.begin(), rows.end());
    bool hadPlaying = G.playSrc == G.srcSel;
    EnterCriticalSection(&G.acs);
    std::vector<Track> &s = G.src();
    std::string playPath;
    bool havePlay = G.playSrc == G.srcSel && G.playIdx >= 0 && G.playIdx < (int)s.size();
    if (havePlay)
        playPath = s[G.playIdx].path;
    for (int i = (int)rows.size() - 1; i >= 0; i--)
        if (rows[i] >= 0 && rows[i] < (int)s.size())
            s.erase(s.begin() + rows[i]);
    if (havePlay) {
        G.playIdx = -1;
        for (size_t i = 0; i < s.size(); i++)
            if (s[i].path == playPath) {
                G.playIdx = (int)i;
                break;
            }
        if (G.playIdx < 0)
            G.playSrc = -1; /* keep the marker pair consistent */
    }
    LeaveCriticalSection(&G.acs);
    if (hadPlaying && havePlay && G.playIdx < 0) {
        G.engine.stop();
        clear_playing();
        SendMessageA(G.hStatus, SB_SETTEXTA, 0, (LPARAM)"Stopped");
    }
    rebuild_sources();
    rebuild_view();
    refresh_count();
    queue_analysis(G.srcSel); /* re-index this list's pending jobs */
    logline("Removed %u track(s)", (unsigned)rows.size());
    save_all_lists();
}

static void ignore_selected()
{
    int n = 0;
    bool favChanged = false;
    std::string playPath;
    /* snapshot the current queue order (by path, stable keys) so the
     * skip-after-ignore walk below survives favorites purges and the
     * view rebuild; decided before anything is mutated */
    bool sameSrc = G.playSrc >= 0 && G.playSrc == G.srcSel;
    std::vector<std::string> oldPaths;
    if (sameSrc) {
        std::vector<Track> &s0 = G.src();
        for (size_t i = 0; i < G.view.size(); i++)
            if (G.view[i] >= 0 && G.view[i] < (int)s0.size())
                oldPaths.push_back(s0[G.view[i]].path);
    }
    {
        Track *pt = current_track();
        if (pt)
            playPath = pt->path;
    }
    EnterCriticalSection(&G.acs);
    std::vector<Track> &s = G.src();
    /* collect the selected paths first: in the favorites view `s` IS
     * G.fav, so erasing below would shift the indexes of the remaining
     * selected rows onto other tracks */
    std::vector<std::string> picked;
    int sel = -1;
    while ((sel = ListView_GetNextItem(G.hList, sel, LVNI_SELECTED)) >= 0) {
        LVITEMA it;
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_PARAM;
        it.iItem = sel;
        if (ListView_GetItem(G.hList, &it) && it.lParam >= 0 &&
            it.lParam < (LPARAM)s.size())
            picked.push_back(m95_lower(s[it.lParam].path));
    }
    for (size_t pi = 0; pi < picked.size(); pi++) {
        {
            const std::string &lp = picked[pi];
            bool dup = false;
            for (size_t i = 0; i < G.ign.size(); i++)
                if (G.ign[i] == lp)
                    dup = true;
            if (!dup) {
                G.ign.push_back(lp);
                n++;
            }
            /* ignoring also drops the track from favorites so the
             * "Favorites (N)" count stays truthful */
            for (size_t f = 0; f < G.fav.size();) {
                if (m95_lower(G.fav[f].path) == lp) {
                    G.fav.erase(G.fav.begin() + f);
                    favChanged = true;
                } else {
                    f++;
                }
            }
        }
    }
    std::sort(G.ign.begin(), G.ign.end()); /* path_ignored binary-searches */
    /* erasing may have shifted the playing marker when playing from the
     * favorites source - re-locate it by path */
    if (G.playSrc == 1) {
        std::string pp;
        if (G.playIdx >= 0 && G.playIdx < (int)G.fav.size())
            pp = G.fav[G.playIdx].path;
        G.playIdx = -1;
        for (size_t i = 0; i < G.fav.size(); i++)
            if (G.fav[i].path == pp) {
                G.playIdx = (int)i;
                break;
            }
        if (G.playIdx < 0 && !pp.empty())
            G.playSrc = -1; /* the playing entry was purged */
    }
    LeaveCriticalSection(&G.acs);
    if (!n && !favChanged) {
        logline("Ignore: no selection");
        return;
    }
    /* if the playing song was just ignored, skip away from it immediately
     * (no matter which source is currently being viewed); if the queue has
     * nothing left to play, stop - including the size-1 queue case */
    bool playingHit = !playPath.empty() && path_ignored(playPath);
    rebuild_view();
    if (favChanged) {
        rebuild_sources();
        queue_analysis(1); /* favorites indexes shifted */
    }
    if (playingHit) {
        int nextIdx = -1;
        std::vector<Track> &s = G.src();
        if (!G.view.empty()) {
            int pos = -1;
            if (sameSrc)
                for (size_t i = 0; i < oldPaths.size(); i++)
                    if (oldPaths[i] == playPath) {
                        pos = (int)i;
                        break;
                    }
            if (pos >= 0) {
                /* walk forward from the ignored song in the old order,
                 * honoring repeat; entries may be gone/ignored now */
                for (size_t step = 1; step <= oldPaths.size(); step++) {
                    size_t raw = pos + step;
                    if (raw >= oldPaths.size()) {
                        if (!G.set.repeat)
                            break; /* end of queue, no repeat -> stop */
                        raw %= oldPaths.size();
                    }
                    const std::string &cp = oldPaths[raw];
                    if (cp == playPath || path_ignored(cp))
                        continue;
                    for (size_t j = 0; j < s.size(); j++)
                        if (s[j].path == cp) {
                            nextIdx = (int)j;
                            break;
                        }
                    if (nextIdx >= 0)
                        break;
                }
            } else {
                /* playing song wasn't part of this queue: start at its head */
                nextIdx = G.view[0];
            }
        }
        if (nextIdx >= 0) {
            G.playSrc = G.srcSel;
            G.playIdx = nextIdx;
            load_current(1);
            scroll_to_playing();
            logline("Playing song ignored - skipped to next");
        } else {
            G.engine.stop();
            clear_playing();
            SendMessageA(G.hStatus, SB_SETTEXTA, 0, (LPARAM)"Stopped");
            logline("Playing song ignored - nothing to play next");
        }
    } else {
        logline("Ignored %d track(s) - see Playlists > Manage ignored tracks", n);
    }
    if (n || favChanged)
        save_all_lists();
}

/* popup listing add targets; ids: 6000 = favorites, 6001+i = playlist i */
static HMENU build_addto_menu()
{
    HMENU m = CreatePopupMenu();
    AppendMenuA(m, MF_STRING, 6000, "Favorites");
    if (!G.pls.empty())
        AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    for (size_t i = 0; i < G.pls.size(); i++) {
        std::string label; /* '&' would be taken as a mnemonic */
        for (size_t k = 0; k < G.pls[i].name.size(); k++) {
            if (G.pls[i].name[k] == '&')
                label += '&';
            label += G.pls[i].name[k];
        }
        AppendMenuA(m, MF_STRING, 6001 + (UINT)i, label.c_str());
    }
    return m;
}

static void do_addto(int id)
{
    int dstSel = -1;
    if (id == 6000)
        dstSel = 1;
    else if (id >= 6001 && id - 6001 < (int)G.pls.size())
        dstSel = 2 + (id - 6001);
    if (dstSel < 0)
        return;
    int added = add_selected_to(G.srcBy(dstSel));
    if (added) {
        logline("Added %d track(s) to %s", added,
                dstSel == 1 ? "favorites" : G.pls[dstSel - 2].name.c_str());
        if (G.srcSel == dstSel)
            rebuild_view();
        if (dstSel == 1)
            rebuild_sources(); /* "Favorites (N)" */
        queue_analysis(dstSel);
        refresh_count();
        save_all_lists();
    } else {
        logline("Nothing added (no selection or already present)");
    }
}

static void fill_ign_list(HWND h)
{
    HWND lb = GetDlgItem(h, IDC_IGN_LIST);
    SendMessageA(lb, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < G.ign.size(); i++)
        SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM)G.ign[i].c_str());
}

static INT_PTR CALLBACK IgnoredProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_INITDIALOG:
        fill_ign_list(h);
        return 1;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_IGN_REMOVE: {
            int c = (int)SendDlgItemMessageA(h, IDC_IGN_LIST, LB_GETCURSEL, 0, 0);
            if (c == LB_ERR)
                break;
            /* erase by the selected text, not by index: never trust that
             * the listbox order equals G.ign's */
            int len = (int)SendDlgItemMessageA(h, IDC_IGN_LIST, LB_GETTEXTLEN, c, 0);
            if (len <= 0)
                break;
            std::string sel((size_t)len + 1, '\0');
            SendDlgItemMessageA(h, IDC_IGN_LIST, LB_GETTEXT, c, (LPARAM)&sel[0]);
            sel.resize(strlen(sel.c_str()));
            std::string p;
            EnterCriticalSection(&G.acs);
            std::vector<std::string>::iterator it = std::find(G.ign.begin(), G.ign.end(), sel);
            if (it != G.ign.end()) {
                p = *it;
                G.ign.erase(it);
            }
            LeaveCriticalSection(&G.acs);
            if (!p.empty()) {
                fill_ign_list(h);
                rebuild_view();
                logline("Unignored: %s", p.c_str());
                save_all_lists();
            }
            break;
        }
        case IDC_IGN_CLEARALL:
            EnterCriticalSection(&G.acs);
            G.ign.clear();
            LeaveCriticalSection(&G.acs);
            fill_ign_list(h);
            rebuild_view();
            logline("Ignore list cleared");
            save_all_lists();
            break;
        case IDOK:
        case IDCANCEL:
            EndDialog(h, 0);
            return 1;
        }
        return 0;
    }
    return 0;
}

/* belt and braces: handle double click by subclassing the listview itself,
 * in case NM_DBLCLK notifications are unreliable on this comctl32 */
static WNDPROC g_lvProc = NULL;
static void trk_sync_now(); /* defined with the tracker view below */

/* list row under client y: comctl32 4.0 (Win95) has no full-row select, so
 * a hit test only finds the item over column 0's label - test at an x
 * inside that label instead, so a click on any column counts */
static int lv_row_at(int y)
{
    if (ListView_GetItemCount(G.hList) <= 0)
        return -1;
    RECT rc;
    if (!ListView_GetItemRect(G.hList, ListView_GetTopIndex(G.hList), &rc, LVIR_LABEL))
        return -1;
    LV_HITTESTINFO ht;
    memset(&ht, 0, sizeof(ht));
    ht.pt.x = rc.left + 2;
    ht.pt.y = y;
    return ListView_HitTest(G.hList, &ht);
}

static LRESULT CALLBACK lv_subproc(HWND hh, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_LBUTTONDBLCLK) {
        int i = lv_row_at((short)HIWORD(l));
        if (i >= 0) {
            play_view_row(i);
            return 0;
        }
    }
    LRESULT r = CallWindowProcA(g_lvProc, hh, m, w, l);
    /* Scrolling the list pumps a tight stream of scroll/paint messages; the
     * tracker's posted row updates and WM_PAINTs get starved in that window
     * and the pattern view freezes. While the user scrolls, pull the latest
     * row synchronously and repaint at once so the view keeps following. */
    if (m == WM_VSCROLL || m == WM_HSCROLL || m == WM_MOUSEWHEEL ||
        (m == WM_KEYDOWN &&
         (w == VK_UP || w == VK_DOWN || w == VK_PRIOR || w == VK_NEXT ||
          w == VK_HOME || w == VK_END)))
        trk_sync_now();
    return r;
}

/* Seek and volume trackbars jump straight to the clicked spot, like the
 * original's JumpSlider (the stock trackbar only pages toward the click).
 * Mouse handling is done here, the parent gets the usual WM_HSCROLL codes:
 * TB_THUMBPOSITION on press (if the value changed) and on release (if the
 * drag moved it), TB_THUMBTRACK while dragging, TB_ENDTRACK at the end. */
static WNDPROC g_tbProc = NULL;
static HWND g_tbDrag = NULL;     /* trackbar with an active mouse drag */
static int g_tbGrab = 0;         /* grab offset when pressed on the thumb */
static LONG g_tbPressVal = 0;

static LONG tb_value_at(HWND h, int x)
{
    RECT ch, th;
    SendMessageA(h, TBM_GETCHANNELRECT, 0, (LPARAM)&ch);
    SendMessageA(h, TBM_GETTHUMBRECT, 0, (LPARAM)&th);
    LONG lo = (LONG)SendMessageA(h, TBM_GETRANGEMIN, 0, 0);
    LONG hi = (LONG)SendMessageA(h, TBM_GETRANGEMAX, 0, 0);
    /* same mapping as comctl32's own thumb placement: the thumb center
     * travels from channel.left + thumb/2 to channel.right - thumb/2 */
    int half = (th.right - th.left) / 2;
    int width = ch.right - ch.left - 2 * half - 1;
    if (hi <= lo || width <= 0)
        return lo;
    double v = (double)(hi - lo) * (x - g_tbGrab - ch.left - half) / width;
    LONG r = lo + (LONG)(v + (v < 0 ? -0.5 : 0.5));
    return r < lo ? lo : (r > hi ? hi : r);
}

static void tb_notify(HWND h, int code, LONG v)
{
    SendMessageA(GetParent(h), WM_HSCROLL, MAKEWPARAM(code, (WORD)v), (LPARAM)h);
}

static LRESULT CALLBACK tb_subproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        int x = (short)LOWORD(l), y = (short)HIWORD(l);
        RECT th;
        SendMessageA(h, TBM_GETTHUMBRECT, 0, (LPARAM)&th);
        POINT pt = { x, y };
        /* pressing the thumb itself keeps the grab offset, so it doesn't jump */
        g_tbGrab = PtInRect(&th, pt) ? x - (th.left + th.right) / 2 : 0;
        LONG old = (LONG)SendMessageA(h, TBM_GETPOS, 0, 0);
        LONG v = tb_value_at(h, x);
        g_tbDrag = h;
        g_tbPressVal = v;
        SetCapture(h);
        if (v != old) {
            SendMessageA(h, TBM_SETPOS, TRUE, v);
            tb_notify(h, TB_THUMBPOSITION, v);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_tbDrag == h && (w & MK_LBUTTON)) {
            LONG v = tb_value_at(h, (short)LOWORD(l));
            if (v != (LONG)SendMessageA(h, TBM_GETPOS, 0, 0)) {
                SendMessageA(h, TBM_SETPOS, TRUE, v);
                tb_notify(h, TB_THUMBTRACK, v);
            }
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (g_tbDrag == h) {
            LONG v = tb_value_at(h, (short)LOWORD(l));
            SendMessageA(h, TBM_SETPOS, TRUE, v);
            g_tbDrag = NULL; /* before ReleaseCapture: WM_CAPTURECHANGED */
            ReleaseCapture();
            if (v != g_tbPressVal)
                tb_notify(h, TB_THUMBPOSITION, v);
            tb_notify(h, TB_ENDTRACK, v);
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if (g_tbDrag == h) { /* capture taken away mid-drag: commit */
            g_tbDrag = NULL;
            LONG v = (LONG)SendMessageA(h, TBM_GETPOS, 0, 0);
            if (v != g_tbPressVal)
                tb_notify(h, TB_THUMBPOSITION, v);
            tb_notify(h, TB_ENDTRACK, v);
        }
        break;
    }
    return CallWindowProcA(g_tbProc, h, m, w, l);
}

/* ---------------- state sync helpers ---------------- */
static void setLoop(bool v)
{
    G.set.loop = v;
    SendMessageA(G.hLoop, BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0);
    CheckMenuItem(GetMenu(G.hwnd), IDM_PB_LOOP, v ? MF_CHECKED : MF_UNCHECKED);
}
static void setRepeat(bool v)
{
    G.set.repeat = v;
    SendMessageA(G.hRepeat, BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0);
    CheckMenuItem(GetMenu(G.hwnd), IDM_PB_REPEAT, v ? MF_CHECKED : MF_UNCHECKED);
}
static void setPlayAll(bool v)
{
    G.set.playAll = v;
    SendMessageA(G.hPlayAll, BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0);
}

/* ---------------- library ops ---------------- */
/* Win95 comdlg32 rejects the NT5-extended OPENFILENAME (88 bytes); the
 * version-4.0 size (76) works on 95, 98 and later alike. */
#ifndef OPENFILENAME_SIZE_VERSION_400
#define OPENFILENAME_SIZE_VERSION_400 76
#endif

static void do_open_files()
{
    /* 32000: Win95's multi-select limit; ~500 names fit in the old 8 KB */
    static char buf[32000];
    memset(buf, 0, sizeof(buf));
    std::string flt = "Modules (";
    std::string pat = "";
    for (size_t i = 0; i < G.exts.size(); i++) {
        flt += "*." + G.exts[i];
        pat += "*." + G.exts[i];
        if (i + 1 < G.exts.size()) {
            flt += ";";
            pat += ";";
        }
    }
    flt += ")";
    static char filter[4096];
    char *p = filter;
    memcpy(p, flt.c_str(), flt.size() + 1);
    p += flt.size() + 1;
    memcpy(p, pat.c_str(), pat.size() + 1);
    p += pat.size() + 1;
    /* filter pairs: description \0 pattern \0 ... \0\0 */
    static const char allf[] = "All files (*.*)\0*.*\0";
    memcpy(p, allf, sizeof(allf)); /* includes the final extra NUL */

    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = OPENFILENAME_SIZE_VERSION_400;
    ofn.hwndOwner = G.hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = sizeof(buf);
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameA(&ofn)) {
        if (CommDlgExtendedError() == FNERR_BUFFERTOOSMALL)
            logline("Too many files selected at once - use Open folder instead");
        return;
    }
    std::vector<std::string> files;
    char *q = buf;
    std::string first = q;
    q += strlen(q) + 1;
    if (*q == 0)
        files.push_back(first);
    else
        while (*q) {
            files.push_back(m95_join(first, q)); /* drive root: "D:\\" */
            q += strlen(q) + 1;
        }
    unsigned added = 0, dupes = 0;
    EnterCriticalSection(&G.acs);
    /* one-time case-insensitive membership set for the whole batch: the old
     * per-file lib_has_path scan lowercased every library entry per
     * incoming file (O(files x lib) string allocations) */
    std::set<std::string> libPaths;
    for (size_t i = 0; i < G.lib.size(); i++)
        libPaths.insert(m95_lower(G.lib[i].path));
    for (size_t i = 0; i < files.size(); i++) {
        std::string lp = m95_lower(files[i]);
        if (libPaths.count(lp) || path_ignored(files[i])) {
            dupes++;
            continue;
        }
        Track t;
        t.path = files[i];
        t.title = m95_basename(files[i]);
        t.folder = m95_parentdir(files[i]);
        t.rekey();
        G.lib.push_back(t);
        libPaths.insert(lp); /* same file twice in the selection stays a dupe */
        added++;
    }
    LeaveCriticalSection(&G.acs);
    rebuild_view();
    queue_analysis(0);
    if (dupes)
        logline("Added %u file(s), skipped %u duplicate(s)", added, dupes);
    else
        logline("Added %u file(s)", added);
    if (added)
        save_all_lists();
}

static void scan_into(const std::string &dir)
{
    /* a rescan of a folder that's gone (CD ejected, drive unmapped) would
     * empty the library and save it that way */
    DWORD at = GetFileAttributesA(dir.c_str());
    if (at == 0xFFFFFFFF || !(at & FILE_ATTRIBUTE_DIRECTORY)) {
        logline("Folder not found: %s - library kept", dir.c_str());
        return;
    }
    G.lastDir = dir;
    WritePrivateProfileStringA("session", "lastdir", dir.c_str(),
                               (m95_exe_dir() + "modjuke95.ini").c_str());
    /* a rescan replaces the whole library: re-locate the playing marker by
     * path, or stop if the song vanished from the new scan */
    std::string playPath;
    bool wasLib = G.playSrc == 0;
    if (wasLib) {
        Track *t = current_track();
        if (t)
            playPath = t->path;
    }
    bool gone = false;
    EnterCriticalSection(&G.acs);
    G.lib.clear();
    scan_dir(dir, G.exts, G.lib);
    /* ignored files never show up again */
    {
        size_t w = 0;
        for (size_t i = 0; i < G.lib.size(); i++)
            if (!path_ignored(G.lib[i].path))
                G.lib[w++] = G.lib[i];
        G.lib.resize(w);
    }
    if (wasLib && !playPath.empty()) {
        G.playIdx = -1;
        for (size_t i = 0; i < G.lib.size(); i++)
            if (G.lib[i].path == playPath) {
                G.playIdx = (int)i;
                break;
            }
        gone = G.playIdx < 0;
        if (gone)
            G.playSrc = -1;
    }
    LeaveCriticalSection(&G.acs);
    if (gone) {
        G.engine.stop();
        clear_playing();
        SendMessageA(G.hStatus, SB_SETTEXTA, 0, (LPARAM)"Playing song left the library - stopped");
    }
    rebuild_view();
    queue_analysis(0);
    logline("Scanned %s: %u tracks", dir.c_str(), (unsigned)G.lib.size());
    save_all_lists();
}

static void do_open_folder()
{
    char path[MAX_PATH] = "";
    BROWSEINFOA bi;
    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = G.hwnd;
    bi.pszDisplayName = path;
    bi.lpszTitle = "Choose a module folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (!pidl)
        return;
    if (SHGetPathFromIDListA(pidl, path))
        scan_into(path);
    CoTaskMemFree(pidl);
}

/* ---------------- layout ---------------- */
static void update_vol_pct()
{
    char b[8];
    sprintf(b, "%d%%", (int)SendMessageA(G.hVol, TBM_GETPOS, 0, 0));
    SetWindowTextA(G.hVolPct, b);
}

static void trk_tab_set(bool pattern); /* defined with the tracker view below */

static void layout()
{
    RECT rc;
    GetClientRect(G.hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    SendMessageA(G.hStatus, WM_SIZE, 0, 0);
    refresh_count(); /* re-split the parts for the new width */
    int y = 4;

    MoveWindow(G.hOpenFld, 4, y, 92, 22, TRUE);
    MoveWindow(G.hRescan, 100, y, 64, 22, TRUE);
    MoveWindow(G.hSearch, 168, y + 2, W - 168 - 8 < 40 ? 40 : W - 168 - 8, 20, TRUE);
    y += 26;
    {
        /* source/order/list buttons: left to right with their usual gaps,
         * wrapping to another row when the window is too narrow (they all
         * fit in one row from 568 px) */
        struct { HWND h; int gap, w, hh; } row[] = {
            { G.hSrc, 0, 90, 200 },    { G.hOrder, 6, 100, 200 }, /* drop-down height */
            { G.hShuf, 6, 86, 22 },    { G.hFilter, 4, 60, 22 },
            { G.hAddPl, 4, 100, 22 },  { G.hClearPl, 4, 100, 22 },
        };
        int x = 4;
        for (size_t i = 0; i < sizeof(row) / sizeof(row[0]); i++) {
            if (x > 4 && x + row[i].gap + row[i].w > W - 4) {
                x = 4;
                y += 26;
            } else {
                x += x > 4 ? row[i].gap : 0;
            }
            MoveWindow(row[i].h, x, y, row[i].w, row[i].hh, TRUE);
            x += row[i].w;
        }
    }
    y += 26;

    int top = y;
    /* transport area: one row when wide enough, two rows on low resolutions,
     * three when even the buttons don't fit in one row */
    RECT sbr;
    GetWindowRect(G.hStatus, &sbr);
    int sbh = sbr.bottom - sbr.top;
    if (sbh <= 0 || sbh > 60)
        sbh = 22;
    int rows = (W >= 760) ? 1 : (W >= 480 ? 2 : 3);
    int ty = H - sbh - 2 - rows * 26; /* transport top */
    int bottom = ty - 4;
    if (bottom < top + 60)
        bottom = top + 60;
    /* right panel: 310 px up to a 640 px wide client (the 640x480 layout is
     * unchanged), then half of every extra pixel, so a wider window shows
     * more tracker channels (trk_push derives them from the panel width)
     * while the list keeps growing too. Below 620 px there is no room for
     * both (the list's columns need ~290 px), and picking songs matters more
     * than the info/tracker panel: the panel is hidden and the list takes
     * the whole width. A full-width window on a 640x480 screen keeps both. */
    bool hidePanel = W < 620;
    int rightw = 310;
    if (W > 640)
        rightw += (W - 640) / 2;
    int lw = hidePanel ? W - 8 : W - rightw - 12;
    MoveWindow(G.hList, 4, top, lw, bottom - top, TRUE);
    {
        /* fit all six columns into the list width (no h-scroll at 640x480) */
        int f1 = 52, f2 = 40, f3 = 36, f4 = 28, f5 = 22;
        int mw = lw - (f1 + f2 + f3 + f4 + f5) - 22;
        if (mw < 90)
            mw = 90;
        ListView_SetColumnWidth(G.hList, 0, mw);
        ListView_SetColumnWidth(G.hList, 1, f1);
        ListView_SetColumnWidth(G.hList, 2, f2);
        ListView_SetColumnWidth(G.hList, 3, f3);
        ListView_SetColumnWidth(G.hList, 4, f4);
        ListView_SetColumnWidth(G.hList, 5, f5);
    }

    /* right panel: two-page tab (info+log / full-size tracker view) */
    MoveWindow(G.hTab, W - rightw + 2, top, rightw - 10, bottom - top, TRUE);
    RECT dr = { 0, 0, rightw - 10, bottom - top };
    SendMessageA(G.hTab, TCM_ADJUSTRECT, FALSE, (LPARAM)&dr);
    int x0 = W - rightw + 2 + dr.left, w0 = dr.right - dr.left;
    int yy = top + dr.top;
    int pbot = top + dr.bottom;
    MoveWindow(G.hTitle, x0, yy, w0, 16, TRUE);
    yy += 20;
    MoveWindow(G.hFmt, x0, yy, w0, 13, TRUE); /* MS Sans Serif 8: 13 px */
    yy += 14;
    MoveWindow(G.hTrk, x0, yy, w0, 13, TRUE);
    yy += 14;
    MoveWindow(G.hLen, x0, yy, w0, 13, TRUE);
    yy += 14;
    MoveWindow(G.hPos, x0, yy, w0, 13, TRUE);
    yy += 14;
    MoveWindow(G.hSeq, x0, yy, w0, 13, TRUE);
    yy += 14;
    MoveWindow(G.hOut, x0, yy, w0, 13, TRUE);
    yy += 18;
    MoveWindow(G.hSub, x0, yy, 50, 200, TRUE);
    MoveWindow(G.hOf, x0 + 56, yy + 2, 44, 12, TRUE);
    MoveWindow(G.hPlayAll, x0 + 106, yy + 1, w0 - 106, 16, TRUE);
    yy += 20;
    /* the pattern page fills the whole tab area; on the info page the log
     * sits below the label stack */
    MoveWindow(G.hPat, x0, top + dr.top, w0, pbot - (top + dr.top), TRUE);
    MoveWindow(G.hLog, x0, yy, w0, pbot - yy, TRUE);
    if (hidePanel != G.panelHidden) {
        /* after the moves, so a shown-again tracker fetches for its size */
        G.panelHidden = hidePanel;
        trk_tab_set(G.set.tracker);
    }

    /* transport row(s); sliders absorb the leftover width */
    y = ty;
    int x = 4;
    MoveWindow(G.hPrev, x, y, 36, 24, TRUE);
    x += 40;
    MoveWindow(G.hPlay, x, y, 60, 24, TRUE);
    x += 64;
    MoveWindow(G.hNext, x, y, 36, 24, TRUE);
    x += 40;
    MoveWindow(G.hStop, x, y, 36, 24, TRUE);
    x += 44;
    if (rows == 3) {
        /* very narrow: the buttons on row 1, Loop/Repeat + volume on row 2 */
        MoveWindow(G.hMute, x, y, 52, 24, TRUE);
        y += 26;
        x = 4;
    }
    MoveWindow(G.hLoop, x, y + 4, 52, 16, TRUE);
    x += 56;
    MoveWindow(G.hRepeat, x, y + 4, 92, 16, TRUE);
    x += 96;
    if (rows != 3) {
        MoveWindow(G.hMute, x, y, 52, 24, TRUE);
        x += 56;
    }
    if (rows == 1) {
        MoveWindow(G.hVol, x, y + 2, 76, 20, TRUE);
        x += 80;
        MoveWindow(G.hVolPct, x, y + 5, 30, 14, TRUE);
        x += 34;
        MoveWindow(G.hTime, x, y + 5, 40, 14, TRUE);
        x += 44;
        int sw = W - 4 - 52 - 4 - x; /* seek gets the rest */
        if (sw < 40)
            sw = 40;
        MoveWindow(G.hSeek, x, y + 2, sw, 20, TRUE);
        MoveWindow(G.hDur, W - 4 - 52, y + 5, 52, 14, TRUE);
    } else {
        /* narrow: volume + % finish the last button row, seek gets its own
         * full row */
        MoveWindow(G.hVolPct, W - 4 - 30, y + 5, 30, 14, TRUE);
        int vw = W - 4 - 34 - 4 - x;
        if (vw < 40)
            vw = 40;
        MoveWindow(G.hVol, x, y + 2, vw, 20, TRUE);
        y += 26;
        MoveWindow(G.hTime, 4, y + 5, 40, 14, TRUE);
        int sw = W - 4 - 52 - 4 - 48;
        MoveWindow(G.hSeek, 48, y + 2, sw < 40 ? 40 : sw, 20, TRUE);
        MoveWindow(G.hDur, W - 4 - 52, y + 5, 52, 14, TRUE);
    }
}

/* ---------------- dialogs ---------------- */
/* interpolation filter lengths (render param) and their labels */
static const int kInterpVals[] = { 0, 1, 2, 4, 8 };
static const char *kInterpNames[] = { "Default (recommended)", "None (crisp, least CPU)",
                                      "Linear", "Cubic", "Sinc (smooth, most CPU)" };
/* output buffering presets: per-buffer size x count (latency vs robustness) */
static const int kBufMs[] = { 60, 100, 220, 300, 500 };
static const int kBufN[] = { 3, 4, 6, 6, 8 };
static const char *kBufNames[] = { "Minimal (~0.2 s)", "Low (~0.4 s)", "Normal (~1.3 s)",
                                   "High (~1.8 s)", "Maximum (~3 s)" }; /* 8 x 500 ms, capped at
                                                             * 16384 frames per buffer */
/* extra tracker display delay: compensates output latency beyond the app's
 * own buffers (sound card / driver / host audio stack) */
/* 50 ms steps up to 600: VMs typically need 300-600 ms and want fine tuning */
static const int kTrkDly[] = { 0, 50, 100, 150, 200, 250, 300, 350, 400,
                               450, 500, 550, 600, 700, 800, 900, 1000 };
static const char *kTrkDlyNames[] = { "None", "50 ms", "100 ms", "150 ms", "200 ms",
                                      "250 ms", "300 ms", "350 ms", "400 ms", "450 ms",
                                      "500 ms", "550 ms", "600 ms", "700 ms", "800 ms",
                                      "900 ms", "1 s" };
static const int kTrkDlyN = (int)(sizeof(kTrkDly) / sizeof(kTrkDly[0]));
/* window caption: internal module title or plain file name */
static const char *kTitleNames[] = { "Module title", "Filename" };
/* UI refresh period, shared by the info tab and the tracker view */
static const int kRefMs[] = { 50, 100, 250, 500 };
static const char *kRefNames[] = { "Smooth (50 ms)", "Fast (100 ms)",
                                   "Normal (250 ms)", "Relaxed (500 ms)" };

/* effective UI refresh rates depend on the background mode (see
 * apply_power_mode below); Settings changes and mode transitions both go
 * through here so they can't fight each other */
static void apply_rates()
{
    int ms = G.set.uiRefreshMs;
    if (G.powerMode == 1 && ms < 500)
        ms = 500; /* visible but unfocused: slow, still alive */
    if (G.powerMode == 2)
        ms = 500; /* minimized: only the mode-detection tick matters */
    SetTimer(G.hwnd, 1, ms, NULL);
    G.engine.setRefresh(ms);
}

static INT_PTR CALLBACK SettingsProc(HWND h, UINT msg, WPARAM wp, LPARAM)
{
    if (msg == WM_INITDIALOG) {
        char b[32];
        const int rates[] = { 44100, 48000, 32000, 22050, 11025 };
        for (int i = 0; i < 5; i++) {
            sprintf(b, "%d", rates[i]);
            SendDlgItemMessageA(h, IDC_SET_RATE, CB_ADDSTRING, 0, (LPARAM)b);
            if (rates[i] == G.set.rate)
                SendDlgItemMessageA(h, IDC_SET_RATE, CB_SETCURSEL, i, 0);
        }
        int isel = 0;
        for (int i = 0; i < 5; i++) {
            SendDlgItemMessageA(h, IDC_SET_INTERP, CB_ADDSTRING, 0, (LPARAM)kInterpNames[i]);
            if (kInterpVals[i] == G.set.interpolation)
                isel = i;
        }
        SendDlgItemMessageA(h, IDC_SET_INTERP, CB_SETCURSEL, isel, 0);
        int bsel = 2;
        for (int i = 0; i < 5; i++) {
            SendDlgItemMessageA(h, IDC_SET_BUFFER, CB_ADDSTRING, 0, (LPARAM)kBufNames[i]);
            if (kBufMs[i] == G.set.bufferMs && kBufN[i] == G.set.bufCount)
                bsel = i;
        }
        SendDlgItemMessageA(h, IDC_SET_BUFFER, CB_SETCURSEL, bsel, 0);
        int dsel = 0; /* nearest preset, so a hand-edited ini value survives OK */
        for (int i = 0; i < kTrkDlyN; i++) {
            SendDlgItemMessageA(h, IDC_SET_TRKDELAY, CB_ADDSTRING, 0, (LPARAM)kTrkDlyNames[i]);
            if (abs(kTrkDly[i] - G.set.trkDelayMs) < abs(kTrkDly[dsel] - G.set.trkDelayMs))
                dsel = i;
        }
        SendDlgItemMessageA(h, IDC_SET_TRKDELAY, CB_SETCURSEL, dsel, 0);
        for (int i = 0; i < 2; i++)
            SendDlgItemMessageA(h, IDC_SET_TITLE, CB_ADDSTRING, 0, (LPARAM)kTitleNames[i]);
        SendDlgItemMessageA(h, IDC_SET_TITLE, CB_SETCURSEL, G.set.titleMode == 1 ? 1 : 0, 0);
        int rsel = 1;
        for (int i = 0; i < 4; i++) {
            SendDlgItemMessageA(h, IDC_SET_REFRESH, CB_ADDSTRING, 0, (LPARAM)kRefNames[i]);
            if (kRefMs[i] == G.set.uiRefreshMs)
                rsel = i;
        }
        SendDlgItemMessageA(h, IDC_SET_REFRESH, CB_SETCURSEL, rsel, 0);
        return TRUE;
    }
    if (msg == WM_COMMAND && wp == IDOK) {
        char b[32];
        GetDlgItemTextA(h, IDC_SET_RATE, b, sizeof(b));
        G.set.rate = atoi(b);
        int ii = (int)SendDlgItemMessageA(h, IDC_SET_INTERP, CB_GETCURSEL, 0, 0);
        G.set.interpolation = kInterpVals[ii < 0 || ii > 4 ? 0 : ii];
        int bi = (int)SendDlgItemMessageA(h, IDC_SET_BUFFER, CB_GETCURSEL, 0, 0);
        if (bi < 0 || bi > 4)
            bi = 2;
        G.set.bufferMs = kBufMs[bi];
        G.set.bufCount = kBufN[bi];
        int di = (int)SendDlgItemMessageA(h, IDC_SET_TRKDELAY, CB_GETCURSEL, 0, 0);
        G.set.trkDelayMs = kTrkDly[di < 0 || di >= kTrkDlyN ? 0 : di];
        G.engine.setTrkDelay(G.set.trkDelayMs);
        G.set.titleMode =
            SendDlgItemMessageA(h, IDC_SET_TITLE, CB_GETCURSEL, 0, 0) == 1 ? 1 : 0;
        int ri = (int)SendDlgItemMessageA(h, IDC_SET_REFRESH, CB_GETCURSEL, 0, 0);
        G.set.uiRefreshMs = kRefMs[ri < 0 || ri > 3 ? 1 : ri];
        apply_rates(); /* honors the current background mode */
        Track *tc = current_track(); /* caption choice applies right away */
        if (tc) {
            std::string cap = "modjuke95 - " + caption_for(*tc);
            SetWindowTextA(G.hwnd, cap.c_str());
        }
        G.set.save();
        EndDialog(h, 1);
        return TRUE;
    }
    if (msg == WM_COMMAND && wp == IDCANCEL) {
        EndDialog(h, 0);
        return TRUE;
    }
    return FALSE;
}

static std::vector<std::string> g_ftypes;
static HWND g_ftypeCb[24];

static INT_PTR CALLBACK FilterProc(HWND h, UINT msg, WPARAM wp, LPARAM)
{
    if (msg == WM_INITDIALOG) {
        char b[32];
        sprintf(b, "%.0f", G.set.minLen);
        SetDlgItemTextA(h, IDC_FLT_MINLEN, G.set.minLen > 0 ? b : "");
        sprintf(b, "%.0f", G.set.maxLen);
        SetDlgItemTextA(h, IDC_FLT_MAXLEN, G.set.maxLen > 0 ? b : "");
        CheckDlgButton(h, IDC_FLT_PLAYABLE, G.set.playableOnly ? BST_CHECKED : BST_UNCHECKED);
        /* file-type checkboxes, created at runtime. Types already enabled
         * come first, so an active filter is always visible (and kept on
         * OK) even when no loaded track has that type; then the types of
         * every source - library, favorites, playlists */
        g_ftypes.clear();
        std::set<std::string> seen;
        {
            const std::string &ts = G.set.types;
            size_t p = 0;
            while (p < ts.size()) {
                size_t e = ts.find(';', p);
                if (e == std::string::npos)
                    e = ts.size();
                std::string t = ts.substr(p, e - p);
                if (!t.empty() && seen.insert(t).second)
                    g_ftypes.push_back(t);
                p = e + 1;
            }
        }
        EnterCriticalSection(&G.acs);
        for (int src = 0; src < 2 + (int)G.pls.size(); src++) {
            const std::vector<Track> &sv = G.srcBy(src);
            for (size_t i = 0; i < sv.size(); i++) {
                std::string e = ext_of(sv[i].path);
                if (!e.empty() && seen.insert(e).second)
                    g_ftypes.push_back(e);
            }
        }
        LeaveCriticalSection(&G.acs);
        if (g_ftypes.size() > 24) /* enabled ones are at the front */
            g_ftypes.resize(24);
        std::sort(g_ftypes.begin(), g_ftypes.end());
        /* dialog units -> pixels */
        RECT u = { 0, 0, 4, 8 };
        MapDialogRect(h, &u);
        int ux = u.right, uy = u.bottom;
        /* dynamic layout: size the dialog to the number of file types so no
         * empty space is left, and start the checkbox rows well below the
         * group box caption (y=58 used to paint right over the caption) */
        int n = (int)g_ftypes.size();
        int cols = n > 8 ? 2 : 1;
        int rows = n > 0 ? (n + cols - 1) / cols : 1;
        const int gY = 56, listTop = 64, rowH = 12;
        int groupH = (listTop - gY) + rows * rowH + 3;
        int btnY = gY + groupH + 8;
        int delta = btnY - 226; /* 226 = button row in the .rc template */
        HWND gbox = GetDlgItem(h, IDC_FLT_GROUP);
        if (gbox) {
            RECT r;
            GetWindowRect(gbox, &r);
            MapWindowPoints(NULL, h, (POINT *)&r, 2);
            MoveWindow(gbox, r.left, r.top, r.right - r.left, groupH * uy / 8, TRUE);
        }
        {
            const int bids[3] = { IDC_FLT_CLEAR, IDOK, IDCANCEL };
            for (int i = 0; i < 3; i++) {
                HWND b = GetDlgItem(h, bids[i]);
                RECT r;
                GetWindowRect(b, &r);
                MapWindowPoints(NULL, h, (POINT *)&r, 2);
                MoveWindow(b, r.left, r.top + delta * uy / 8, r.right - r.left,
                           r.bottom - r.top, TRUE);
            }
            RECT r;
            GetWindowRect(h, &r);
            int nw = r.right - r.left;
            int nh = (r.bottom - r.top) + delta * uy / 8;
            SetWindowPos(h, NULL, 0, 0, nw, nh, SWP_NOMOVE | SWP_NOZORDER);
            /* re-center over the owner after the resize */
            HWND par = GetParent(h);
            RECT pr;
            if (par && GetWindowRect(par, &pr)) {
                int x = pr.left + ((pr.right - pr.left) - nw) / 2;
                int y = pr.top + ((pr.bottom - pr.top) - nh) / 2;
                SetWindowPos(h, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            }
        }
        for (int i = 0; i < n; i++) {
            int col = i % cols, row = i / cols;
            int x = (16 + col * 92) * ux / 4;
            int y = (listTop + row * rowH) * uy / 8;
            g_ftypeCb[i] = CreateWindowExA(0, "BUTTON", g_ftypes[i].c_str(),
                                           BS_AUTOCHECKBOX | WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                           x, y, 80 * ux / 4, 12 * uy / 8, h, /* inside the box */
                                           (HMENU)(400 + (UINT)i), GetModuleHandleA(NULL), NULL);
            setfont(g_ftypeCb[i]);
            SendMessageA(g_ftypeCb[i], BM_SETCHECK,
                         types_contains(g_ftypes[i]) ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        return TRUE;
    }
    if (msg == WM_COMMAND && wp == IDC_FLT_CLEAR) {
        /* like the original: reset the criteria, then Apply picks them up */
        SetDlgItemTextA(h, IDC_FLT_MINLEN, "");
        SetDlgItemTextA(h, IDC_FLT_MAXLEN, "");
        CheckDlgButton(h, IDC_FLT_PLAYABLE, BST_UNCHECKED);
        for (size_t i = 0; i < g_ftypes.size(); i++)
            SendMessageA(g_ftypeCb[i], BM_SETCHECK, BST_UNCHECKED, 0);
        return TRUE;
    }
    if (msg == WM_COMMAND && wp == IDOK) {
        char b[32];
        /* same 0..3600 s range the settings loader accepts (a larger value
         * would silently turn into "no limit" after a restart) */
        GetDlgItemTextA(h, IDC_FLT_MINLEN, b, sizeof(b));
        G.set.minLen = std::max(0.0, std::min(3600.0, atof(b)));
        GetDlgItemTextA(h, IDC_FLT_MAXLEN, b, sizeof(b));
        G.set.maxLen = std::max(0.0, std::min(3600.0, atof(b)));
        G.set.playableOnly = IsDlgButtonChecked(h, IDC_FLT_PLAYABLE) == BST_CHECKED;
        std::string ts;
        for (size_t i = 0; i < g_ftypes.size(); i++)
            if (SendMessageA(g_ftypeCb[i], BM_GETCHECK, 0, 0) == BST_CHECKED) {
                if (!ts.empty())
                    ts += ";";
                ts += g_ftypes[i];
            }
        G.set.types = ts;
        G.set.save();
        rebuild_view();
        EndDialog(h, 1);
        return TRUE;
    }
    if (msg == WM_COMMAND && wp == IDCANCEL) {
        EndDialog(h, 0);
        return TRUE;
    }
    return FALSE;
}

static INT_PTR CALLBACK AboutProc(HWND h, UINT msg, WPARAM wp, LPARAM)
{
    if (msg == WM_COMMAND && (wp == IDOK || wp == IDCANCEL)) {
        EndDialog(h, 0);
        return TRUE;
    }
    return FALSE;
}

/* ---------------- tracker view sync ---------------- */
/* tell the engine which window to push; while enabled the engine posts
 * WM_APP_ROWS itself on every playback row change (row-accurate, no
 * polling on the UI side) */
static void trk_push(bool on)
{
    if (!G.hPat)
        return;
    if (G.panelHidden)
        on = false; /* nothing to show: the engine stops formatting rows */
    RECT cr;
    GetClientRect(G.hPat, &cr);
    int nch = (cr.right - 8) / (4 * G.trkCharW);
    if (nch < 1)
        nch = 1;
    if (nch > 64)
        nch = 64;
    /* belt and braces: row height is measured at startup, but never let a
     * zero reach the division below (tiny-window + failed metrics) */
    int rowH = G.trkRowH > 0 ? G.trkRowH : 12;
    /* 2*half+1 rows must fit below the channel header line (one row tall,
     * see PatProc), or the top row is drawn over the header */
    int half = ((cr.bottom - rowH) / rowH - 1) / 2;
    if (half < 2)
        half = 2;
    if (half > 40)
        half = 40;
    if (G.trkChOff > G.trkTotal - nch)
        G.trkChOff = G.trkTotal - nch;
    if (G.trkChOff < 0)
        G.trkChOff = 0;
    G.engine.fetchRows(on, half, nch, G.trkChOff);
}

/* background behavior: while the app is not in focus, only audio keeps
 * running at full pace. Visible-but-unfocused, the info tab and tracker
 * view update at most every 500 ms and the analyzer idles. Minimized, the
 * tracker fetch is switched off entirely (the engine stops building row
 * messages) and no text updates happen; the engine's audible-position
 * bookkeeping and playback continue untouched in every mode. */
static void apply_power_mode(int mode)
{
    if (mode == G.powerMode)
        return;
    G.powerMode = mode;
    if (mode == 2) {
        if (G.set.tracker)
            trk_push(false); /* no row formatting/posting while invisible */
    }
    apply_rates();
    EnterCriticalSection(&G.acs);
    G.anaPause = mode != 0;
    LeaveCriticalSection(&G.acs);
    SetEvent(G.aEv); /* let the analyzer notice quickly */
}

/* right panel pages: 0 = info+log, 1 = full-size tracker view */
static void trk_tab_set(bool pattern)
{
    G.set.tracker = pattern;
    if (G.hTab)
        SendMessageA(G.hTab, TCM_SETCURSEL, pattern ? 1 : 0, 0);
    /* a hidden panel (narrow window, see layout) keeps every page hidden;
     * the selected page still switches and comes back with the panel */
    int show = (pattern || G.panelHidden) ? SW_HIDE : SW_SHOW;
    ShowWindow(G.hTab, G.panelHidden ? SW_HIDE : SW_SHOW);
    ShowWindow(G.hTitle, show);
    ShowWindow(G.hFmt, show);
    ShowWindow(G.hTrk, show);
    ShowWindow(G.hLen, show);
    ShowWindow(G.hPos, show);
    ShowWindow(G.hSeq, show);
    ShowWindow(G.hOut, show);
    ShowWindow(G.hSub, show);
    ShowWindow(G.hOf, show);
    ShowWindow(G.hPlayAll, show);
    ShowWindow(G.hLog, show);
    ShowWindow(G.hPat, (pattern && !G.panelHidden) ? SW_SHOW : SW_HIDE);
    CheckMenuItem(GetMenu(G.hwnd), IDM_VIEW_TRACKER,
                  pattern ? MF_CHECKED : MF_UNCHECKED);
    trk_push(pattern);
}

/* ---------------- commands ---------------- */
static void on_command(int id)
{
    switch (id) {
    case IDM_FILE_OPEN:
        do_open_files();
        break;
    case IDM_FILE_OPENFOLDER:
    case IDC_BTN_OPENFOLDER:
        do_open_folder();
        break;
    case IDM_FILE_RESCAN:
    case IDC_BTN_RESCAN:
        if (!G.lastDir.empty())
            scan_into(G.lastDir);
        else
            logline("Rescan: open a folder first (File > Open folder)");
        break;
    case IDM_FILE_IMPORTM3U: {
        char fn[MAX_PATH] = "";
        OPENFILENAMEA o;
        memset(&o, 0, sizeof(o));
        o.lStructSize = OPENFILENAME_SIZE_VERSION_400;
        o.hwndOwner = G.hwnd;
        o.lpstrFilter = "M3U\0*.m3u\0All\0*.*\0";
        o.lpstrFile = fn;
        o.nMaxFile = MAX_PATH;
        o.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
        if (GetOpenFileNameA(&o)) {
            std::vector<Track> t;
            if (m3u_import(fn, t)) {
                int dst = G.srcSel;
                if (dst < 1) {
                    /* import into a fresh playlist when viewing the library */
                    Playlist pl;
                    pl.name = unique_pl_name("Imported M3U", -1);
                    EnterCriticalSection(&G.acs);
                    G.pls.push_back(pl);
                    LeaveCriticalSection(&G.acs);
                    dst = 2 + (int)G.pls.size() - 1;
                    rebuild_sources();
                }
                EnterCriticalSection(&G.acs);
                std::vector<Track> &d = G.srcBy(dst);
                unsigned added = 0;
                for (size_t i = 0; i < t.size(); i++)
                    if (!vec_has_path(d, t[i].path)) {
                        d.push_back(t[i]);
                        added++;
                    }
                LeaveCriticalSection(&G.acs);
                if (G.srcSel == dst)
                    rebuild_view();
                if (dst == 1)
                    rebuild_sources(); /* "Favorites (N)" */
                queue_analysis(dst);
                refresh_count();
                logline("Imported %u track(s), %u new", (unsigned)t.size(), added);
                if (added)
                    save_all_lists();
            }
        }
        break;
    }
    case IDM_FILE_EXPORTM3U: {
        char fn[MAX_PATH] = "";
        OPENFILENAMEA o;
        memset(&o, 0, sizeof(o));
        o.lStructSize = OPENFILENAME_SIZE_VERSION_400;
        o.hwndOwner = G.hwnd;
        o.lpstrFilter = "M3U\0*.m3u\0";
        o.lpstrFile = fn;
        o.nMaxFile = MAX_PATH;
        o.Flags = OFN_OVERWRITEPROMPT;
        o.lpstrDefExt = "m3u";
        if (GetSaveFileNameA(&o)) {
            /* copy under the lock: the analyzer thread updates track fields
             * concurrently */
            std::vector<Track> cp;
            EnterCriticalSection(&G.acs);
            cp = G.src();
            LeaveCriticalSection(&G.acs);
            if (m3u_export(fn, cp))
                logline("Exported to %s", fn);
        }
        break;
    }
    case IDM_FILE_EXIT:
        DestroyWindow(G.hwnd);
        break;
    case IDM_PB_PLAYPAUSE:
    case IDC_BTN_PLAYPAUSE:
        cmd_playpause();
        break;
    case IDM_PB_STOP:
    case IDC_BTN_STOP:
        G.engine.stop();
        SendMessageA(G.hStatus, SB_SETTEXTA, 0, (LPARAM)"Stopped");
        clear_playing();
        break;
    case IDM_PB_PREV:
    case IDC_BTN_PREV:
        cmd_prev();
        break;
    case IDM_PB_NEXT:
    case IDC_BTN_NEXT:
        advance(1);
        break;
    case IDM_PB_LOOP:
        setLoop(!G.set.loop);
        break;
    case IDC_CHK_LOOP:
        setLoop(SendMessageA(G.hLoop, BM_GETCHECK, 0, 0) == BST_CHECKED);
        break;
    case IDM_PB_REPEAT:
        setRepeat(!G.set.repeat);
        break;
    case IDC_CHK_PLAYALL: /* BS_AUTOCHECKBOX: the box already toggled itself */
        setPlayAll(SendMessageA(G.hPlayAll, BM_GETCHECK, 0, 0) == BST_CHECKED);
        G.set.save();
        break;
    case IDC_CHK_REPEAT:
        setRepeat(SendMessageA(G.hRepeat, BM_GETCHECK, 0, 0) == BST_CHECKED);
        break;
    case IDM_VIEW_TRACKER:
        trk_tab_set(!G.set.tracker);
        G.set.save();
        break;
    case IDM_TRK_CHL:
    case IDM_TRK_CHR: {
        if (!G.set.tracker || G.trkTotal <= 0)
            break;
        RECT cr;
        GetClientRect(G.hPat, &cr);
        int nch = (cr.right - 8) / (4 * G.trkCharW);
        if (nch < 1)
            nch = 1;
        if (nch > 64)
            nch = 64;
        G.trkChOff += (id == IDM_TRK_CHL) ? -8 : 8;
        if (G.trkChOff < 0)
            G.trkChOff = 0;
        if (G.trkChOff > G.trkTotal - nch)
            G.trkChOff = G.trkTotal - nch;
        trk_push(true);
        break;
    }
    case IDM_PB_MUTE:
    case IDC_BTN_MUTE:
        G.set.mute = !G.set.mute;
        SetWindowTextA(G.hMute, G.set.mute ? "Unmute" : "Mute");
        /* one ModifyMenu with the check state: a separate CheckMenuItem
         * before it would be reset by the MF_STRING rewrite */
        ModifyMenuA(GetMenu(G.hwnd), IDM_PB_MUTE,
                    MF_BYCOMMAND | MF_STRING | (G.set.mute ? MF_CHECKED : MF_UNCHECKED),
                    IDM_PB_MUTE, G.set.mute ? "Un&mute" : "&Mute");
        G.engine.setVolume(G.set.volume, G.set.mute);
        break;
    case IDM_PL_NEW: {
        char def[64];
        sprintf(def, "Playlist %u", (unsigned)(G.pls.size() + 1));
        if (!input_box(unique_pl_name(def, -1).c_str()))
            break;
        Playlist pl;
        pl.name = unique_pl_name(g_input, -1);
        EnterCriticalSection(&G.acs); /* analyzer binds srcBy() refs under it */
        G.pls.push_back(pl);
        LeaveCriticalSection(&G.acs);
        rebuild_sources();
        logline("Created playlist \"%s\"", pl.name.c_str());
        save_all_lists();
        break;
    }
    case IDM_PL_RENAME: {
        int p = G.srcSel - 2;
        if (p < 0 || p >= (int)G.pls.size()) {
            logline("Rename: switch to a playlist first");
            break;
        }
        if (!input_box(G.pls[p].name.c_str()))
            break;
        std::string newName = unique_pl_name(g_input, p);
        if (newName == G.pls[p].name)
            break; /* unchanged: moving the plan onto itself would erase it */
        {
            /* carry the drawn shuffle order over to the new name */
            std::string oldKey = "playlist:" + G.pls[p].name;
            std::map<std::string, std::vector<std::string>>::iterator it =
                G.plans.find(oldKey);
            if (it != G.plans.end()) {
                std::vector<std::string> plan;
                plan.swap(it->second);
                G.plans.erase(it);
                G.plans["playlist:" + newName].swap(plan);
                save_plan_file();
            }
        }
        EnterCriticalSection(&G.acs);
        G.pls[p].name = newName;
        LeaveCriticalSection(&G.acs);
        rebuild_sources();
        logline("Playlist renamed to \"%s\"", newName.c_str());
        save_all_lists();
        break;
    }
    case IDM_PL_DELETE: {
        int p = G.srcSel - 2;
        if (p < 0 || p >= (int)G.pls.size()) {
            logline("Delete: switch to a playlist first");
            break;
        }
        char q[300];
        sprintf(q, "Delete playlist \"%s\" (%u tracks)?", G.pls[p].name.c_str(),
                (unsigned)G.pls[p].tr.size());
        if (MessageBoxA(G.hwnd, q, "modjuke95", MB_YESNO | MB_ICONQUESTION) != IDYES)
            break;
        /* stop when THAT playlist is the playing source - not only when it
         * happens to be the one on screen */
        if (G.playSrc == 2 + p) {
            G.engine.stop();
            clear_playing();
            SendMessageA(G.hStatus, SB_SETTEXTA, 0, (LPARAM)"Stopped");
        }
        G.plans.erase("playlist:" + G.pls[p].name);
        save_plan_file();
        EnterCriticalSection(&G.acs); /* analyzer binds srcBy() refs under it */
        G.pls.erase(G.pls.begin() + p);
        /* later playlists move down one source index: the playing one and
         * queued analysis jobs must follow, or they'd name the next list */
        if (G.playSrc > 2 + p)
            G.playSrc--;
        for (std::deque<std::pair<int, int>>::iterator it = G.aq.begin(); it != G.aq.end();) {
            if (it->first == 2 + p) {
                it = G.aq.erase(it);
                continue;
            }
            if (it->first > 2 + p)
                it->first--;
            ++it;
        }
        LeaveCriticalSection(&G.acs);
        G.srcSel = 0;
        rebuild_sources();
        rebuild_view();
        refresh_count();
        logline("Playlist deleted");
        save_all_lists();
        break;
    }
    case IDM_PL_FAV:
        fav_add_selected();
        break;
    case IDM_PL_UNFAV:
        if (G.srcSel == 1)
            remove_selected_from_current();
        else
            logline("Switch to Favorites to remove from it");
        break;
    case IDM_PL_IGNORE:
        ignore_selected();
        break;
    case IDM_PL_MANAGEIGN:
        DialogBoxParamA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDD_IGNORED), G.hwnd,
                        IgnoredProc, 0);
        break;
    case IDM_PL_SHUFFLENOW:
        cmd_shufflenow();
        break;
    case IDM_SEEK_BACK:
    case IDM_SEEK_FWD:
    case IDM_SEEK_BACK30:
    case IDM_SEEK_FWD30: {
        double step = (id == IDM_SEEK_BACK || id == IDM_SEEK_BACK30) ? -1.0 : 1.0;
        step *= (id == IDM_SEEK_BACK30 || id == IDM_SEEK_FWD30) ? 30.0 : 5.0;
        EngineSnap es = G.engine.snap();
        if (es.loaded) {
            /* relative to what is being heard, not the render position */
            double base = es.heard > 0 ? es.heard : es.pos;
            double p = base + step;
            if (p < 0)
                p = 0;
            G.engine.seekTo(p);
        }
        break;
    }
    case IDM_VOL_UP:
    case IDM_VOL_DOWN:
    case IDM_VOL_ZERO: {
        int v = (id == IDM_VOL_ZERO) ? 0
                                     : (int)SendMessageA(G.hVol, TBM_GETPOS, 0, 0) +
                                           (id == IDM_VOL_UP ? 5 : -5);
        if (v < 0)
            v = 0;
        if (v > 100)
            v = 100;
        SendMessageA(G.hVol, TBM_SETPOS, TRUE, v);
        update_vol_pct();
        G.set.volume = v;
        G.engine.setVolume(v, G.set.mute);
        break;
    }
    case IDM_TOGGLE_LOOP:
        setLoop(!G.set.loop);
        break;
    case IDM_TOGGLE_REPEAT:
        setRepeat(!G.set.repeat);
        break;
    case IDM_FOCUS_SEARCH:
        SetFocus(G.hSearch);
        break;
    case IDM_REVEAL_PLAYING:
        scroll_to_playing();
        break;
    case IDM_CLEAR_SEARCH:
        if (GetWindowTextLengthA(G.hSearch) > 0) {
            SetWindowTextA(G.hSearch, "");
            KillTimer(G.hwnd, 2); /* the EN_CHANGE above armed a second rebuild */
            rebuild_view();
        }
        break;
    case IDM_PLAY_SELECTED: {
        int sel = ListView_GetNextItem(G.hList, -1, LVNI_SELECTED);
        if (sel >= 0)
            play_view_row(sel);
        break;
    }
    case IDM_HELP_SETTINGS:
        DialogBoxParamA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDD_SETTINGS), G.hwnd,
                        SettingsProc, 0);
        break;
    case IDM_HELP_ABOUT:
        DialogBoxParamA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDD_ABOUT), G.hwnd,
                        AboutProc, 0);
        break;
    case IDC_BTN_SHUFFLENOW:
        cmd_shufflenow();
        break;
    case IDC_BTN_FILTER:
        DialogBoxParamA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDD_FILTER), G.hwnd,
                        FilterProc, 0);
        break;
    case IDC_BTN_ADDPL: {
        /* pick a destination (favorites or a playlist) via a popup */
        HMENU m = build_addto_menu();
        RECT br;
        GetWindowRect(G.hAddPl, &br);
        int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN, br.left, br.bottom, 0,
                                G.hwnd, NULL);
        DestroyMenu(m);
        if (id)
            do_addto(id);
        break;
    }
    case IDC_BTN_CLEARPL: {
        if (G.srcSel < 1) {
            logline("Select a favorites/playlist source to clear it");
            break;
        }
        /* marker first: clear_playing() re-titles the rows still on screen,
         * which must not index the emptied vector */
        if (G.playSrc == G.srcSel) {
            G.engine.stop();
            clear_playing();
            SendMessageA(G.hStatus, SB_SETTEXTA, 0, (LPARAM)"Stopped");
        }
        EnterCriticalSection(&G.acs);
        G.src().clear();
        LeaveCriticalSection(&G.acs);
        rebuild_sources(); /* "Favorites (N)" */
        rebuild_view();
        refresh_count();
        logline("List cleared");
        save_all_lists();
        break;
    }
    }
}

/* ---------------- window proc ---------------- */
/* refresh_ui runs 2-20 times a second; on Win95 every WM_SETTEXT repaints
 * the control even when the text is the same, so compare first (a
 * WM_GETTEXT only copies a string). Comparing with the control itself
 * rather than a cache stays right when other code sets the same control. */
static void set_text_if_changed(HWND h, const char *txt)
{
    char cur[160];
    int n = GetWindowTextA(h, cur, sizeof(cur));
    if (n >= (int)sizeof(cur) - 1 || strcmp(cur, txt) != 0)
        SetWindowTextA(h, txt);
}

static void set_status_if_changed(const char *txt)
{
    int n = LOWORD(SendMessageA(G.hStatus, SB_GETTEXTLENGTHA, 0, 0));
    if (n == (int)strlen(txt)) {
        std::string cur((size_t)n + 1, '\0');
        SendMessageA(G.hStatus, SB_GETTEXTA, 0, (LPARAM)&cur[0]);
        if (strcmp(cur.c_str(), txt) == 0)
            return;
    }
    SendMessageA(G.hStatus, SB_SETTEXTA, 0, (LPARAM)txt);
}

static void refresh_ui()
{
    EngineSnap s = G.engine.snap();
    /* time + seekbar follow the heard position (consistent with the tracker
     * view); s.pos is the render position, a buffer queue ahead */
    double shown = s.heard > 0 ? s.heard : s.pos;
    /* don't fight a user drag, nor the moment right after a seek */
    bool seekHeld = g_tbDrag == G.hSeek || (DWORD)(GetTickCount() - g_seekDrag) <= 700;
    if (!seekHeld)
        set_text_if_changed(G.hTime, m95_fmt_time(shown).c_str());
    if (s.loaded && s.dur > 0 && s.dur < 1e6) {
        /* the loaded subsong's own length (the analysis knows subsong 1) */
        if ((LONG)(s.dur * 10) != (LONG)SendMessageA(G.hSeek, TBM_GETRANGEMAX, 0, 0)) {
            set_seek_range(s.dur);
            char lb[48];
            snprintf(lb, sizeof(lb), "Length: %s", m95_fmt_time(s.dur).c_str());
            SetWindowTextA(G.hLen, lb);
        }
        /* every time, not only on a range change: a refresh between the
         * load request and the engine's load writes "--:--" below, and the
         * range (already set from the analysis) may not change after it */
        set_text_if_changed(G.hDur, m95_fmt_time(s.dur).c_str());
    } else if (!s.loaded && !seekHeld) {
        /* stopped: thumb home, no length */
        if (SendMessageA(G.hSeek, TBM_GETPOS, 0, 0) != 0)
            SendMessageA(G.hSeek, TBM_SETPOS, TRUE, 0);
        set_text_if_changed(G.hDur, "--:--");
    }
    if (s.dur > 0 && s.dur < 1e6 && !seekHeld) {
        int p = (int)(shown * 10);
        int rmax = (int)(s.dur * 10);
        if (p > rmax)
            p = rmax;
        if (p != (int)SendMessageA(G.hSeek, TBM_GETPOS, 0, 0))
            SendMessageA(G.hSeek, TBM_SETPOS, TRUE, p);
    }
    char b[128];
    /* prefer the heard position (what the tracker view highlights); the raw
     * render position runs ahead of the speakers by the buffer queue */
    sprintf(b, "Position: order %d, pattern %d, row %d",
            s.trkValid ? s.trkOrder : s.order,
            s.trkValid ? s.trkPat : s.pat,
            s.trkValid ? s.trkRow : s.row);
    set_text_if_changed(G.hPos, b);
    sprintf(b, "Sequencer: speed %d, tempo %d", s.speed, s.bpm);
    set_text_if_changed(G.hSeq, b);
    /* the buffer in use (a changed setting applies at the next load) */
    sprintf(b, "Output: waveOut %d Hz, buffer %d ms", s.rate, s.bufMs);
    set_text_if_changed(G.hOut, b);
    Track *t = current_track();
    if (s.playing) {
        /* no track (cleared/deleted list): leave the status to whoever
         * cleared it instead of a bare "Playing: " */
        if (t) {
            std::string st = (s.paused ? "Paused: " : "Playing: ");
            st += t->title;
            set_status_if_changed(st.c_str());
        }
        set_text_if_changed(G.hPlay, s.paused ? "Play" : "Pause");
    } else {
        /* not playing: a natural end leaves loaded=true, so this branch must
         * fire unconditionally or the button stays stuck on "Pause" */
        set_text_if_changed(G.hPlay, "Play");
        if (!s.loaded && !G.trkRows.empty()) {
            /* stopped: blank the pattern view */
            G.trkRows.clear();
            G.trkTotal = 0; /* no "ch a-b of N" header over the blank view */
            if (G.hPat)
                InvalidateRect(G.hPat, NULL, FALSE);
        }
    }
}

/* ---------------- tracker (pattern) view ----------------
 * Owner-drawn child: monospace bitmap font, one TextOut per row, current
 * row inverted and centered. Everything is erased+drawn inside WM_PAINT so
 * there is no flicker, and a full repaint is only ~20 TextOut calls. */

/* apply one row-window message (posted or synchronously grabbed) and
 * repaint immediately - RDW_UPDATENOW forces the WM_PAINT now instead of
 * queueing it, where Win95 starves it during list scrolling */
static void trk_apply_rows(RowsMsg *m)
{
    G.trkHalf = m->half;
    G.trkOff = m->off;
    G.trkNchView = m->nch;
    G.trkTotal = m->total;
    G.trkShownOrder = m->order;
    G.trkShownRow = m->row;
    G.trkRows.swap(m->rows);
    if (G.hPat)
        RedrawWindow(G.hPat, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
}

/* keep the pattern view alive while the list is being scrolled (see
 * lv_subproc): bypass the message queue entirely */
static void trk_sync_now()
{
    if (!G.hPat || !G.set.tracker || G.panelHidden)
        return;
    RowsMsg *m = G.engine.grabLatestRows();
    if (!m)
        return;
    if (m->order != G.trkShownOrder || m->row != G.trkShownRow ||
        G.trkRows.empty())
        trk_apply_rows(m);
    delete m;
}

static LRESULT CALLBACK PatProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_ERASEBKGND:
        return 1; /* painted in WM_PAINT */
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT cr;
        GetClientRect(h, &cr);
        /* under GDI exhaustion the brushes can be NULL; FillRect then takes
         * a system color instead of drawing with an invalid handle */
        FillRect(dc, &cr, G.trkBg ? G.trkBg : (HBRUSH)(COLOR_WINDOW + 1));
        HFONT of = (HFONT)SelectObject(dc, G.hTrkFont);
        SetBkMode(dc, TRANSPARENT);
        int topY = 0;
        if (G.trkTotal > G.trkNchView && G.trkNchView > 0) {
            char hb[48];
            sprintf(hb, "ch %d-%d of %d  (Ctrl+Shift+arrows to page)",
                    G.trkOff + 1, G.trkOff + G.trkNchView, G.trkTotal);
            SetTextColor(dc, RGB(128, 128, 128));
            TextOutA(dc, 2, 1, hb, (int)strlen(hb));
            topY = G.trkRowH > 12 ? G.trkRowH : 12;
        }
        int midY = topY + (cr.bottom - topY) / 2;
        for (size_t i = 0; i < G.trkRows.size(); i++) {
            int y = midY + ((int)i - G.trkHalf) * G.trkRowH - G.trkRowH / 2;
            if (y < topY || y > cr.bottom) /* never over the header */
                continue;
            if ((int)i == G.trkHalf) {
                RECT rr = { 0, y, cr.right, y + G.trkRowH };
                FillRect(dc, &rr, G.trkHi ? G.trkHi : (HBRUSH)(COLOR_HIGHLIGHT + 1));
                SetTextColor(dc, RGB(255, 255, 255));
            } else {
                SetTextColor(dc, RGB(192, 192, 192));
            }
            TextOutA(dc, 2, y, G.trkRows[i].c_str(), (int)G.trkRows[i].size());
        }
        SelectObject(dc, of);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcA(h, m, w, l);
}

/* remember the current song + heard position for the next start */
static void save_session()
{
    std::string ini = m95_exe_dir() + "modjuke95.ini";
    Track *t = current_track();
    EngineSnap es = G.engine.snap();
    if (t && es.loaded) {
        /* prefer the heard position; es.pos is the render position, which
         * sits a buffer queue ahead of the speakers */
        double pos = es.heard > 0 ? es.heard : es.pos;
        if (G.set.loop && es.dur > 0)
            pos = fmod(pos, es.dur);
        char pb[32];
        WritePrivateProfileStringA("session", "lastpath", t->path.c_str(), ini.c_str());
        sprintf(pb, "%.3f", pos);
        WritePrivateProfileStringA("session", "lastpos", pb, ini.c_str());
        sprintf(pb, "%d", G.curSub);
        WritePrivateProfileStringA("session", "lastsub", pb, ini.c_str());
    } else {
        WritePrivateProfileStringA("session", "lastpath", NULL, ini.c_str());
    }
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        G.hwnd = h;
        G.hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        G.hOpenFld = mkctrl("BUTTON", "Open folder...", BS_PUSHBUTTON, 0, 0, 0, 0,
                            IDC_BTN_OPENFOLDER);
        G.hRescan = mkctrl("BUTTON", "Rescan", BS_PUSHBUTTON, 0, 0, 0, 0, IDC_BTN_RESCAN);
        G.hSearch = mkctrl("EDIT", "", ES_AUTOHSCROLL | WS_BORDER, 0, 0, 0, 0,
                           IDC_EDIT_SEARCH);
        G.hSrc = mkctrl("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, IDC_COMBO_SOURCE);
        G.hOrder = mkctrl("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, IDC_COMBO_ORDER);
        G.hShuf = mkctrl("BUTTON", "Shuffle now", BS_PUSHBUTTON, 0, 0, 0, 0,
                         IDC_BTN_SHUFFLENOW);
        G.hFilter = mkctrl("BUTTON", "Filter", BS_PUSHBUTTON, 0, 0, 0, 0, IDC_BTN_FILTER);
        G.hAddPl = mkctrl("BUTTON", "Add to list...", BS_PUSHBUTTON, 0, 0, 0, 0,
                          IDC_BTN_ADDPL);
        G.hClearPl = mkctrl("BUTTON", "Clear list", BS_PUSHBUTTON, 0, 0, 0, 0,
                            IDC_BTN_CLEARPL);
        G.hList = mkctrl(WC_LISTVIEWA, "",
                         LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER, 0, 0, 0, 0, IDC_LIST);
    g_lvProc = (WNDPROC)GetWindowLongA(G.hList, GWL_WNDPROC);
    SetWindowLongA(G.hList, GWL_WNDPROC, (LONG)(WNDPROC)lv_subproc);
        ListView_SetExtendedListViewStyle(G.hList, LVS_EX_FULLROWSELECT);
        G.hTitle = mkctrl("STATIC", "", SS_LEFT, 0, 0, 0, 0, IDC_STATIC_TITLE);
        G.hFmt = mkctrl("STATIC", "Format:", SS_LEFT, 0, 0, 0, 0, IDC_STATIC_FORMAT);
        G.hTrk = mkctrl("STATIC", "Tracker:", SS_LEFT, 0, 0, 0, 0, IDC_STATIC_TRACKER);
        G.hLen = mkctrl("STATIC", "Length:", SS_LEFT, 0, 0, 0, 0, IDC_STATIC_LENGTH);
        G.hPos = mkctrl("STATIC", "Position:", SS_LEFT, 0, 0, 0, 0, IDC_STATIC_POSITION);
        G.hSeq = mkctrl("STATIC", "Sequencer:", SS_LEFT, 0, 0, 0, 0, IDC_STATIC_SEQUENCER);
        G.hOut = mkctrl("STATIC", "Output:", SS_LEFT, 0, 0, 0, 0, IDC_STATIC_OUTPUT);
        G.hSub = mkctrl("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, IDC_COMBO_SUBSONG);
        G.hOf = mkctrl("STATIC", "of 1", SS_LEFT, 0, 0, 0, 0, IDC_STATIC_OF);
        G.hPlayAll = mkctrl("BUTTON", "Play all subsongs", BS_AUTOCHECKBOX, 0, 0, 0, 0,
                            IDC_CHK_PLAYALL);
        G.hLog = mkctrl("EDIT", "",
                        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_BORDER |
                            WS_VSCROLL | ES_LEFT | WS_TABSTOP,
                        0, 0, 0, 0, IDC_EDIT_LOG);
        /* right-panel pages: info+log / full-size tracker view */
        G.hTab = mkctrl(WC_TABCONTROLA, "", WS_TABSTOP, 0, 0, 0, 0, IDC_TAB);
        {
            TCITEMA it;
            memset(&it, 0, sizeof(it));
            it.mask = TCIF_TEXT;
            it.pszText = (LPSTR) "Info";
            SendMessageA(G.hTab, TCM_INSERTITEMA, 0, (LPARAM)&it);
            it.pszText = (LPSTR) "Pattern";
            SendMessageA(G.hTab, TCM_INSERTITEMA, 1, (LPARAM)&it);
        }
        /* tracker view: own window class, painted entirely by us */
        G.hPat = CreateWindowExA(0, "m95patcls", "", WS_CHILD | WS_BORDER, 0, 0, 0, 0,
                                 h, (HMENU)IDC_TRACKER, GetModuleHandleA(NULL), NULL);
        G.hTrkFont = CreateFontA(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Terminal");
        if (!G.hTrkFont)
            G.hTrkFont = (HFONT)GetStockObject(SYSTEM_FIXED_FONT);
        {
            HDC dc = GetDC(h);
            if (dc) {
                HFONT o = (HFONT)SelectObject(dc, G.hTrkFont);
                TEXTMETRICA tm;
                GetTextMetricsA(dc, &tm);
                G.trkRowH = tm.tmHeight + 2;
                SIZE sz;
                GetTextExtentPoint32A(dc, "M", 1, &sz);
                G.trkCharW = sz.cx > 0 ? sz.cx : 6;
                SelectObject(dc, o);
                ReleaseDC(h, dc);
            } else { /* GDI exhausted: sane fixed-pitch defaults */
                G.trkRowH = 14;
                G.trkCharW = 6;
            }
        }
        G.trkBg = CreateSolidBrush(RGB(0, 0, 0));
        G.trkHi = CreateSolidBrush(RGB(0, 0, 128));
        G.hPrev = mkctrl("BUTTON", "|<<", BS_PUSHBUTTON, 0, 0, 0, 0, IDC_BTN_PREV);
        G.hPlay = mkctrl("BUTTON", "Play", BS_PUSHBUTTON, 0, 0, 0, 0, IDC_BTN_PLAYPAUSE);
        G.hNext = mkctrl("BUTTON", ">>|", BS_PUSHBUTTON, 0, 0, 0, 0, IDC_BTN_NEXT);
        G.hStop = mkctrl("BUTTON", "Stop", BS_PUSHBUTTON, 0, 0, 0, 0, IDC_BTN_STOP);
        G.hLoop = mkctrl("BUTTON", "Loop", BS_AUTOCHECKBOX, 0, 0, 0, 0, IDC_CHK_LOOP);
        G.hRepeat = mkctrl("BUTTON", "Repeat queue", BS_AUTOCHECKBOX, 0, 0, 0, 0,
                           IDC_CHK_REPEAT);
        G.hMute = mkctrl("BUTTON", "Mute", BS_PUSHBUTTON, 0, 0, 0, 0, IDC_BTN_MUTE);
        G.hVol = mkctrl(TRACKBAR_CLASSA, "", TBS_HORZ | TBS_NOTICKS, 0, 0, 0, 0,
                        IDC_TRK_VOLUME);
        G.hVolPct = mkctrl("STATIC", "100%", SS_LEFT, 0, 0, 0, 0, IDC_STATIC_VOLPCT);
        G.hTime = mkctrl("STATIC", "0:00", SS_LEFT, 0, 0, 0, 0, IDC_STATIC_TIME);
        G.hSeek = mkctrl(TRACKBAR_CLASSA, "", TBS_HORZ | TBS_NOTICKS, 0, 0, 0, 0,
                         IDC_TRK_SEEK);
        G.hDur = mkctrl("STATIC", "--:--", SS_RIGHT, 0, 0, 0, 0, IDC_STATIC_DUR);
        g_tbProc = (WNDPROC)GetWindowLongA(G.hSeek, GWL_WNDPROC);
        SetWindowLongA(G.hSeek, GWL_WNDPROC, (LONG)(WNDPROC)tb_subproc);
        SetWindowLongA(G.hVol, GWL_WNDPROC, (LONG)(WNDPROC)tb_subproc);
        G.hStatus = mkctrl(STATUSCLASSNAMEA, "", 0, 0, 0, 0, 0, IDC_STATUSBAR);
        int parts[2] = { 500, -1 };
        SendMessageA(G.hStatus, SB_SETPARTS, 2, (LPARAM)parts);

        {
            LVCOLUMNA c;
            memset(&c, 0, sizeof(c));
            c.mask = LVCF_TEXT | LVCF_WIDTH;
            const char *names[] = { "Module", "Folder", "Len", "Fmt", "Ch", "Ss" };
            const int widths[] = { 300, 90, 48, 40, 30, 28 };
            for (int i = 0; i < 6; i++) {
                c.pszText = (LPSTR)names[i];
                c.cx = widths[i];
                ListView_InsertColumn(G.hList, i, &c);
            }
        }
        SendMessageA(G.hOrder, CB_ADDSTRING, 0, (LPARAM)"Alphabetical");
        SendMessageA(G.hOrder, CB_ADDSTRING, 0, (LPARAM)"Path");
        SendMessageA(G.hOrder, CB_ADDSTRING, 0, (LPARAM)"Shuffle");
        SendMessageA(G.hOrder, CB_SETCURSEL, G.set.order, 0);
        SendMessageA(G.hVol, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageA(G.hVol, TBM_SETPOS, TRUE, G.set.volume);
        update_vol_pct();
        SendMessageA(G.hSeek, TBM_SETRANGE, TRUE, MAKELPARAM(0, 0)); /* inert until a song loads */
        setLoop(G.set.loop);
        setRepeat(G.set.repeat);
        setPlayAll(G.set.playAll);
        if (G.set.mute) {
            SetWindowTextA(G.hMute, "Unmute");
            ModifyMenuA(GetMenu(G.hwnd), IDM_PB_MUTE, MF_BYCOMMAND | MF_STRING | MF_CHECKED,
                        IDM_PB_MUTE, "Un&mute");
        }
        DragAcceptFiles(h, TRUE);
        SetTimer(h, 1, G.set.uiRefreshMs, NULL);

        InitializeCriticalSection(&G.acs);
        G.aEv = CreateEventA(NULL, FALSE, FALSE, NULL);
        {
            DWORD tid;
            G.anaThr = CreateThread(NULL, 0x100000, analyzer_main, NULL, 0, &tid);
        }
        G.engine.init(h);
        G.engine.setTrkDelay(G.set.trkDelayMs);
        apply_rates(); /* timer + engine refresh, honoring background mode */
        G.engine.setVolume(G.set.volume, G.set.mute);
        logline("modjuke95 v1 (libopenmpt %s)", mpt_version_string().c_str());
        {
            std::string dir = m95_exe_dir();
            std::string last = dir + "last.m3u";
            std::vector<Track> t;
            if (m3u_import(last, t) && !t.empty()) {
                G.lib = t;
                logline("Restored %u library tracks", (unsigned)t.size());
            }
            /* favorites */
            m3u_import(dir + "favorites.m3u", G.fav);
            /* playlists (names in the ini, tracks in playlist-N.m3u) */
            std::string ini = dir + "modjuke95.ini";
            int npl = GetPrivateProfileIntA("playlists", "count", 0, ini.c_str());
            if (npl < 0 || npl > 1000) /* garbled ini: don't loop for ages */
                npl = 0;
            for (int i = 0; i < npl; i++) {
                char key[16], nm[256], fn[64];
                sprintf(key, "name%d", i + 1);
                sprintf(nm, "Playlist %d", i + 1);
                GetPrivateProfileStringA("playlists", key, nm, nm, sizeof(nm), ini.c_str());
                sprintf(fn, "playlist-%d.m3u", i + 1);
                Playlist pl;
                pl.name = unique_pl_name(nm, -1);
                /* a missing file keeps the (empty) playlist: dropping it
                 * would lose its name at the next save */
                m3u_import(dir + fn, pl.tr);
                G.pls.push_back(pl);
            }
            /* ignored files */
            FILE *fi = fopen((dir + "ignored.txt").c_str(), "r");
            if (!fi) /* crash between remove and rename left only the .tmp */
                fi = fopen((dir + "ignored.txt.tmp").c_str(), "r");
            if (fi) {
                char ln[1024];
                while (fgets(ln, sizeof(ln), fi)) {
                    if (!strchr(ln, '\n') && !feof(fi)) {
                        int ch; /* over-long line: no real path, skip it */
                        while ((ch = fgetc(fi)) != EOF && ch != '\n') {
                        }
                        continue;
                    }
                    std::string p = m95_lower(m95_trim(ln));
                    if (!p.empty())
                        G.ign.push_back(p);
                }
                fclose(fi);
                std::sort(G.ign.begin(), G.ign.end());
            }
            /* strip ignored files from everything restored */
            for (size_t i = 0; i < G.lib.size();)
                if (path_ignored(G.lib[i].path))
                    G.lib.erase(G.lib.begin() + i);
                else
                    i++;
            load_plan_file();
            acache_load(); /* skip re-parsing unchanged modules */
            m95_stage("stage:lists-loaded");
            /* locate the song to resume BEFORE the first view build: when it
             * lives in favorites/a playlist the source switch happens now and
             * the expensive ListView fill runs once instead of twice */
            char lp[MAX_PATH];
            GetPrivateProfileStringA("session", "lastdir", "", lp, sizeof(lp), ini.c_str());
            G.lastDir = lp; /* Rescan works after a restart too */
            GetPrivateProfileStringA("session", "lastpath", "", lp, sizeof(lp), ini.c_str());
            double pos = 0;
            int sub = 1;
            int fsrc = -1, fidx = -1;
            if (lp[0] && !path_ignored(lp)) {
                DWORD at = GetFileAttributesA(lp);
                if (at != 0xFFFFFFFF && !(at & FILE_ATTRIBUTE_DIRECTORY)) {
                    char pb[32];
                    GetPrivateProfileStringA("session", "lastpos", "0", pb, sizeof(pb),
                                             ini.c_str());
                    pos = atof(pb);
                    if (!(pos >= 0))
                        pos = 0; /* garbled/NaN */
                    sub = GetPrivateProfileIntA("session", "lastsub", 1, ini.c_str());
                    if (sub < 1)
                        sub = 1;
                    /* find it in any source (library, favorites, playlists)
                     * so the marker shows up even if it wasn't in the library */
                    std::string want = m95_lower(lp);
                    EnterCriticalSection(&G.acs);
                    for (int s0 = 0; s0 < 2 + (int)G.pls.size() && fsrc < 0; s0++) {
                        std::vector<Track> &v = G.srcBy(s0);
                        for (size_t i = 0; i < v.size(); i++) {
                            if (m95_lower(v[i].path) == want) {
                                fsrc = s0;
                                fidx = (int)i;
                                break;
                            }
                        }
                    }
                    LeaveCriticalSection(&G.acs);
                }
            }
            if (fsrc >= 0)
                G.srcSel = fsrc;
            rebuild_sources();
            rebuild_view();
            m95_stage("stage:first-view");
            queue_analysis(0);
            if (!G.fav.empty())
                queue_analysis(1);
            for (size_t i = 0; i < G.pls.size(); i++)
                if (!G.pls[i].tr.empty())
                    queue_analysis(2 + (int)i);
            /* like the original: resume the last song, paused, at its last
             * position */
            if (fsrc >= 0) {
                G.playSrc = fsrc;
                G.playIdx = fidx;
                /* clamp a stale subsong against the known count
                 * (unanalyzed tracks report ss == 1; trust those) */
                {
                    Track *ft = current_track();
                    if (ft && ft->analyzed && ft->ss > 1 && sub > ft->ss)
                        sub = ft->ss;
                }
                /* load_current fills the info panel/caption; the engine
                 * opens the song already paused at the old position */
                load_current(sub, pos > 0.5 ? pos : 0, true);
                refresh_titles();
                logline("Resumed %s (paused)", m95_basename(lp).c_str());
            }
            m95_stage("stage:session-restored");
        }
        trk_tab_set(G.set.tracker);
        return 0;
    }
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED)
            return 0;
        /* batch the relayout: the class used to carry CS_HREDRAW|CS_VREDRAW,
         * which invalidated the whole client area on every WM_SIZE - during
         * a maximize/resize that pinned the CPU with a full redraw storm and
         * starved the (emulated) audio device enough to be audible. Now one
         * forced repaint per size event, and the tracker re-fetch is
         * coalesced on a timer so the engine thread isn't hammered mid-drag. */
        SendMessageA(h, WM_SETREDRAW, FALSE, 0);
        layout();
        SendMessageA(h, WM_SETREDRAW, TRUE, 0);
        /* RDW_ERASE is essential: without the erase flag the parent would
         * not repaint its gray background, leaving stale pixels of a child
         * (e.g. the tracker view) where it used to sit before the resize.
         * No RDW_UPDATENOW: during a resize burst the paint coalesces into
         * one pass at the next idle instead of a storm per size event. */
        RedrawWindow(h, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN);
        if (G.set.tracker)
            SetTimer(h, 4, 150, NULL);
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        int ev = HIWORD(wp);
        if (ev == BN_CLICKED) {
            on_command(id);
            return 0;
        }
        if (ev == EN_CHANGE && id == IDC_EDIT_SEARCH) {
            SetTimer(h, 2, 200, NULL);
            return 0;
        }
        if (ev == CBN_SELCHANGE && id == IDC_COMBO_SOURCE) {
            G.srcSel = (int)SendMessageA(G.hSrc, CB_GETCURSEL, 0, 0);
            rebuild_view();
            scroll_to_playing();
            return 0;
        }
        if (ev == CBN_SELCHANGE && id == IDC_COMBO_ORDER) {
            G.set.order = (int)SendMessageA(G.hOrder, CB_GETCURSEL, 0, 0);
            /* no redraw here: like the original, the drawn order survives
             * sort-order switches (ensure_shuffle_plan inside rebuild_view) */
            rebuild_view();
            scroll_to_playing();
            return 0;
        }
        if (ev == CBN_SELCHANGE && id == IDC_COMBO_SUBSONG) {
            int s = (int)SendMessageA(G.hSub, CB_GETCURSEL, 0, 0);
            /* with "play all subsongs" on, playback goes on from the pick */
            if (s >= 0 && G.engine.snap().loaded)
                load_current(s + 1);
            return 0;
        }
        if (ev == 0 || ev == 1)
            on_command(id); // menu (0) / accelerator (1)
        return 0;
    }
    case WM_HSCROLL: {
        if ((HWND)lp == G.hVol) {
            G.set.volume = (int)SendMessageA(G.hVol, TBM_GETPOS, 0, 0);
            G.engine.setVolume(G.set.volume, G.set.mute);
            update_vol_pct();
            return 0;
        }
        if ((HWND)lp == G.hSeek) {
            /* like the original: seek on press and on release; while
             * dragging only the time label follows the thumb */
            int code = LOWORD(wp);
            double sec = SendMessageA(G.hSeek, TBM_GETPOS, 0, 0) / 10.0;
            g_seekDrag = GetTickCount();
            if (code == TB_THUMBTRACK) {
                SetWindowTextA(G.hTime, m95_fmt_time(sec).c_str());
            } else if (code != TB_ENDTRACK &&
                       SendMessageA(G.hSeek, TBM_GETRANGEMAX, 0, 0) > 0 &&
                       G.engine.snap().loaded) {
                G.engine.seekTo(sec);
                SetWindowTextA(G.hTime, m95_fmt_time(sec).c_str());
            }
            return 0;
        }
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lp;
        if (nm->code == TCN_SELCHANGE && nm->idFrom == IDC_TAB) {
            bool pat = SendMessageA(G.hTab, TCM_GETCURSEL, 0, 0) == 1;
            if (pat != G.set.tracker) {
                trk_tab_set(pat);
                G.set.save();
            }
            return 0;
        }
        /* double-clicks are handled in lv_subproc: on comctl32 <= 4.70
         * NM_DBLCLK carries only an NMHDR (no item index to read) */
        if (nm->idFrom == IDC_LIST && nm->code == NM_RCLICK) {
            /* select the right-clicked row, then show a context menu */
            POINT pt;
            GetCursorPos(&pt);
            POINT cp = pt;
            ScreenToClient(G.hList, &cp);
            int hit = lv_row_at(cp.y);
            if (hit >= 0)
                ListView_SetItemState(G.hList, hit, LVIS_SELECTED | LVIS_FOCUSED,
                                      LVIS_SELECTED | LVIS_FOCUSED);
            HMENU m = CreatePopupMenu();
            AppendMenuA(m, MF_STRING, 5000, "Play");
            AppendMenuA(m, MF_SEPARATOR, 0, NULL);
            if (G.srcSel == 1)
                AppendMenuA(m, MF_STRING, 5002, "Remove from favorites");
            else
                AppendMenuA(m, MF_STRING, 5001, "Add to favorites");
            AppendMenuA(m, MF_STRING, 5003, "Ignore");
            if (G.srcSel >= 1)
                AppendMenuA(m, MF_STRING, 5004, "Remove from this list");
            AppendMenuA(m, MF_SEPARATOR, 0, NULL);
            AppendMenuA(m, MF_POPUP, (UINT_PTR)build_addto_menu(), "Add to list");
            int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0,
                                    G.hwnd, NULL);
            DestroyMenu(m);
            switch (id) {
            case 5000: {
                int sel = ListView_GetNextItem(G.hList, -1, LVNI_SELECTED);
                if (sel >= 0)
                    play_view_row(sel);
                break;
            }
            case 5001:
                fav_add_selected();
                break;
            case 5002:
            case 5004:
                remove_selected_from_current();
                break;
            case 5003:
                ignore_selected();
                break;
            default:
                if (id >= 6000)
                    do_addto(id);
            }
            return 0;
        }
        if (nm->idFrom == IDC_LIST && nm->code == LVN_KEYDOWN) {
            NMLVKEYDOWN *kd = (NMLVKEYDOWN *)lp;
            if (kd->wVKey == VK_RETURN) {
                int sel = ListView_GetNextItem(G.hList, -1, LVNI_SELECTED);
                if (sel >= 0)
                    play_view_row(sel);
            }
            return 0;
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == 1) {
            /* re-evaluate the background mode on every tick (cheap): while
             * minimized nothing but audio runs; our own modal dialogs count
             * as focused (they are owned top-level windows) */
            HWND fg = GetForegroundWindow();
            bool ours = (fg == h) || (fg && GetWindow(fg, GW_OWNER) == h);
            apply_power_mode(IsIconic(h) ? 2 : (ours ? 0 : 1));
            if (G.powerMode != 2)
                refresh_ui();
        } else if (wp == 2) {
            KillTimer(h, 2);
            rebuild_view();
        } else if (wp == 3) {
            KillTimer(h, 3);
            rebuild_view();
        } else if (wp == 4) {
            /* resize settled: refetch the tracker window for the new size */
            KillTimer(h, 4);
            if (G.set.tracker)
                trk_push(true);
        }
        return 0;
    case WM_APP_ENDED:
        /* wParam: generation of the load it belongs to - one from before
         * the latest load/stop is stale (would skip the new song or undo
         * Stop). lParam: 0 ended, 1 load failed, 2 sound device failed */
        if ((unsigned)wp != G.engine.generation())
            return 0;
        if (lp == 0)
            on_ended();
        else
            on_play_failed((int)lp);
        return 0;
    case WM_APP_ANALYZED: {
        AnaResult *r = (AnaResult *)lp;
        /* apply parsed fields here: every Track mutation runs on the UI
         * thread, so the analyzer never writes strings another thread reads */
        int src = r->src, idx = r->idx;
        if (r->valid) {
            EnterCriticalSection(&G.acs);
            std::vector<Track> &s = G.srcBy(src);
            if (idx < (int)s.size() && s[idx].path == r->path) {
                Track &t = s[idx];
                t.dur = r->dur;
                t.fmt = r->fmt;
                t.tracker = r->trk;
                t.modTitle = r->mt;
                t.ch = r->ch;
                t.ss = r->ss;
                t.broken = !r->loaded;
                t.analyzed = true;
                t.rekey(); /* fmt/modTitle feed the cached search keys */
            }
            LeaveCriticalSection(&G.acs);
            update_row(src, idx);
        }
        delete r;
        Track *t = current_track();
        if (t && src == G.playSrc && idx == G.playIdx) {
            char b[160];
            SetWindowTextA(G.hTitle, disp_mod_title(*t).c_str());
            std::string cap = "modjuke95 - " + caption_for(*t);
            SetWindowTextA(G.hwnd, cap.c_str());
            /* length/seek range: refresh_ui takes them from the engine (the
             * loaded subsong's real length); this only covers the moment
             * before the engine reports one */
            if (!(G.engine.snap().dur > 0)) {
                snprintf(b, sizeof(b), "Length: %s", m95_fmt_time(t->dur).c_str());
                SetWindowTextA(G.hLen, b);
                SetWindowTextA(G.hDur, m95_fmt_time(t->dur).c_str());
                set_seek_range(t->dur);
            }
            snprintf(b, sizeof(b), "Format: %s", t->fmt.empty() ? "?" : t->fmt.c_str());
            SetWindowTextA(G.hFmt, b);
            snprintf(b, sizeof(b), "Tracker: %s", t->tracker.empty() ? "?" : t->tracker.c_str());
            SetWindowTextA(G.hTrk, b);
            /* the song may have started unanalyzed with only "1" listed */
            fill_subsongs(t->ss, G.curSub);
        }
        /* length-based filters (and search by internal title) may hide/reveal
         * a track once it is analyzed; coalesce the rebuilds - during a whole
         * analysis run this would otherwise resort the list per track */
        if (G.set.minLen > 0 || G.set.maxLen > 0 || G.set.playableOnly ||
            GetWindowTextLengthA(G.hSearch) > 0)
            SetTimer(h, 3, 500, NULL);
        return 0;
    }
    case WM_APP_LOG: {
        char *s = (char *)lp;
        m95_log(G.hLog, "%s", s);
        free(s);
        return 0;
    }
    case WM_APP_ROWS: {
        RowsMsg *m = (RowsMsg *)lp;
        trk_apply_rows(m);
        delete m;
        return 0;
    }
    case WM_DROPFILES: {
        HDROP dr = (HDROP)wp;
        UINT n = DragQueryFileA(dr, 0xFFFFFFFF, NULL, 0);
        char path[MAX_PATH];
        /* dropping can drop previously-ignored files out of the library and
         * dedup drops later copies - both shift indices, so re-locate the
         * playing marker by path afterwards */
        std::string playPath;
        bool wasLib = G.playSrc == 0;
        if (wasLib) {
            Track *pt = current_track();
            if (pt)
                playPath = pt->path;
        }
        bool gone = false;
        EnterCriticalSection(&G.acs);
        /* batch membership set, same reasoning as do_open_files */
        std::set<std::string> libPaths;
        for (size_t j = 0; j < G.lib.size(); j++)
            libPaths.insert(m95_lower(G.lib[j].path));
        for (UINT i = 0; i < n; i++) {
            if (!DragQueryFileA(dr, i, path, MAX_PATH))
                continue;
            DWORD at = GetFileAttributesA(path);
            if (at != 0xFFFFFFFF && (at & FILE_ATTRIBUTE_DIRECTORY)) {
                scan_dir(path, G.exts, G.lib);
            } else if (has_module_ext(path, G.exts) && /* like folder drops */
                       !libPaths.count(m95_lower(path)) && !path_ignored(path)) {
                Track t;
                t.path = path;
                t.title = m95_basename(path);
                t.folder = m95_parentdir(path);
                t.rekey();
                G.lib.push_back(t);
                libPaths.insert(m95_lower(path));
            }
        }
        /* dropped directories may contain ignored files - strip them */
        {
            size_t w = 0;
            for (size_t i = 0; i < G.lib.size(); i++)
                if (!path_ignored(G.lib[i].path))
                    G.lib[w++] = G.lib[i];
            G.lib.resize(w);
        }
        /* dropping a directory already in the library would add every file
         * again - keep the library free of duplicates */
        dedup_lib();
        if (wasLib && !playPath.empty()) {
            G.playIdx = -1;
            for (size_t i = 0; i < G.lib.size(); i++)
                if (G.lib[i].path == playPath) {
                    G.playIdx = (int)i;
                    break;
                }
            gone = G.playIdx < 0;
            if (gone)
                G.playSrc = -1;
        }
        LeaveCriticalSection(&G.acs);
        DragFinish(dr);
        if (gone) {
            G.engine.stop();
            clear_playing();
            SendMessageA(G.hStatus, SB_SETTEXTA, 0, (LPARAM)"Playing song left the library - stopped");
        }
        rebuild_view();
        queue_analysis(0);
        save_all_lists();
        return 0;
    }
    case WM_ENDSESSION:
        /* Windows is shutting down: the process ends after this message,
         * WM_DESTROY never comes. Lists are saved on every change; save
         * what is otherwise only written at exit. */
        if (wp) {
            save_session();
            G.set.save();
            save_all_lists();
        }
        return 0;
    case WM_GETMINMAXINFO: {
        /* below this the controls can't be laid out usefully */
        RECT r = { 0, 0, 300, 280 };
        AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, TRUE, 0);
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = r.right - r.left;
        mm->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }
    case WM_DESTROY: {
        KillTimer(h, 1);
        save_session(); /* needs the engine's last position */
        EnterCriticalSection(&G.acs);
        G.aquit = true;
        LeaveCriticalSection(&G.acs);
        SetEvent(G.aEv);
        /* perf note (opt run 7): opt-in via [perf] note=1 in the ini;
         * one appended line per session so runs can be compared */
        if (GetPrivateProfileIntA("perf", "note", 0,
                                  (m95_exe_dir() + "modjuke95.ini").c_str())) {
            EngineSnap ps = G.engine.snap();
            MEMORYSTATUS mem;
            memset(&mem, 0, sizeof(mem));
            mem.dwLength = sizeof(mem);
            GlobalMemoryStatus(&mem);
            FILE *pf = fopen((m95_exe_dir() + "modjuke95-perf.log").c_str(), "a");
            if (pf) {
                fprintf(pf,
                        "%08lu perf: played %.0fs, row posts %ld, parses %u "
                        "(cache hits %u), phys avail %lu of %lu KB\n",
                        GetTickCount(), ps.played, ps.posts, g_parses, g_cacheHits,
                        mem.dwAvailPhys / 1024, mem.dwTotalPhys / 1024);
                fclose(pf);
            }
        }
        /* audio stops first - the analyzer wait below can take seconds */
        bool engGone = G.engine.shutdown();
        /* the analyzer may be inside a multi-second module parse; give it
         * room, and remember whether it really exited - the cache rewrite
         * and the critical-section teardown below both need it gone */
        bool anaGone = WaitForSingleObject(G.anaThr, 8000) == WAIT_OBJECT_0;
        CloseHandle(G.anaThr);
        CloseHandle(G.aEv);
        G.threadsGone = engGone && anaGone;
        G.set.save();
        save_all_lists();
        if (anaGone)
            acache_rewrite(); /* compact the appended analysis cache */
        if (G.hTrkFont)
            DeleteObject(G.hTrkFont);
        if (G.trkBg)
            DeleteObject(G.trkBg);
        if (G.trkHi)
            DeleteObject(G.trkHi);
        if (anaGone)
            DeleteCriticalSection(&G.acs); /* a live analyzer still uses it */
        PostQuitMessage(0);
        return 0;
    }
    default:
        return DefWindowProcA(h, msg, wp, lp);
    }
}

/* Global shortcuts (the accelerator table) vs. the focused control; there
 * is no dialog manager on the main window, so this decides who gets a key:
 *  - search box: typing keys stay in the edit; Esc clears the search,
 *    Tab/Enter/Down go to the list; F-keys and Ctrl+letter still work
 *  - an open drop-down list owns every key (Enter/Esc/arrows/letters)
 *  - a focused button or checkbox owns Space and Enter
 * Returns true when the message was handled here. */
static bool translate_keys(MSG *m)
{
    if (m->message != WM_KEYDOWN && m->message != WM_SYSKEYDOWN)
        return TranslateAcceleratorA(G.hwnd, G.hAcc, m) != 0;
    HWND f = GetFocus();
    UINT vk = (UINT)m->wParam;
    if (f == G.hSearch) {
        if (m->message == WM_KEYDOWN && vk == VK_ESCAPE) {
            on_command(IDM_CLEAR_SEARCH);
            return true;
        }
        if (m->message == WM_KEYDOWN && (vk == VK_TAB || vk == VK_RETURN || vk == VK_DOWN)) {
            SetFocus(G.hList);
            return true;
        }
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
        bool global = (vk >= VK_F1 && vk <= VK_F24) || (ctrl && vk >= 'A' && vk <= 'Z');
        if (!global)
            return false;
    } else if (f) {
        char cls[16];
        if (GetClassNameA(f, cls, sizeof(cls))) {
            if (lstrcmpiA(cls, "ComboBox") == 0 && SendMessageA(f, CB_GETDROPPEDSTATE, 0, 0))
                return false;
            /* Enter stays with a focused button; Space is always play/pause
             * (a clicked button keeps the focus, and Space pressing it
             * again - Clear list, Shuffle now - surprised users) */
            if (lstrcmpiA(cls, "Button") == 0 && vk == VK_RETURN)
                return false;
        }
    }
    return TranslateAcceleratorA(G.hwnd, G.hAcc, m) != 0;
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE, LPSTR, int show)
{
    m95_install_fault_filter();
    /* one instance per install folder: two copies would overwrite each
     * other's lists, analysis cache and settings (they share the files
     * next to the exe). The second start brings the first to the front. */
    {
        std::string d = m95_lower(m95_exe_dir());
        unsigned long hsh = 5381;
        for (size_t i = 0; i < d.size(); i++)
            hsh = hsh * 33 + (unsigned char)d[i];
        char mx[48];
        sprintf(mx, "modjuke95-%08lx", hsh); /* no '\\' allowed in the name */
        CreateMutexA(NULL, FALSE, mx); /* released when the process ends */
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND other = FindWindowA("modjuke95cls", NULL);
            if (other) {
                if (IsIconic(other))
                    ShowWindow(other, SW_RESTORE);
                SetForegroundWindow(other);
            }
            return 0;
        }
    }
    m95_stage("stage:winmain");
    OleInitialize(NULL);
    m95_stage("stage:ole");
    /* plain InitCommonControls: InitCommonControlsEx only exists in
     * comctl32 4.70+; the original Win95 comctl32 has only this one. */
    InitCommonControls();
    m95_stage("stage:controls");

    G.set.load();
    m95_stage("stage:libopenmpt");
    G.exts = mpt_supported_extensions();
    m95_stage("stage:exts-done");
    if (G.exts.empty()) {
        const char *d[] = { "669", "amf", "ams", "dbm", "dmf", "dsm", "dtm", "far",
                            "gdm", "it",  "j2b", "med", "mdl", "mod", "mptm", "mt2",
                            "mtm", "okt", "pt3", "s3m", "stm", "ult", "wow",  "xm" };
        for (int i = 0; i < 24; i++)
            G.exts.push_back(d[i]);
    }

    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = 0; /* no CS_HREDRAW/CS_VREDRAW: they invalidate the whole
                   * client area on every resize (redraw storm, audio drop) */
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    /* program icon: standard size for Explorer/Alt-Tab, small for the
     * title bar (LoadImage picks the matching 16x16 image from the ico) */
    wc.hIcon = LoadIconA(hi, MAKEINTRESOURCEA(IDI_APP));
    wc.hIconSm = (HICON)LoadImageA(hi, MAKEINTRESOURCEA(IDI_APP), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON),
                                   LR_DEFAULTCOLOR);
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "modjuke95cls";
    RegisterClassExA(&wc);

    WNDCLASSA pc;
    memset(&pc, 0, sizeof(pc));
    pc.lpfnWndProc = PatProc;
    pc.hInstance = hi;
    pc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    pc.lpszClassName = "m95patcls";
    RegisterClassA(&pc);

    G.hAcc = LoadAcceleratorsA(hi, "M95ACC");
    /* default size: 900x620, shrunk to fit the work area on small screens */
    RECT wa;
    wa.left = wa.top = 0;
    wa.right = 900;
    wa.bottom = 620;
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
    int cw = (int)(wa.right - wa.left) - 8;
    int ch = (int)(wa.bottom - wa.top) - 8;
    if (cw > 900)
        cw = 900;
    if (ch > 620)
        ch = 620;
    if (cw < 320)
        cw = 320;
    if (ch < 240)
        ch = 240;
    /* centered in the work area: a cascaded CW_USEDEFAULT position could
     * push a work-area-sized window under the taskbar at 640x480 */
    int wx = wa.left + ((int)(wa.right - wa.left) - cw) / 2;
    int wy = wa.top + ((int)(wa.bottom - wa.top) - ch) / 2;
    HWND hw = CreateWindowExA(0, "modjuke95cls", "modjuke95",
                              WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, wx < 0 ? 0 : wx,
                              wy < 0 ? 0 : wy, cw, ch, NULL, LoadMenuA(hi, "M95MENU"),
                              hi, NULL);
    if (!hw) { /* out of USER resources: don't sit invisible in GetMessage */
        MessageBoxA(NULL, "Could not create the main window (out of resources?)",
                    "modjuke95", MB_OK | MB_ICONSTOP);
        return 1;
    }
    G.hwnd = hw;
    m95_stage("stage:window");
    ShowWindow(hw, show);
    UpdateWindow(hw);
    m95_stage("stage:loop");

    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        if (!translate_keys(&m)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
    }
    OleUninitialize();
    if (!G.threadsGone)
        /* a worker thread didn't stop in time and still uses the globals:
         * end here, before static destructors free them under it
         * (everything worth keeping is saved already) */
        TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
