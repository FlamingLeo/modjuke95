/* Win95-safe shadows of Win32 APIs that llvm-mingw's static C++ runtime
 * (libunwind/libc++/mingw CRT) imports but that do not exist on Win9x.
 *
 * Some callers (libunwind) reference the plain stdcall thunk symbol
 * (_Name@N), others (dllimport prototypes) call through the IAT entry
 * (__imp__Name@N). For every shadowed API we define BOTH: a real function
 * with the thunk's name and an __imp_ variable pointing at it. The linker
 * then never pulls the corresponding import-library member, so no import
 * for these APIs is emitted and the exe stays loadable on Win95/98.
 */
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <wctype.h>
#include <wchar.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>

extern "C"
{

/* ---------- boot lock ------------------------------------------------------- */

static volatile LONG g_bootflag = 0;
static CRITICAL_SECTION g_bootcs;
static volatile LONG g_bootready = 0;

static void boot95(void)
{
    if (g_bootready)
        return;
    if (InterlockedIncrement(&g_bootflag) == 1) {
        InitializeCriticalSection(&g_bootcs);
        InterlockedExchange(&g_bootready, 1);
    } else {
        while (!g_bootready)
            Sleep(0);
    }
}

/* ---------- SRW locks (used by libunwind's DWARF cache, throw path only) -- */
/* SRWLOCK is a single pointer-sized slot; we store an atomic state word in it:
 *   0        free
 *   1..n     n shared readers
 *   BIG      exclusive owner
 * Pure Interlocked ops, all available on Win95. */

#define SRW95_EXCLUSIVE 0x40000000L

static LONG *srw_state(SRWLOCK *l) { return (LONG *)&l->Ptr; }

void WINAPI shim_InitializeSRWLock(SRWLOCK *l) asm("_InitializeSRWLock@4");
void WINAPI shim_InitializeSRWLock(SRWLOCK *l) { l->Ptr = 0; }

void WINAPI shim_AcquireSRWLockExclusive(SRWLOCK *l) asm("_AcquireSRWLockExclusive@4");
void WINAPI shim_AcquireSRWLockExclusive(SRWLOCK *l)
{
    LONG *s = srw_state(l);
    while (InterlockedCompareExchange(s, SRW95_EXCLUSIVE, 0) != 0)
        Sleep(1);
}

void WINAPI shim_ReleaseSRWLockExclusive(SRWLOCK *l) asm("_ReleaseSRWLockExclusive@4");
void WINAPI shim_ReleaseSRWLockExclusive(SRWLOCK *l) { InterlockedExchange(srw_state(l), 0); }

void WINAPI shim_AcquireSRWLockShared(SRWLOCK *l) asm("_AcquireSRWLockShared@4");
void WINAPI shim_AcquireSRWLockShared(SRWLOCK *l)
{
    LONG *s = srw_state(l);
    for (;;) {
        LONG v = *s;
        if (v >= SRW95_EXCLUSIVE) {
            Sleep(1);
            continue;
        }
        if (InterlockedCompareExchange(s, v + 1, v) == v)
            return;
    }
}

void WINAPI shim_ReleaseSRWLockShared(SRWLOCK *l) asm("_ReleaseSRWLockShared@4");
void WINAPI shim_ReleaseSRWLockShared(SRWLOCK *l) { InterlockedDecrement(srw_state(l)); }

BOOLEAN WINAPI shim_TryAcquireSRWLockExclusive(SRWLOCK *l) asm("_TryAcquireSRWLockExclusive@4");
BOOLEAN WINAPI shim_TryAcquireSRWLockExclusive(SRWLOCK *l)
{
    return InterlockedCompareExchange(srw_state(l), SRW95_EXCLUSIVE, 0) == 0;
}

BOOLEAN WINAPI shim_TryAcquireSRWLockShared(SRWLOCK *l) asm("_TryAcquireSRWLockShared@4");
BOOLEAN WINAPI shim_TryAcquireSRWLockShared(SRWLOCK *l)
{
    LONG *s = srw_state(l);
    LONG v = *s;
    if (v >= SRW95_EXCLUSIVE)
        return FALSE;
    return InterlockedCompareExchange(s, v + 1, v) == v;
}

/* ---------- condition variables (coarse exclusive-wait semantics) ---------- */

struct CV95
{
    CRITICAL_SECTION cs;
    HANDLE ev; /* manual-reset */
    int generation;
    int waiters;
};

static CV95 *cv_get(PCONDITION_VARIABLE c)
{
    CV95 *p = (CV95 *)c->Ptr;
    if (!p) {
        boot95();
        EnterCriticalSection(&g_bootcs);
        p = (CV95 *)c->Ptr;
        if (!p) {
            p = (CV95 *)HeapAlloc(GetProcessHeap(), 0, sizeof(CV95));
            InitializeCriticalSection(&p->cs);
            p->ev = CreateEventA(NULL, TRUE, FALSE, NULL);
            p->generation = 0;
            p->waiters = 0;
            c->Ptr = p;
        }
        LeaveCriticalSection(&g_bootcs);
    }
    return p;
}

static BOOL cv_sleep(CV95 *cv, DWORD ms)
{
    EnterCriticalSection(&cv->cs);
    int gen = cv->generation;
    cv->waiters++;
    LeaveCriticalSection(&cv->cs);
    BOOL ok = FALSE;
    for (;;) {
        WaitForSingleObject(cv->ev, ms == INFINITE ? 100 : ms);
        EnterCriticalSection(&cv->cs);
        if (cv->generation != gen) {
            cv->waiters--;
            ResetEvent(cv->ev);
            LeaveCriticalSection(&cv->cs);
            ok = TRUE;
            break;
        }
        LeaveCriticalSection(&cv->cs);
        if (ms != INFINITE) {
            EnterCriticalSection(&cv->cs);
            cv->waiters--;
            LeaveCriticalSection(&cv->cs);
            break;
        }
    }
    return ok;
}

void WINAPI shim_InitializeConditionVariable(PCONDITION_VARIABLE c)
    asm("_InitializeConditionVariable@4");
void WINAPI shim_InitializeConditionVariable(PCONDITION_VARIABLE c) { c->Ptr = 0; }

BOOL WINAPI shim_SleepConditionVariableSRW(PCONDITION_VARIABLE c, PSRWLOCK l, DWORD ms, ULONG flags)
    asm("_SleepConditionVariableSRW@16");
BOOL WINAPI shim_SleepConditionVariableSRW(PCONDITION_VARIABLE c, PSRWLOCK l, DWORD ms, ULONG flags)
{
    CV95 *cv = cv_get(c);
    if (!(flags & 1)) /* CONDITION_VARIABLE_LOCK_SHARED */
        shim_ReleaseSRWLockExclusive(l);
    BOOL ok = cv_sleep(cv, ms);
    if (!(flags & 1))
        shim_AcquireSRWLockExclusive(l);
    return ok;
}

BOOL WINAPI shim_SleepConditionVariableCS(PCONDITION_VARIABLE c, PCRITICAL_SECTION cs, DWORD ms)
    asm("_SleepConditionVariableCS@12");
BOOL WINAPI shim_SleepConditionVariableCS(PCONDITION_VARIABLE c, PCRITICAL_SECTION cs, DWORD ms)
{
    CV95 *cv = cv_get(c);
    LeaveCriticalSection(cs);
    BOOL ok = cv_sleep(cv, ms);
    EnterCriticalSection(cs);
    return ok;
}

void WINAPI shim_WakeConditionVariable(PCONDITION_VARIABLE c) asm("_WakeConditionVariable@4");
void WINAPI shim_WakeConditionVariable(PCONDITION_VARIABLE c)
{
    CV95 *cv = cv_get(c);
    EnterCriticalSection(&cv->cs);
    if (cv->waiters > 0) {
        cv->generation++;
        SetEvent(cv->ev);
    }
    LeaveCriticalSection(&cv->cs);
}

void WINAPI shim_WakeAllConditionVariable(PCONDITION_VARIABLE c)
    asm("_WakeAllConditionVariable@4");
void WINAPI shim_WakeAllConditionVariable(PCONDITION_VARIABLE c)
{
    shim_WakeConditionVariable(c);
}

/* ---------- fiber-local storage -> TLS fallback ----------------------------- */

DWORD WINAPI shim_FlsAlloc(PFLS_CALLBACK_FUNCTION cb) asm("_FlsAlloc@4");
DWORD WINAPI shim_FlsAlloc(PFLS_CALLBACK_FUNCTION cb)
{
    (void)cb;
    return TlsAlloc();
}

BOOL WINAPI shim_FlsFree(DWORD d) asm("_FlsFree@4");
BOOL WINAPI shim_FlsFree(DWORD d) { return TlsFree(d); }

PVOID WINAPI shim_FlsGetValue(DWORD d) asm("_FlsGetValue@4");
PVOID WINAPI shim_FlsGetValue(DWORD d) { return TlsGetValue(d); }

BOOL WINAPI shim_FlsSetValue(DWORD d, PVOID p) asm("_FlsSetValue@8");
BOOL WINAPI shim_FlsSetValue(DWORD d, PVOID p) { return TlsSetValue(d, p); }

/* ---------- wide library helpers -> ANSI wrappers --------------------------- */

HMODULE WINAPI shim_GetModuleHandleW(LPCWSTR name) asm("_GetModuleHandleW@4");
HMODULE WINAPI shim_GetModuleHandleW(LPCWSTR name)
{
    if (!name)
        return GetModuleHandleA(NULL);
    char buf[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, name, -1, buf, MAX_PATH, NULL, NULL);
    return GetModuleHandleA(buf);
}

HMODULE WINAPI shim_LoadLibraryW(LPCWSTR name) asm("_LoadLibraryW@4");
HMODULE WINAPI shim_LoadLibraryW(LPCWSTR name)
{
    char buf[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, name, -1, buf, MAX_PATH, NULL, NULL);
    return LoadLibraryA(buf);
}

HMODULE WINAPI shim_LoadLibraryExW(LPCWSTR name, HANDLE h, DWORD flags)
    asm("_LoadLibraryExW@12");
HMODULE WINAPI shim_LoadLibraryExW(LPCWSTR name, HANDLE h, DWORD flags)
{
    char buf[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, name, -1, buf, MAX_PATH, NULL, NULL);
    return LoadLibraryExA(buf, h, flags);
}

BOOL WINAPI shim_GetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE *out)
    asm("_GetModuleHandleExW@12");
