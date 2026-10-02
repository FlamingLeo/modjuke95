/* Win95-safe replacements for libc++'s thread_win32 primitives.
 *
 * llvm-mingw's libc++ implements std::__1::__libcpp_* with SRW locks and
 * condition variables that only exist on Vista+. Pulling that object in would
 * make the exe unloadable on Windows 95/98. By defining the same mangled
 * symbols ourselves (CRITICAL_SECTION / semaphore based, all Win95-native) the
 * linker never selects the SRW object.
 *
 * SIGNATURES MUST MATCH libc++ v23 __thread/support/windows.h EXACTLY:
 * int-returning functions return 0 on success; cxa_guard treats any nonzero
 * as "failed to acquire mutex" and calls abort_message -> abort. (An earlier
 * void-returning version left EAX garbage and caused random startup aborts.)
 */
#include <windows.h>
#include <time.h>
#include <chrono>

namespace std
{
inline namespace __1
{

static CRITICAL_SECTION g_boot;
static volatile LONG g_bootClaim = 0, g_bootDone = 0;
/* one-time init of g_boot, safe when two threads arrive at once: the
 * winner initializes and then publishes "done"; everyone else waits for
 * that (the old single counter let a second thread through early, onto an
 * uninitialized critical section). Sleep(1), not Sleep(0): Sleep(0) only
 * yields to equal priority, so a time-critical waiter could starve the
 * normal-priority initializer forever. Interlocked* compile to inline
 * xchg here, no kernel32 import. */
static void boot()
{
    if (g_bootDone)
        return;
    if (InterlockedExchange(&g_bootClaim, 1) == 0) {
        InitializeCriticalSection(&g_boot);
        InterlockedExchange(&g_bootDone, 1);
    } else {
        while (!g_bootDone)
            Sleep(1);
    }
}

struct M95M
{
    CRITICAL_SECTION cs;
};
struct M95CV
{
    CRITICAL_SECTION cs;
    HANDLE sem;
    int waiters;
};

static M95M *mget(void **m)
{
    boot();
    M95M *p = (M95M *)*m;
    if (!p) {
        EnterCriticalSection(&g_boot);
        p = (M95M *)*m;
        if (!p) {
            p = (M95M *)HeapAlloc(GetProcessHeap(), 0, sizeof(M95M));
            InitializeCriticalSection(&p->cs);
            *m = p;
        }
        LeaveCriticalSection(&g_boot);
    }
    return p;
}

static M95CV *cvget(void **c)
{
    boot();
    M95CV *p = (M95CV *)*c;
    if (!p) {
        EnterCriticalSection(&g_boot);
        p = (M95CV *)*c;
        if (!p) {
            p = (M95CV *)HeapAlloc(GetProcessHeap(), 0, sizeof(M95CV));
            InitializeCriticalSection(&p->cs);
            p->sem = CreateSemaphoreA(NULL, 0, 32767, NULL);
            p->waiters = 0;
            *c = p;
        }
        LeaveCriticalSection(&g_boot);
    }
    return p;
}

int __libcpp_mutex_lock(void **m)
{
    EnterCriticalSection(&mget(m)->cs);
    return 0;
}
int __libcpp_mutex_unlock(void **m)
{
    LeaveCriticalSection(&mget(m)->cs);
    return 0;
}
bool __libcpp_mutex_trylock(void **m) { return TryEnterCriticalSection(&mget(m)->cs) != 0; }
int __libcpp_mutex_destroy(void **m)
{
    M95M *p = (M95M *)*m;
    if (p) {
        DeleteCriticalSection(&p->cs);
        HeapFree(GetProcessHeap(), 0, p);
        *m = 0;
    }
    return 0;
}

int __libcpp_condvar_signal(void **c)
{
    M95CV *p = cvget(c);
    EnterCriticalSection(&p->cs);
    if (p->waiters > 0)
        ReleaseSemaphore(p->sem, 1, NULL);
    LeaveCriticalSection(&p->cs);
    return 0;
}
int __libcpp_condvar_broadcast(void **c)
{
    M95CV *p = cvget(c);
    EnterCriticalSection(&p->cs);
    if (p->waiters > 0)
        ReleaseSemaphore(p->sem, p->waiters, NULL);
    LeaveCriticalSection(&p->cs);
    return 0;
}
int __libcpp_condvar_wait(void **c, void **m)
{
    M95CV *p = cvget(c);
    EnterCriticalSection(&p->cs);
    p->waiters++;
    LeaveCriticalSection(&p->cs);
    __libcpp_mutex_unlock(m);
    WaitForSingleObject(p->sem, INFINITE);
    EnterCriticalSection(&p->cs);
    p->waiters--;
    LeaveCriticalSection(&p->cs);
    __libcpp_mutex_lock(m);
    return 0;
}
int __libcpp_condvar_timedwait(void **c, void **m, timespec *ts)
{
    M95CV *p = cvget(c);
    DWORD ms = 1000;
    if (ts)
        ms = (DWORD)(ts->tv_sec * 1000 + ts->tv_nsec / 1000000);
    EnterCriticalSection(&p->cs);
    p->waiters++;
    LeaveCriticalSection(&p->cs);
    __libcpp_mutex_unlock(m);
    DWORD r = WaitForSingleObject(p->sem, ms);
    EnterCriticalSection(&p->cs);
    p->waiters--;
    LeaveCriticalSection(&p->cs);
    __libcpp_mutex_lock(m);
    return r == WAIT_TIMEOUT ? 110 /* ETIMEDOUT */ : 0;
}
int __libcpp_condvar_destroy(void **c)
{
    M95CV *p = (M95CV *)*c;
    if (p) {
        CloseHandle(p->sem);
        DeleteCriticalSection(&p->cs);
        HeapFree(GetProcessHeap(), 0, p);
        *c = 0;
    }
    return 0;
}

int __libcpp_recursive_mutex_init(void *(*p)[6])
{
    InitializeCriticalSection((CRITICAL_SECTION *)p);
    return 0;
}
int __libcpp_recursive_mutex_lock(void *(*p)[6])
{
    EnterCriticalSection((CRITICAL_SECTION *)p);
    return 0;
}
int __libcpp_recursive_mutex_unlock(void *(*p)[6])
{
    LeaveCriticalSection((CRITICAL_SECTION *)p);
    return 0;
}
bool __libcpp_recursive_mutex_trylock(void *(*p)[6])
{
    return TryEnterCriticalSection((CRITICAL_SECTION *)p) != 0;
}
int __libcpp_recursive_mutex_destroy(void *(*p)[6])
{
    DeleteCriticalSection((CRITICAL_SECTION *)p);
    return 0;
}

int __libcpp_execute_once(void **flag, void (*fn)())
{
    boot();
    EnterCriticalSection(&g_boot);
    if (!*flag) {
        fn();
        *flag = (void *)1;
    }
    LeaveCriticalSection(&g_boot);
    return 0;
}

struct Thunk
{
    void *(*fn)(void *);
    void *arg;
};
static DWORD WINAPI thunk(LPVOID p)
{
    Thunk t = *(Thunk *)p;
    HeapFree(GetProcessHeap(), 0, p);
    t.fn(t.arg);
    return 0;
}
int __libcpp_thread_create(void **t, void *(*fn)(void *), void *arg)
{
    Thunk *th = (Thunk *)HeapAlloc(GetProcessHeap(), 0, sizeof(Thunk));
    th->fn = fn;
    th->arg = arg;
    HANDLE h = CreateThread(NULL, 0x100000, thunk, th, 0, NULL);
    *t = h;
    return h ? 0 : 11;
}
int __libcpp_thread_join(void **t)
{
    HANDLE h = (HANDLE)*t;
    if (h) {
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
        *t = 0;
    }
    return 0;
}
int __libcpp_thread_detach(void **t)
{
    HANDLE h = (HANDLE)*t;
    if (h)
        CloseHandle(h);
    *t = 0;
    return 0;
}
bool __libcpp_thread_isnull(void *const *t) { return *t == 0; }
long __libcpp_thread_get_id(void *const *t) { return (long)(DWORD_PTR)*t; }
long __libcpp_thread_get_current_id() { return (long)GetCurrentThreadId(); }
bool __libcpp_thread_id_equal(long a, long b) { return a == b; }
bool __libcpp_thread_id_less(long a, long b) { return a < b; }
void __libcpp_thread_yield() { Sleep(0); }
void __libcpp_thread_sleep_for(const chrono::nanoseconds &ns)
{
    Sleep((DWORD)(ns.count() / 1000000));
}

int __libcpp_tls_create(long *key, unsigned long(__stdcall *dtor)(void *))
{
    DWORD k = TlsAlloc();
    *key = (long)k;
    (void)dtor;
    return k == 0xFFFFFFFF ? 11 : 0;
}
void *__libcpp_tls_get(long key) { return TlsGetValue((DWORD)key); }
int __libcpp_tls_set(long key, void *v)
{
    TlsSetValue((DWORD)key, v);
    return 0;
}

} // namespace __1
} // namespace std
