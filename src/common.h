#pragma once
#define WIN32_LEAN_AND_MEAN
#ifndef WINVER
#define WINVER 0x0400
#define _WIN32_WINDOWS 0x0400
#define _WIN32_WINNT 0x0400
#endif
#ifndef _WIN32_IE
#define _WIN32_IE 0x0400
#endif

#include <windows.h>
#include <mmsystem.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

std::string m95_lower(const std::string &s);        // codepage/DBCS-aware
size_t m95_last_sep(const std::string &p);           // last '\\' or '/', DBCS-safe
std::string m95_utf8_to_ansi(const std::string &s, bool onlyIfValid = false);
std::string m95_fmt_time(double sec);            // "m:ss", "--:--" when invalid
std::string m95_exe_dir();                       // trailing backslash
const char *m95_edition(); // "full" or "common formats" (edition.cpp)
std::string m95_basename(const std::string &p); // file name part
std::string m95_parentdir(const std::string &p);
bool m95_read_file(const std::string &path, std::vector<char> &out);
void m95_log(HWND edit, const char *fmt, ...);
std::string m95_trim(const std::string &s);
void m95_stage(const char *tag); // append startup progress marker to stage log
/* crash-safe file replace: write to dst + ".tmp", then m95_tmp_commit()
 * swaps it in only if every write and the close succeeded (else dst is
 * left untouched and the temp file removed) */
FILE *m95_tmp_open(const std::string &dst);
bool m95_tmp_commit(FILE *f, const std::string &dst);
std::string m95_join(const std::string &dir, const char *name); /* dir + sep + name, no doubled separator */
extern "C" void m95_install_fault_filter(void); // log hard faults to crash log