BOOL WINAPI shim_GetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE *out)
{
    if (!out)
        return FALSE;
    if (flags & 2 /* GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS */) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((LPCVOID)name, &mbi, sizeof(mbi)))
            return FALSE;
        *out = (HMODULE)mbi.AllocationBase;
        return *out != NULL;
    }
    char buf[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, name, -1, buf, MAX_PATH, NULL, NULL);
    *out = GetModuleHandleA(buf);
    return *out != NULL;
}

/* ---------- PSAPI-via-kernel32 (Vista+ K32 aliases) ------------------------- */

BOOL WINAPI shim_K32EnumProcessModules(HANDLE proc, HMODULE *mods, DWORD cb, DWORD *needed)
    asm("_K32EnumProcessModules@16");
BOOL WINAPI shim_K32EnumProcessModules(HANDLE proc, HMODULE *mods, DWORD cb, DWORD *needed)
{
    /* Win95 has no PSAPI; report just the main executable. libunwind only
     * uses this to scan modules for DWARF unwind info - our app never loads
     * DLLs containing C++ exceptions. */
    (void)proc;
    if (needed)
        *needed = sizeof(HMODULE);
    if (!mods || cb < sizeof(HMODULE))
        return FALSE;
    mods[0] = GetModuleHandleA(NULL);
    return TRUE;
}

