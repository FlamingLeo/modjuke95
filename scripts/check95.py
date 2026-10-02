#!/usr/bin/env python3
"""Win95 native-compatibility gate for modjuke95.exe.

Fails the build when the PE imports:
  * a DLL that does not ship with Windows 95,
  * a Unicode (…W) Win32 API,
  * a known post-Win95 kernel32 function,
  * MSVCRT.DLL (not on every Win95 install; the C runtime is CRTDLL.DLL),
  * a CRTDLL function the Windows 95 CRTDLL.DLL doesn't export 
    (reference list: crtdll95.txt), and when the PE header is not 
    a Win95-loadable i386 GUI image.

Usage: check95.py file.exe
"""
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pe_imports

ALLOWED_DLLS = {
    'KERNEL32.dll', 'USER32.dll', 'GDI32.dll', 'WINMM.dll', 'WINMM.DLL',
    'COMCTL32.dll', 'COMDLG32.dll', 'SHELL32.dll', 'OLE32.dll', 'ADVAPI32.dll',
    'crtdll.dll',
}

DENY_FUNCS = {
    'IsDebuggerPresent', 'InitializeCriticalSectionAndSpinCount',
    'InitializeCriticalSectionEx', 'EncodePointer', 'DecodePointer',
    'HeapSetInformation', 'SetDllDirectoryA', 'GetModuleHandleExA',
    'GetLongPathNameA', 'InterlockedCompareExchange64', 'GetNativeSystemInfo',
    'SetThreadStackGuarantee', 'FlsAlloc', 'CreateSymbolicLinkA',
    'SleepConditionVariableCS', 'InitializeSRWLock', 'AcquireSRWLockExclusive',
    'CreateSemaphoreW', 'GetModuleFileNameW', 'SHGetFolderPathW',
    'K32EnumProcessModules', 'K32GetModuleInformation',
    'GetLocaleInfoEx', 'GetSystemTimePreciseAsFileTime',
}

# kernel32 functions proven present on Win95
KNOWN_KERNEL32 = {
    'CloseHandle', 'CreateSemaphoreA', 'DeleteCriticalSection', 'EnterCriticalSection',
    'ExitProcess', 'FindClose', 'FindFirstFileA', 'FindNextFileA', 'FreeLibrary',
    'GetACP', 'GetCPInfo', 'GetCommandLineA', 'GetConsoleMode',
    'GetConsoleScreenBufferInfo', 'GetCurrentThread', 'GetCurrentThreadId',
    'GetFullPathNameA',
    'GetLastError', 'GetModuleHandleA', 'GetProcAddress', 'GetStdHandle',
    'GetSystemTimeAsFileTime', 'InitializeCriticalSection', 'InterlockedDecrement',
    'InterlockedExchange', 'InterlockedIncrement', 'LeaveCriticalSection',
    'LoadLibraryA', 'MultiByteToWideChar', 'ReleaseSemaphore',
    'SetConsoleCursorPosition', 'SetLastError', 'SetUnhandledExceptionFilter',
    'Sleep', 'TlsAlloc', 'TlsFree', 'TlsGetValue', 'TlsSetValue', 'VirtualProtect',
    'VirtualQuery', 'WaitForMultipleObjects', 'WaitForSingleObject',
    'WideCharToMultiByte', 'WriteConsoleA',
    'GetStartupInfoA', 'IsDBCSLeadByte', 'GetCurrentProcess',
    # common Win95-era additions used by this app / CRT:
    'CreateThread', 'CreateEventA', 'SetEvent', 'ResetEvent', 'GetTickCount',
    'GetModuleFileNameA', 'GetCurrentProcessId', 'GetSystemDirectoryA',
    'GetWindowsDirectoryA', 'GetTempPathA', 'GetFileSize', 'SetFilePointer',
    'CreateFileA', 'ReadFile', 'WriteFile', 'GetFileType', 'FlushFileBuffers',
    'SetEndOfFile', 'GetLocalTime', 'SystemTimeToFileTime', 'FileTimeToLocalFileTime',
    'FileTimeToSystemTime', 'LocalFileTimeToFileTime', 'GetDateFormatA',
    'GetTimeFormatA', 'GetOEMCP', 'LCMapStringA', 'GetStringTypeA', 'HeapAlloc',
    'HeapCreate', 'HeapDestroy', 'HeapFree', 'HeapReAlloc', 'HeapSize',
    'VirtualAlloc', 'VirtualFree', 'TerminateThread', 'GetExitCodeThread',
    'SetPriorityClass', 'GetPriorityClass', 'SetThreadPriority',
    'GetThreadPriority', 'QueryPerformanceCounter', 'QueryPerformanceFrequency',
    'GetEnvironmentStrings', 'FreeEnvironmentStringsA', 'GetEnvironmentVariableA',
    'SetEnvironmentVariableA', 'GetCommandLineA', 'GetCurrentDirectoryA',
    'SetCurrentDirectoryA', 'GetDriveTypeA', 'GetDiskFreeSpaceA',
    'GetVolumeInformationA', 'RemoveDirectoryA', 'CreateDirectoryA', 'DeleteFileA',
    'CopyFileA', 'MoveFileA', 'GetFileAttributesA', 'SetFileAttributesA',
    'GetFileTime', 'SetFileTime', 'CompareStringA', 'FoldStringA', 'MultiByteToWideChar',
    'ExitThread', 'SuspendThread', 'ResumeThread', 'GetSystemInfo',
    'GlobalMemoryStatus',
    'GlobalAlloc', 'GlobalFree', 'GlobalLock', 'GlobalUnlock', 'GlobalSize',
    'LocalAlloc', 'LocalFree', 'lstrlenA', 'lstrcmpiA', 'OutputDebugStringA', 'GetPrivateProfileIntA',
    'GetPrivateProfileStringA', 'WritePrivateProfileStringA',
    'UnhandledExceptionFilter', 'SetUnhandledExceptionFilter', 'RaiseException',
    'GetProcessHeap', 'HeapAlloc', 'HeapFree', 'HeapReAlloc', 'GetACP',
    'AreFileApisANSI', 'FormatMessageA', 'IsDBCSLeadByteEx', 'GetLocaleInfoA',
    # single-instance mutex, exit when a thread hangs
    'CreateMutexA', 'TerminateProcess',
}

