#pragma once
#include "common.h"

struct Track
{
    std::string path;
    std::string title;   // display name (basename until analyzed)
    std::string modTitle; // internal module title (from metadata, once analyzed)
    std::string folder;  // parent dir name
    std::string fmt;     // short format, e.g. "xm"
    std::string tracker;
    double dur = -1;     // seconds; <0 unknown
    int ch = 0;
    int ss = 1;          // subsong count
    bool broken = false;
    bool analyzed = false;
    /* cached lowercase keys so sorting/searching never re-lowercases and
     * never allocates per comparison or per keystroke; call rekey() after
     * creating a track or changing title/fmt/modTitle. The file-type
     * extension is NOT cached: it is only needed by rebuild_view, where
     * ext_of() derives it on the fly (extensions stay inside SSO, so no
     * heap cost) - one less string per track in big libraries. */
    std::string ltitle, lfolder, lhay;
    void rekey();
};

struct Playlist
{
    std::string name;
    std::vector<Track> tr;
};

enum OrderMode { ORDER_ALPHA = 0, ORDER_PATH = 1, ORDER_SHUFFLE = 2 };

struct Settings
{
    int rate = 44100;
    int interpolation = 0;
    int bufferMs = 220;
    int bufCount = 6;
    int trkDelayMs = 0; /* extra display delay for the tracker view */
    int titleMode = 0;  /* window caption: 0 = module title, 1 = filename */
    int uiRefreshMs = 100; /* info tab + tracker refresh period */
    int volume = 80;
    bool mute = false;
    bool loop = false;
    bool repeat = false;
    bool playAll = false;
    double minLen = 0;
    double maxLen = 0;
    bool playableOnly = false;
    bool tracker = true;
    std::string types; /* ";"-separated enabled extensions; empty = all */
    int order = ORDER_ALPHA;
    void load();
    void save() const;
};

/* Scan a directory tree for supported module files. */
/* name ends in one of the module extensions (lowercase list) */
bool has_module_ext(const std::string &name, const std::vector<std::string> &exts);
void scan_dir(const std::string &root, const std::vector<std::string> &exts,
              std::vector<Track> &out);

/* M3U import/export. */
bool m3u_export(const std::string &path, const std::vector<Track> &tracks);
bool m3u_import(const std::string &path, std::vector<Track> &out);