/* ---------- Vista/Win8 kernel32 additions used by modern libc++ ------------- */

void WINAPI shim_GetSystemTimePreciseAsFileTime(LPFILETIME ft)
    asm("_GetSystemTimePreciseAsFileTime@4");
void WINAPI shim_GetSystemTimePreciseAsFileTime(LPFILETIME ft)
{
    GetSystemTimeAsFileTime(ft);
}

int WINAPI shim_GetLocaleInfoExW(LPCWSTR locale, DWORD type, LPWSTR buf, int n)
    asm("_GetLocaleInfoEx@16");
int WINAPI shim_GetLocaleInfoExW(LPCWSTR locale, DWORD type, LPWSTR buf, int n)
{
    /* Win95 has no name-based locale API (and no W): map the name to an LCID
     * coarsely and go through GetLocaleInfoA. Only used by std::locale. */
    LCID lcid = LOCALE_USER_DEFAULT;
    if (locale && locale[0]) {
        static const struct
        {
            const wchar_t *pfx;
            WORD lang;
        } tab[] = { { L"en", LANG_ENGLISH }, { L"de", LANG_GERMAN },
                    { L"fr", LANG_FRENCH },  { L"es", LANG_SPANISH },
                    { L"it", LANG_ITALIAN } };
        for (int i = 0; i < 5; i++)
            if (wcsncmp(locale, tab[i].pfx, 2) == 0)
                lcid = MAKELCID(MAKELANGID(tab[i].lang, SUBLANG_DEFAULT), SORT_DEFAULT);
    }
    char out[512];
    int r = GetLocaleInfoA(lcid, type, out, sizeof(out));
    if (r <= 0)
        return 0;
    if (buf && n > 0) {
        MultiByteToWideChar(CP_ACP, 0, out, -1, buf, n);
        if (r > n)
            r = n;
    }
    return r;
}

