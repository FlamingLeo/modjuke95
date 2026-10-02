#include "common.h"

static bool has_high(const std::string &s)
{
    for (size_t i = 0; i < s.size(); i++)
        if ((unsigned char)s[i] >= 0x80)
            return true;
    return false;
}

std::string m95_lower(const std::string &s)
{
    std::string r = s;
    if (!has_high(r)) { /* plain ASCII: no API call */
        for (size_t i = 0; i < r.size(); i++)
            if (r[i] >= 'A' && r[i] <= 'Z')
                r[i] = char(r[i] + 32);
        return r;
    }
    /* codepage-aware like the file system (Ä/ä are one name on VFAT) and
     * DBCS-safe (CJK trail bytes 0x41-0x5A must not be "lowered") */
    if (!r.empty())
        CharLowerBuffA(&r[0], (DWORD)r.size());
    return r;
}

size_t m95_last_sep(const std::string &p)
{
    if (!has_high(p))
        return p.find_last_of("\\/");
    /* DBCS codepages: '\\' (0x5C) can be the trail byte of a character */
    size_t last = std::string::npos;
    for (size_t i = 0; i < p.size(); i++) {
        if (IsDBCSLeadByte((BYTE)p[i]) && i + 1 < p.size()) {
            i++;
            continue;
        }
        if (p[i] == '\\' || p[i] == '/')
            last = i;
    }
    return last;
}

/* UTF-8 (libopenmpt's metadata) -> the ANSI codepage the ANSI controls
 * show; Win95's MultiByteToWideChar has no CP_UTF8, so decode by hand.
 * onlyIfValid: leave the string alone unless it is well-formed UTF-8 with
 * non-ASCII bytes (strings cached by older builds). */
std::string m95_utf8_to_ansi(const std::string &s, bool onlyIfValid)
{
    if (!has_high(s))
        return s;
    std::vector<WCHAR> w;
    w.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        unsigned cp;
        int n;
        if (c < 0x80) {
            cp = c;
            n = 0;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            n = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            n = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            n = 3;
        } else {
            if (onlyIfValid)
                return s;
            w.push_back(L'?');
            i++;
            continue;
        }
        bool ok = true; /* each continuation byte is checked below */
        for (int k = 1; k <= n && ok; k++) {
            if (i + k >= s.size() || ((unsigned char)s[i + k] & 0xC0) != 0x80)
                ok = false;
            else
                cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        }
        if (!ok) {
            if (onlyIfValid)
                return s;
            w.push_back(L'?');
            i++;
            continue;
        }
        i += (size_t)n + 1;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            w.push_back((WCHAR)(0xD800 + (cp >> 10)));
            w.push_back((WCHAR)(0xDC00 + (cp & 0x3FF)));
        } else {
            w.push_back((WCHAR)cp);
        }
    }
    if (w.empty())
        return std::string();
    int len = WideCharToMultiByte(CP_ACP, 0, &w[0], (int)w.size(), NULL, 0, NULL, NULL);
    if (len <= 0)
        return s;
    std::string out((size_t)len, '\0');
    WideCharToMultiByte(CP_ACP, 0, &w[0], (int)w.size(), &out[0], len, NULL, NULL);
    return out;
}

std::string m95_fmt_time(double sec)
{
    if (!(sec > 0.0) || sec >= 1e18)
        return "--:--";
    long t = (long)(sec + 0.5);
    long m = t / 60, s = t % 60;
    char buf[32];
    sprintf(buf, "%ld:%02ld", m, s);
    return buf;
}

std::string m95_exe_dir()
{
    char buf[MAX_PATH] = "";
    GetModuleFileNameA(NULL, buf, MAX_PATH);
    std::string p = buf;
    size_t i = m95_last_sep(p);
    if (i == std::string::npos)
        return ".\\";
    return p.substr(0, i + 1);
}

std::string m95_basename(const std::string &p)
{
    size_t i = m95_last_sep(p);
    return i == std::string::npos ? p : p.substr(i + 1);
}

std::string m95_parentdir(const std::string &p)
{
    size_t i = m95_last_sep(p);
    if (i == std::string::npos)
        return "";
    std::string pre = p.substr(0, i);
    size_t j = m95_last_sep(pre);
    return j == std::string::npos ? pre : pre.substr(j + 1);
}

bool m95_read_file(const std::string &path, std::vector<char> &out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 24 * 1024 * 1024) {
        /* modules are a few MB at most; a 64 MB read would thrash or fail
         * outright on the 16-64 MB machines this targets */
        fclose(f);
        return false;
    }
    out.resize((size_t)n);
    size_t got = n ? fread(&out[0], 1, (size_t)n, f) : 0;
    fclose(f);
    return got == (size_t)n;
}

void m95_log(HWND edit, const char *fmt, ...)
{
    if (!edit)
        return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    strcat(buf, "\r\n");
    int len = GetWindowTextLengthA(edit);
    /* keep the log bounded: appending near the edit control's 30k limit both
     * fails and gets slower (whole-text memmove) the longer it grows */
    if (len > 24000) {
        SendMessageA(edit, EM_SETSEL, 0, 8000);
        SendMessageA(edit, EM_REPLACESEL, 0, (LPARAM)"");
    }
    len = GetWindowTextLengthA(edit);
    SendMessageA(edit, EM_SETSEL, len, len);
    SendMessageA(edit, EM_REPLACESEL, 0, (LPARAM)buf);
}

std::string m95_trim(const std::string &s)
{
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
        b++;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
        e--;
    return s.substr(b, e - b);
}

FILE *m95_tmp_open(const std::string &dst)
{
    return fopen((dst + ".tmp").c_str(), "w");
}

bool m95_tmp_commit(FILE *f, const std::string &dst)
{
    std::string tmp = dst + ".tmp";
    bool ok = !ferror(f);
    if (fclose(f) != 0) /* disk full shows up here at the latest */
        ok = false;
    if (ok) {
        /* Win95 has no MoveFileEx(REPLACE_EXISTING): remove + rename. A
         * crash in between leaves only the .tmp, which readers fall back to */
        remove(dst.c_str());
        ok = rename(tmp.c_str(), dst.c_str()) == 0;
    }
    if (!ok)
        remove(tmp.c_str());
    return ok;
}

std::string m95_join(const std::string &dir, const char *name)
{
    /* a drive root comes back from the shell as "D:\" */
    if (!dir.empty() && (dir[dir.size() - 1] == '\\' || dir[dir.size() - 1] == '/'))
        return dir + name;
    return dir + "\\" + name;
}

void m95_stage(const char *tag)
{
    char path[MAX_PATH];
    if (!GetModuleFileNameA(NULL, path, MAX_PATH))
        return;
    char *slash = strrchr(path, '\\');
    if (slash)
        slash[1] = '\0';
    else
        path[0] = '\0';
    /* a deep install folder (Win95 allows ~259 chars) must not overflow */
    if (strlen(path) + sizeof("modjuke95-stage.log") > sizeof(path))
        return;
    strcat(path, "modjuke95-stage.log");
    FILE *f = fopen(path, "a");
    if (f) {
        /* ms since boot; the differences between lines are the phase costs
         * of startup (opt run 6) */
        fprintf(f, "%08lu %s\n", GetTickCount(), tag);
        fclose(f);
    }
}

namespace
{
struct StageCtor
{
    StageCtor() { m95_stage("stage:global-ctors-common"); }
} g_stage_ctor;
}
