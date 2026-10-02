#include "model.h"
#include <algorithm>
#include <map>

void Track::rekey()
{
    ltitle = m95_lower(title);
    lfolder = m95_lower(folder);
    /* same haystack the original modjuke searches: dir/name fmt title path */
    lhay = lfolder + "/" + ltitle + " " + m95_lower(fmt) + " " +
           m95_lower(modTitle) + " " + m95_lower(path);
}

static std::string ini_path()
{
    return m95_exe_dir() + "modjuke95.ini";
}

void Settings::load()
{
    const std::string p = ini_path();
    rate = GetPrivateProfileIntA("audio", "rate", 44100, p.c_str());
    /* a garbled ini must never feed the engine or libopenmpt raw values */
    if (rate != 44100 && rate != 48000 && rate != 32000 && rate != 22050 &&
        rate != 11025)
        rate = 44100;
    interpolation = GetPrivateProfileIntA("audio", "interpolation", 0, p.c_str());
    if (interpolation != 0 && interpolation != 1 && interpolation != 2 &&
        interpolation != 4 && interpolation != 8)
        interpolation = 0;
    /* one-time migration: the old interpolation setting was broken (it went
     * to a non-existent ctl), so the stored default of 2 never took effect;
     * map it back to 0 so the sound stays as it always was */
    if (!GetPrivateProfileIntA("audio", "interpfix", 0, p.c_str()) && interpolation == 2)
        interpolation = 0;
    bufferMs = GetPrivateProfileIntA("audio", "bufferms", 220, p.c_str());
    /* 0 would size the wave buffers to nothing, huge values overflow the
     * size math in the engine; the presets live inside 50..2000 */
    if (bufferMs < 50)
        bufferMs = 50;
    if (bufferMs > 2000)
        bufferMs = 2000;
    bufCount = GetPrivateProfileIntA("audio", "bufcount", 6, p.c_str());
    trkDelayMs = GetPrivateProfileIntA("audio", "trkdelay", 0, p.c_str());
    if (trkDelayMs < 0)
        trkDelayMs = 0;
    if (trkDelayMs > 1000)
        trkDelayMs = 1000;
    if (bufCount < 2)
        bufCount = 2;
    if (bufCount > 10)
        bufCount = 10;
    volume = GetPrivateProfileIntA("ui", "volume", 80, p.c_str());
    if (volume < 0)
        volume = 0;
    if (volume > 100)
        volume = 100;
    mute = GetPrivateProfileIntA("ui", "mute", 0, p.c_str()) != 0;
    loop = GetPrivateProfileIntA("ui", "loop", 0, p.c_str()) != 0;
    repeat = GetPrivateProfileIntA("ui", "repeat", 0, p.c_str()) != 0;
    playAll = GetPrivateProfileIntA("ui", "playall", 0, p.c_str()) != 0;
    /* the original sorts "by directory" out of the box */
    order = GetPrivateProfileIntA("ui", "order", ORDER_PATH, p.c_str());
    if (order < 0 || order > 2)
        order = ORDER_PATH; /* guard against a garbled ini */
    char buf[64];
    GetPrivateProfileStringA("ui", "minlen", "0", buf, sizeof(buf), p.c_str());
    minLen = atof(buf);
    GetPrivateProfileStringA("ui", "maxlen", "0", buf, sizeof(buf), p.c_str());
    maxLen = atof(buf);
    /* garbled values (negative, NaN, or absurd) must not hide the whole
     * list; 0 means "no limit" (see rebuild_view) */
    if (!(minLen >= 0) || minLen > 3600)
        minLen = 0;
    if (!(maxLen >= 0) || maxLen > 3600)
        maxLen = 0;
    playableOnly = GetPrivateProfileIntA("ui", "playableonly", 0, p.c_str()) != 0;
    tracker = GetPrivateProfileIntA("ui", "tracker", 1, p.c_str()) != 0;
    titleMode = GetPrivateProfileIntA("ui", "titlemode", 0, p.c_str());
    if (titleMode < 0 || titleMode > 1)
        titleMode = 0;
    uiRefreshMs = GetPrivateProfileIntA("ui", "refresh", 100, p.c_str());
    if (uiRefreshMs < 20)
        uiRefreshMs = 20;
    if (uiRefreshMs > 1000)
        uiRefreshMs = 1000;
    char tb[512];
    GetPrivateProfileStringA("ui", "types", "", tb, sizeof(tb), p.c_str());
    types = tb;
}