/* ---------- CRT exports CRTDLL.DLL lacks (Vista-era libc++ hooks) --------- */
/* libc++'s locale/support code references _l (locale-reentrant) and _s
 * (secure) CRT variants that only exist in Vista+ msvcrt.dll. None of our
 * code paths care about the extra locale parameter - delegate to the classic
 * functions that CRTDLL.DLL has. */

int shim_mbtowc_l(wchar_t *dst, const char *src, size_t n, void *loc) asm("__mbtowc_l");
int shim_mbtowc_l(wchar_t *dst, const char *src, size_t n, void *loc)
{
    (void)loc;
    return mbtowc(dst, src, n);
}

double shim_strtod_l(const char *s, char **end, void *loc) asm("__strtod_l");
double shim_strtod_l(const char *s, char **end, void *loc)
{
    (void)loc;
    return strtod(s, end);
}

/* locale-reentrant ctype/collation variants (locale ignored) */
int shim_iswalpha_l(wint_t c, void *l) asm("__iswalpha_l");
int shim_iswalpha_l(wint_t c, void *l) { (void)l; return iswalpha(c); }
int shim_iswupper_l(wint_t c, void *l) asm("__iswupper_l");
int shim_iswupper_l(wint_t c, void *l) { (void)l; return iswupper(c); }
int shim_iswlower_l(wint_t c, void *l) asm("__iswlower_l");
int shim_iswlower_l(wint_t c, void *l) { (void)l; return iswlower(c); }
int shim_iswdigit_l(wint_t c, void *l) asm("__iswdigit_l");
int shim_iswdigit_l(wint_t c, void *l) { (void)l; return iswdigit(c); }
int shim_iswxdigit_l(wint_t c, void *l) asm("__iswxdigit_l");
int shim_iswxdigit_l(wint_t c, void *l) { (void)l; return iswxdigit(c); }
int shim_iswspace_l(wint_t c, void *l) asm("__iswspace_l");
int shim_iswspace_l(wint_t c, void *l) { (void)l; return iswspace(c); }
int shim_iswprint_l(wint_t c, void *l) asm("__iswprint_l");
int shim_iswprint_l(wint_t c, void *l) { (void)l; return iswprint(c); }
int shim_iswpunct_l(wint_t c, void *l) asm("__iswpunct_l");
int shim_iswpunct_l(wint_t c, void *l) { (void)l; return iswpunct(c); }
int shim_iswcntrl_l(wint_t c, void *l) asm("__iswcntrl_l");
int shim_iswcntrl_l(wint_t c, void *l) { (void)l; return iswcntrl(c); }
int shim_iswctype_l(wint_t c, wctype_t t, void *l) asm("__iswctype_l");
int shim_iswctype_l(wint_t c, wctype_t t, void *l) { (void)l; return iswctype(c, t); }
wint_t shim_towlower_l(wint_t c, void *l) asm("__towlower_l");
wint_t shim_towlower_l(wint_t c, void *l) { (void)l; return towlower(c); }
wint_t shim_towupper_l(wint_t c, void *l) asm("__towupper_l");
wint_t shim_towupper_l(wint_t c, void *l) { (void)l; return towupper(c); }
int shim_strcoll_l(const char *a, const char *b, void *l) asm("__strcoll_l");
int shim_strcoll_l(const char *a, const char *b, void *l) { (void)l; return strcoll(a, b); }
size_t shim_strxfrm_l(char *d, const char *s, size_t n, void *l) asm("__strxfrm_l");
size_t shim_strxfrm_l(char *d, const char *s, size_t n, void *l) { (void)l; return strxfrm(d, s, n); }
int shim_wcscoll_l(const wchar_t *a, const wchar_t *b, void *l) asm("__wcscoll_l");
int shim_wcscoll_l(const wchar_t *a, const wchar_t *b, void *l) { (void)l; return wcscoll(a, b); }
size_t shim_wcsxfrm_l(wchar_t *d, const wchar_t *s, size_t n, void *l) asm("__wcsxfrm_l");
size_t shim_wcsxfrm_l(wchar_t *d, const wchar_t *s, size_t n, void *l) { (void)l; return wcsxfrm(d, s, n); }
int shim_tolower_l(int c, void *l) asm("__tolower_l");
int shim_tolower_l(int c, void *l) { (void)l; return tolower(c); }
int shim_toupper_l(int c, void *l) asm("__toupper_l");
int shim_toupper_l(int c, void *l) { (void)l; return toupper(c); }