def header_fails(path):
    # windows 95 refuses images whose subsystem version is >4.0
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    if d[pe:pe + 4] != b'PE\0\0':
        return ['not a PE image']
    opt = pe + 24
    machine = struct.unpack_from('<H', d, pe + 4)[0]
    magic = struct.unpack_from('<H', d, opt)[0]
    osv = struct.unpack_from('<2H', d, opt + 40)
    ssv = struct.unpack_from('<2H', d, opt + 48)
    subsys = struct.unpack_from('<H', d, opt + 68)[0]
    fails = []
    if machine != 0x14c or magic != 0x10b:
        fails.append('not a PE32 i386 image (machine %#x, magic %#x)' % (machine, magic))
    if osv > (4, 0):
        fails.append('OS version %d.%d > 4.0' % osv)
    if ssv > (4, 0):
        fails.append('subsystem version %d.%d > 4.0' % ssv)
    if subsys != 2:
        fails.append('subsystem %d is not GUI (2)' % subsys)
    return fails

def crtdll95_exports():
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'crtdll95.txt')
    with open(p) as f:
        return {l.strip() for l in f if l.strip() and not l.startswith('#')}

def main(path):
    imp = pe_imports.parse(path)
    fails, warns = header_fails(path), []
    norm = {k.upper(): v for k, v in imp.items()}
    if 'CRTDLL.DLL' not in norm:
        fails.append('no CRTDLL.DLL import (C runtime missing?)')
    exports = crtdll95_exports()
    for fn in norm.get('CRTDLL.DLL', []):
        if fn not in exports:
            fails.append('CRTDLL.DLL!%s (not exported by the Win95 CRTDLL 3.50)' % fn)
    for dll in sorted(norm):
        if dll not in {a.upper() for a in ALLOWED_DLLS}:
            fails.append('DLL not on Win95: %s' % dll)
            continue
        for fn in norm[dll]:
            if fn.startswith('#ord'):
                continue
            if fn in DENY_FUNCS:
                fails.append('%s!%s (post-Win95 API)' % (dll, fn))
            if dll != 'CRTDLL.DLL' and fn.endswith('W') \
               and not fn.startswith('waveOut') and not fn.endswith('W2'):
                fails.append('%s!%s (Unicode API, not native on Win9x)' % (dll, fn))
            if dll == 'KERNEL32.DLL' and fn not in KNOWN_KERNEL32:
                warns.append('kernel32!%s (not in reference set - review)' % fn)
    for w in warns:
        print('WARN: ' + w)
    if fails:
        for f in fails:
            print('FAIL: ' + f)
        sys.exit(1)
    print('import audit: %d DLLs, 0 failures, %d warnings' % (len(norm), len(warns)))

if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit('usage: check95.py file.exe')
    main(sys.argv[1])