static void wi(const char *sec, const char *key, long v)
{
    char buf[32];
    sprintf(buf, "%ld", v);
    WritePrivateProfileStringA(sec, key, buf, ini_path().c_str());
}

void Settings::save() const
{
    wi("audio", "rate", rate);
    wi("audio", "interpolation", interpolation);
    wi("audio", "interpfix", 1);
    wi("audio", "bufferms", bufferMs);
    wi("audio", "bufcount", bufCount);
    wi("audio", "trkdelay", trkDelayMs);
    wi("ui", "volume", volume);
    wi("ui", "mute", mute);
    wi("ui", "loop", loop);
    wi("ui", "repeat", repeat);
    wi("ui", "playall", playAll);
    wi("ui", "order", order);
    wi("ui", "playableonly", playableOnly);
    wi("ui", "tracker", tracker);
    wi("ui", "titlemode", titleMode);
    wi("ui", "refresh", uiRefreshMs);
    WritePrivateProfileStringA("ui", "types", types.c_str(), ini_path().c_str());
    char buf[32];
    sprintf(buf, "%.1f", minLen);
    WritePrivateProfileStringA("ui", "minlen", buf, ini_path().c_str());
    sprintf(buf, "%.1f", maxLen);
    WritePrivateProfileStringA("ui", "maxlen", buf, ini_path().c_str());
}

bool has_module_ext(const std::string &name, const std::vector<std::string> &exts)
{
    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos)
        return false;
    std::string e = m95_lower(name.substr(dot + 1));
    for (size_t i = 0; i < exts.size(); i++)
        if (exts[i] == e)
            return true;
    return false;
}

bool entry_supported(const std::string &path, const std::vector<std::string> &exts)
{
    if (has_module_ext(path, exts))
        return true;
    /* Amiga-style "mod.title": the type is the prefix of the file name */
    size_t sep = m95_last_sep(path);
    size_t b = sep == std::string::npos ? 0 : sep + 1;
    size_t dot = path.find('.', b);
    if (dot == std::string::npos || dot == b)
        return false;
    std::string e = m95_lower(path.substr(b, dot - b));
    for (size_t i = 0; i < exts.size(); i++)
        if (exts[i] == e)
            return true;
    return false;
}