int shim_wcrtomb_s(size_t *ret, char *s, size_t n, wchar_t wc, void *st) asm("_wcrtomb_s");
int shim_wcrtomb_s(size_t *ret, char *s, size_t n, wchar_t wc, void *st)
{
    (void)st;
    if (ret)
        *ret = 0;
    if (!s || n == 0)
        return 22 /* EINVAL */;
    /* convert into a local buffer first: wctomb may write up to
     * MB_CUR_MAX bytes, more than the n the caller has left */
    char tmp[MB_LEN_MAX];
    int r = wctomb(tmp, wc);
    if (r < 0 || (size_t)r > n) {
        s[0] = '\0';
        return 34 /* ERANGE */;
    }
    memcpy(s, tmp, (size_t)r);
    if (ret)
        *ret = (size_t)r;
    return 0;
}

/* rand_s only exists in msvcrt 8+, not in CRTDLL; seed-mix rand() is fine
 * for our uses */
int shim_rand_s(unsigned int *v) asm("_rand_s");
int shim_rand_s(unsigned int *v)
{
    *v = ((unsigned)rand() << 16) ^ (unsigned)rand();
    return 0;
}

/* _aligned_* only exist in msvcrt 8+ (Vista), not in CRTDLL; implement with
 * a back-pointer */
/* header layout before the returned pointer: { void *raw; size_t size; } */
void *shim_aligned_malloc(size_t size, size_t align) asm("__aligned_malloc");
void *shim_aligned_malloc(size_t size, size_t align)
{
    if (align < sizeof(void *))
        align = sizeof(void *);
    char *raw = (char *)malloc(size + align + 2 * sizeof(void *));
    if (!raw)
        return NULL;
    char *p = (char *)(((uintptr_t)raw + 2 * sizeof(void *) + align - 1) & ~(align - 1));
    ((void **)p)[-2] = raw;
    ((size_t *)p)[-1] = size;
    return p;
}

void shim_aligned_free(void *p) asm("__aligned_free");
void shim_aligned_free(void *p)
{
    if (p)
        free(((void **)p)[-2]);
}

void *shim_aligned_realloc(void *p, size_t size, size_t align) asm("__aligned_realloc");
void *shim_aligned_realloc(void *p, size_t size, size_t align)
{
    if (!p)
        return shim_aligned_malloc(size, align);
    size_t oldsz = ((size_t *)p)[-1];
    void *q = shim_aligned_malloc(size, align);
    if (!q)
        return NULL;
    memcpy(q, p, oldsz < size ? oldsz : size);
    shim_aligned_free(p);
    return q;
}

/* ---------- referenced only by discarded code -------------------------------
 * libc++ code that --gc-sections drops later still references these, and
 * the Win95 CRTDLL.DLL doesn't export them, so the link needs definitions.
 * They fail cleanly in case one ever becomes reachable. */
FILE *shim_wfopen(const wchar_t *name, const wchar_t *mode)
{
    (void)name;
    (void)mode;
    errno = ENOENT;
    return NULL;
}

void *shim_create_locale(int cat, const char *name)
{
    (void)cat;
    (void)name;
    return NULL;
}

void shim_free_locale(void *l)
{
    (void)l;
}

int shim_strerror_s(char *buf, size_t n, int err)
{
    if (!buf || n == 0)
        return 22 /* EINVAL */;
    const char *m = strerror(err);
    size_t len = strlen(m);
    if (len >= n)
        len = n - 1;
    memcpy(buf, m, len);
    buf[len] = '\0';
    return 0;
}

/* ---------- no C++ demangler ------------------------------------------------
 * libc++abi's default terminate handler is the only user of __cxa_demangle;
 * defining it here keeps cxa_demangle.o (~166 KB of code and tables) out of
 * the exe. Status -2 ("invalid name") makes the handler print the mangled
 * type name instead. */
extern "C" char *__cxa_demangle(const char *, char *, size_t *, int *status)
{
    if (status)
        *status = -2;
    return NULL;
}

/* ---------- startup crash logger --------------------------------------------
 * Shadow the CRT's abort/_amsg_exit/_assert so a failure during CRT or global
 * init leaves a modjuke95-crash.log next to the exe (caller EIP + EBP walk),
 * mappable via the link map instead of just a dialog box. */

extern "C" IMAGE_DOS_HEADER __ImageBase; /* linker-provided: our own image */

static FILE *crashlog_open(const char *tag, void *ret)
{
    char path[MAX_PATH];
    if (!GetModuleFileNameA(NULL, path, MAX_PATH))
        return NULL;
    char *slash = strrchr(path, '\\');
    if (slash)
        slash[1] = '\0';
    else
        path[0] = '\0';
    if (strlen(path) + sizeof("modjuke95-crash.log") > sizeof(path))
        return NULL; /* deep install folder: no room for the name */
    strcat(path, "modjuke95-crash.log");
    FILE *f = fopen(path, "a");
    if (!f)
        return NULL;
    setvbuf(f, NULL, _IONBF, 0); /* never lose lines to stdio buffering */
    fprintf(f, "%s ret=%p", tag, ret);
    /* EBP chains are unusable (runtime built without frame pointers).
     * Instead scan the live stack upwards for plausible return addresses,
     * i.e. values inside the image's .text section, whose bounds come from
     * our own PE header (a hard-coded range goes stale as the code grows).
     * The scan stops at the thread's stack top (TIB fs:[4], same offset on
     * Win9x and NT): reading past it can hit unmapped memory and fault
     * inside the logger. */
    unsigned long tlo = 0, thi = 0;
    {
        const BYTE *base = (const BYTE *)&__ImageBase;
        const IMAGE_NT_HEADERS *nt =
            (const IMAGE_NT_HEADERS *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
        const IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++, sh++)
            if (memcmp(sh->Name, ".text", 6) == 0) {
                tlo = (unsigned long)(base + sh->VirtualAddress);
                thi = tlo + sh->Misc.VirtualSize;
            }
    }
    void **sp, **top;
    __asm__ volatile("movl %%esp, %0" : "=r"(sp));
    __asm__ volatile("movl %%fs:4, %0" : "=r"(top));
    int n = 0;
    for (int i = 0; i < 512 && sp + i < top; i++) {
        unsigned long a = (unsigned long)sp[i];
        if (a >= tlo && a < thi) {
            fprintf(f, " %p", (void *)a);
            if (++n >= 24)
                break;
        }
    }
    fprintf(f, "\n");
    return f;
}