void scan_dir(const std::string &root, const std::vector<std::string> &exts,
              std::vector<Track> &out)
{
    WIN32_FIND_DATAA fd;
    std::string spec = m95_join(root, "*");
    HANDLE h = FindFirstFileA(spec.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        std::string full = m95_join(root, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            /* system folders (the Recycle Bin "RECYCLED" keeps deleted
             * modules under renamed .MOD names) aren't part of a library */
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM))
                scan_dir(full, exts, out);
        } else if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) &&
                   has_module_ext(fd.cFileName, exts)) {
            Track t;
            t.path = full;
            t.title = fd.cFileName;
            t.folder = m95_parentdir(full);
            t.rekey();
            out.push_back(t);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void m3u_put_hidden(FILE *f, const HiddenEntry &h)
{
    fprintf(f, "#EXTINF:-1,%s\n%s\n", m95_basename(h.path).c_str(), h.path.c_str());
}

bool m3u_export(const std::string &path, const std::vector<Track> &tracks,
                const std::vector<HiddenEntry> *hidden)
{
    /* temp + swap: a crash, power loss or full disk mid-write must not
     * leave a truncated list behind */
    FILE *f = m95_tmp_open(path);
    if (!f)
        return false;
    fputs("#EXTM3U\n", f);
    /* hidden entries by the path they followed; each anchor is used once,
     * at its first occurrence (a map: big lists x many hidden entries) */
    std::map<std::string, std::vector<size_t>> after;
    if (hidden) {
        for (size_t i = 0; i < hidden->size(); i++) {
            if ((*hidden)[i].after.empty())
                m3u_put_hidden(f, (*hidden)[i]);
            else
                after[(*hidden)[i].after].push_back(i);
        }
    }
    for (size_t i = 0; i < tracks.size(); i++) {
        fprintf(f, "#EXTINF:-1,%s\n%s\n", tracks[i].title.c_str(), tracks[i].path.c_str());
        if (!after.empty()) {
            auto it = after.find(tracks[i].path);
            if (it != after.end()) {
                for (size_t k : it->second)
                    m3u_put_hidden(f, (*hidden)[k]);
                after.erase(it);
            }
        }
    }
    /* the entry they followed is gone: keep them, at the end, in order */
    if (!after.empty()) {
        std::vector<size_t> rest;
        for (auto &a : after)
            rest.insert(rest.end(), a.second.begin(), a.second.end());
        std::sort(rest.begin(), rest.end());
        for (size_t k : rest)
            m3u_put_hidden(f, (*hidden)[k]);
    }
    return m95_tmp_commit(f, path);
}

bool m3u_import(const std::string &path, std::vector<Track> &out,
                const std::vector<std::string> *exts, std::vector<HiddenEntry> *hidden)
{
    /* hidden entries follow the last visible entry read so far (or the
     * last visible one already in out, when importing into a list) */
    std::string prev = out.empty() ? std::string() : out.back().path;
    FILE *f = fopen(path.c_str(), "r");
    if (!f) /* a crash between remove and rename leaves only the .tmp */
        f = fopen((path + ".tmp").c_str(), "r");
    if (!f)
        return false;
    /* relative entries resolve against the folder of the m3u itself;
     * m95_parentdir() yields only the folder NAME, so do it inline */
    std::string base;
    {
        size_t di = m95_last_sep(path);
        if (di != std::string::npos)
            base = path.substr(0, di);
    }
    char line[1024];
    bool first = true;
    while (fgets(line, sizeof(line), f)) {
        if (!strchr(line, '\n') && !feof(f)) {
            /* longer than any Win95 path (a foreign m3u's long #EXTINF
             * title): skip the whole line instead of importing its tail
             * as bogus tracks */
            int ch;
            while ((ch = fgetc(f)) != EOF && ch != '\n') {
            }
            continue;
        }
        char *st = line;
        if (first) {
            first = false;
            /* tolerate a UTF-8 BOM (exported by other players) so it does
             * not turn "#EXTM3U" into a bogus path entry */
            if ((unsigned char)st[0] == 0xEF && (unsigned char)st[1] == 0xBB &&
                (unsigned char)st[2] == 0xBF)
                st += 3;
        }
        std::string s = m95_trim(st); /* also strips CRLF/LF line ends */
        if (s.empty() || s[0] == '#')
            continue;
        /* already absolute: drive letter ("C:..."), UNC ("\\server\..."),
         * or rooted ("\mods\..."); otherwise make it absolute */
        bool absolute = (s.size() >= 2 && s[1] == ':') || s[0] == '\\';
        if (!absolute && !base.empty())
            s = m95_join(base, s.c_str());
        if (exts && hidden && !entry_supported(s, *exts)) {
            HiddenEntry h;
            h.after = prev;
            h.path = s;
            hidden->push_back(h);
            continue;
        }
        prev = s;
        Track t;
        t.path = s;
        t.title = m95_basename(s);
        t.folder = m95_parentdir(s);
        t.rekey();
        out.push_back(t);
    }
    fclose(f);
    return true;
}