/* a stack overflow leaves the faulting thread almost no stack: the log is
 * written from a fresh thread instead */
struct FaultInfo
{
    DWORD code;
    void *eip;
};
static DWORD WINAPI fault_log_thread(LPVOID p)
{
    FaultInfo *fi = (FaultInfo *)p;
    char tag[96];
    sprintf(tag, "FAULT code=%08lx eip=%p (stack overflow)", (unsigned long)fi->code, fi->eip);
    FILE *f = crashlog_open(tag, fi->eip);
    if (f)
        fclose(f);
    return 0;
}

/* Catch hard faults (page fault, illegal instruction, div0, stack overflow)
 * so they leave a crash log instead of just a dialog. */
static LONG WINAPI m95_fault_filter(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        static FaultInfo fi; /* not on the exhausted stack */
        fi.code = ep->ExceptionRecord->ExceptionCode;
        fi.eip = ep->ExceptionRecord->ExceptionAddress;
        DWORD tid;
        HANDLE th = CreateThread(NULL, 0x10000, fault_log_thread, &fi, 0, &tid);
        if (th) {
            WaitForSingleObject(th, 5000);
            CloseHandle(th);
        }
        return EXCEPTION_EXECUTE_HANDLER;
    }
    char tag[96];
    sprintf(tag, "FAULT code=%08lx eip=%p",
            (unsigned long)ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress);
    FILE *f = crashlog_open(tag, ep->ExceptionRecord->ExceptionAddress);
    if (f)
        fclose(f);
    return EXCEPTION_EXECUTE_HANDLER;
}

void m95_install_fault_filter(void)
{
    SetUnhandledExceptionFilter(m95_fault_filter);
}

void shim_abort(void) asm("_abort");
void shim_abort(void)
{
    FILE *f = crashlog_open("ABORT", __builtin_return_address(0));
    if (f)
        fclose(f);
    ExitProcess(77);
}

void shim_amsg_exit(int code) asm("__amsg_exit");
void shim_amsg_exit(int code)
{
    char tag[32];
    strcpy(tag, "AMSG_exit_");
    tag[10] = (char)('0' + (code / 10) % 10);
    tag[11] = (char)('0' + code % 10);
    tag[12] = '\0';
    FILE *f = crashlog_open(tag, __builtin_return_address(0));
    if (f)
        fclose(f);
    ExitProcess(78);
}

void shim_assert(const char *expr, const char *file, int line) asm("__assert");
void shim_assert(const char *expr, const char *file, int line)
{
    FILE *f = crashlog_open("ASSERT", __builtin_return_address(0));
    if (f) {
        fprintf(f, "assert %s %s:%d\n", expr ? expr : "?", file ? file : "?", line);
        fclose(f);
    }
    ExitProcess(79);
}

/* ---------- expose as IAT (__imp_) symbols ---------------------------------- */

#define IMP(var, deco, fn) void *var asm("__imp__" #deco) = (void *)(fn)

IMP(imp1, AcquireSRWLockExclusive@4, &shim_AcquireSRWLockExclusive);
IMP(imp2, ReleaseSRWLockExclusive@4, &shim_ReleaseSRWLockExclusive);
IMP(imp3, AcquireSRWLockShared@4, &shim_AcquireSRWLockShared);
IMP(imp4, ReleaseSRWLockShared@4, &shim_ReleaseSRWLockShared);
IMP(imp5, TryAcquireSRWLockExclusive@4, &shim_TryAcquireSRWLockExclusive);
IMP(imp6, TryAcquireSRWLockShared@4, &shim_TryAcquireSRWLockShared);
IMP(imp7, InitializeSRWLock@4, &shim_InitializeSRWLock);
IMP(imp8, InitializeConditionVariable@4, &shim_InitializeConditionVariable);
IMP(imp9, SleepConditionVariableSRW@16, &shim_SleepConditionVariableSRW);
IMP(imp10, SleepConditionVariableCS@12, &shim_SleepConditionVariableCS);
IMP(imp11, WakeConditionVariable@4, &shim_WakeConditionVariable);
IMP(imp12, WakeAllConditionVariable@4, &shim_WakeAllConditionVariable);
IMP(imp13, FlsAlloc@4, &shim_FlsAlloc);
IMP(imp14, FlsFree@4, &shim_FlsFree);
IMP(imp15, FlsGetValue@4, &shim_FlsGetValue);
IMP(imp16, FlsSetValue@8, &shim_FlsSetValue);
IMP(imp17, GetModuleHandleW@4, &shim_GetModuleHandleW);
IMP(imp18, LoadLibraryW@4, &shim_LoadLibraryW);
IMP(imp19, LoadLibraryExW@12, &shim_LoadLibraryExW);
IMP(imp20, GetModuleHandleExW@12, &shim_GetModuleHandleExW);
IMP(imp21, _mbtowc_l, &shim_mbtowc_l);
IMP(imp22, _strtod_l, &shim_strtod_l);
IMP(imp23, wcrtomb_s, &shim_wcrtomb_s);
IMP(imp24, K32EnumProcessModules@16, &shim_K32EnumProcessModules);
IMP(imp25, _iswalpha_l, &shim_iswalpha_l);
IMP(imp26, _iswupper_l, &shim_iswupper_l);
IMP(imp27, _iswlower_l, &shim_iswlower_l);
IMP(imp28, _iswdigit_l, &shim_iswdigit_l);
IMP(imp29, _iswxdigit_l, &shim_iswxdigit_l);
IMP(imp30, _iswspace_l, &shim_iswspace_l);
IMP(imp31, _iswprint_l, &shim_iswprint_l);
IMP(imp32, _iswpunct_l, &shim_iswpunct_l);
IMP(imp33, _iswcntrl_l, &shim_iswcntrl_l);
IMP(imp34, _iswctype_l, &shim_iswctype_l);
IMP(imp35, _towlower_l, &shim_towlower_l);
IMP(imp36, _towupper_l, &shim_towupper_l);
IMP(imp37, _strcoll_l, &shim_strcoll_l);
IMP(imp38, _strxfrm_l, &shim_strxfrm_l);
IMP(imp39, _wcscoll_l, &shim_wcscoll_l);
IMP(imp40, _wcsxfrm_l, &shim_wcsxfrm_l);
IMP(imp41, _tolower_l, &shim_tolower_l);
IMP(imp42, _toupper_l, &shim_toupper_l);
IMP(imp43, rand_s, &shim_rand_s);
IMP(imp44, GetSystemTimePreciseAsFileTime@4, &shim_GetSystemTimePreciseAsFileTime);
IMP(imp45, GetLocaleInfoEx@16, &shim_GetLocaleInfoExW);
IMP(imp46, _aligned_malloc, &shim_aligned_malloc);
IMP(imp47, _aligned_free, &shim_aligned_free);
IMP(imp48, _aligned_realloc, &shim_aligned_realloc);
IMP(imp49, abort, &shim_abort);
IMP(imp50, _amsg_exit, &shim_amsg_exit);
IMP(imp51, _assert, &shim_assert);
IMP(imp52, _wfopen, &shim_wfopen);
IMP(imp53, _create_locale, &shim_create_locale);
IMP(imp54, _free_locale, &shim_free_locale);
IMP(imp55, strerror_s, &shim_strerror_s);

#undef IMP

} /* extern "C" */
