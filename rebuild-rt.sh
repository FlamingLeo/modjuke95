#!/bin/sh
set -e

# Rebuild the safe static runtime stack for modjuke95.
#
# llvm-mingw ships libc++/libc++abi/libunwind/mingw-CRT/compiler-rt
# precompiled with SSE enabled and pulling Vista+ CRT exports. Real Win95
# machines and CPU-accurate VMs fault on the first SSE instruction.
# This script rebuilds every static runtime input with -march=i486 using
# the llvm-mingw toolchain binaries, for CRTDLL.DLL (part of every Windows
# 95 install). Output: lib-rt/, with the CRT headers in lib-rt/include/
# that libopenmpt and the app compile against.
#
# RT_WORK=dir picks the work folder (default /tmp/w95build). Downloaded
# sources are kept there between runs; build trees are redone each time.
cd "$(dirname "$0")"
ROOT="$PWD"   # the build steps below cd into the source trees
LM="$ROOT/llvm-mingw"
export PATH="$LM/bin:$PATH"
R="$LM/lib/clang/23/include"
W="${RT_WORK:-/tmp/w95build}"
# mingw-w64 v14: emulates many of the functions CRTDLL lacks (fgetpos,
# fsetpos, _filelengthi64, __getmainargs, iswctype, ...); v12 didn't
MW_VER=14.0.0
MW_SHA256=d71cc644cd5a37c337f2719f3e0c79d89e8d8d5fb9e2952a62d3fa23623dc137
OUT="$ROOT/lib-rt"
LLVM_URL=https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.2/llvm-project-23.1.2.src.tar.xz
LLVM_SHA256=c98bbef08a2b4c2613cd50e9aa9ae7b69b1fe6c16b2c40373bc0ab6116fdf78a
[ -x "$LM/bin/i686-w64-mingw32-clang" ] || { echo "toolchain missing - run ./rebuild.sh"; exit 1; }

fetch() {  # url sha256 dest
  curl -fL --retry 3 -o "$3" "$1"
  echo "$2  $3" | sha256sum -c - >/dev/null || { echo "checksum mismatch: $1"; rm -f "$3"; exit 1; }
}

# sources (kept between runs)
mkdir -p "$W"
[ -d "$W/rt-src" ] || {
  fetch "$LLVM_URL" "$LLVM_SHA256" "$W/llvm-project.tar.xz"
  mkdir -p "$W/rt-src.tmp"
  tar xf "$W/llvm-project.tar.xz" -C "$W/rt-src.tmp" --strip-components=1 \
    llvm-project-23.1.2.src/libcxx llvm-project-23.1.2.src/libcxxabi \
    llvm-project-23.1.2.src/libunwind llvm-project-23.1.2.src/compiler-rt/lib/builtins \
    llvm-project-23.1.2.src/libc/shared llvm-project-23.1.2.src/libc/src/__support \
    llvm-project-23.1.2.src/libc/hdr llvm-project-23.1.2.src/libc/include
  mv "$W/rt-src.tmp" "$W/rt-src"
  rm "$W/llvm-project.tar.xz"
}
MW="$W/mingw-w64-$MW_VER"
[ -d "$MW" ] || {
  fetch "https://github.com/mingw-w64/mingw-w64/archive/refs/tags/v$MW_VER.tar.gz" \
    "$MW_SHA256" "$W/mingw64.tar.gz"
  tar xzf "$W/mingw64.tar.gz" -C "$W"
  rm "$W/mingw64.tar.gz"
}

# fresh build area
B="$W/build"           # install prefix (headers, CRT libs) + object dirs
S="$B/stage"           # the files that end up in $OUT
rm -rf "$B" "$MW/mingw-w64-headers/b" "$MW/mingw-w64-crt/b"
mkdir -p "$S"

# generated libc++ config headers
cp "$W/rt-src/libcxx/include/__config_site.in" "$W/rt-src/libcxx/include/__config_site"
python3 - "$W/rt-src/libcxx/include/__config_site" <<'PYEOF'
import sys, re
p = sys.argv[1]
s = open(p).read()
repl = {
 '#cmakedefine _LIBCPP_ABI_VERSION @_LIBCPP_ABI_VERSION@': '#define _LIBCPP_ABI_VERSION 1',
 '#cmakedefine _LIBCPP_ABI_NAMESPACE @_LIBCPP_ABI_NAMESPACE@': '#define _LIBCPP_ABI_NAMESPACE __1',
 '#cmakedefine01 _LIBCPP_ABI_FORCE_ITANIUM': '#define _LIBCPP_ABI_FORCE_ITANIUM 0',
 '#cmakedefine01 _LIBCPP_ABI_FORCE_MICROSOFT': '#define _LIBCPP_ABI_FORCE_MICROSOFT 0',
 '#cmakedefine01 _LIBCPP_HAS_THREADS': '#define _LIBCPP_HAS_THREADS 1',
 '#cmakedefine01 _LIBCPP_HAS_MONOTONIC_CLOCK': '#define _LIBCPP_HAS_MONOTONIC_CLOCK 1',
 '#cmakedefine01 _LIBCPP_HAS_MUSL_LIBC': '#define _LIBCPP_HAS_MUSL_LIBC 0',
 '#cmakedefine01 _LIBCPP_HAS_THREAD_API_PTHREAD': '#define _LIBCPP_HAS_THREAD_API_PTHREAD 0',
 '#cmakedefine01 _LIBCPP_HAS_THREAD_API_EXTERNAL': '#define _LIBCPP_HAS_THREAD_API_EXTERNAL 0',
 '#cmakedefine01 _LIBCPP_HAS_THREAD_API_WIN32': '#define _LIBCPP_HAS_THREAD_API_WIN32 1',
 '#cmakedefine01 _LIBCPP_HAS_THREAD_API_C11': '#define _LIBCPP_HAS_THREAD_API_C11 0',
 '#cmakedefine _LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS': '#define _LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS',
 '#cmakedefine01 _LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS': '#define _LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS 0',
 '#cmakedefine01 _LIBCPP_HAS_FILESYSTEM': '#define _LIBCPP_HAS_FILESYSTEM 1',
 '#cmakedefine01 _LIBCPP_HAS_RANDOM_DEVICE': '#define _LIBCPP_HAS_RANDOM_DEVICE 1',
 '#cmakedefine01 _LIBCPP_HAS_LOCALIZATION': '#define _LIBCPP_HAS_LOCALIZATION 1',
 '#cmakedefine01 _LIBCPP_HAS_UNICODE': '#define _LIBCPP_HAS_UNICODE 1',
 '#cmakedefine01 _LIBCPP_HAS_WIDE_CHARACTERS': '#define _LIBCPP_HAS_WIDE_CHARACTERS 1',
 '#cmakedefine01 _LIBCPP_HAS_TIME_ZONE_DATABASE': '#define _LIBCPP_HAS_TIME_ZONE_DATABASE 0',
 '#cmakedefine01 _LIBCPP_INSTRUMENTED_WITH_ASAN': '#define _LIBCPP_INSTRUMENTED_WITH_ASAN 0',
 '#cmakedefine _LIBCPP_PSTL_BACKEND_SERIAL': '#define _LIBCPP_PSTL_BACKEND_SERIAL',
 '#cmakedefine _LIBCPP_HARDENING_MODE_DEFAULT @_LIBCPP_HARDENING_MODE_DEFAULT@': '#define _LIBCPP_HARDENING_MODE_DEFAULT _LIBCPP_HARDENING_MODE_FAST',
 '#cmakedefine _LIBCPP_ASSERTION_SEMANTIC_DEFAULT @_LIBCPP_ASSERTION_SEMANTIC_DEFAULT@': '#define _LIBCPP_ASSERTION_SEMANTIC_DEFAULT _LIBCPP_ASSERTION_SEMANTIC_QUICK_ENFORCE',
 '#cmakedefine01 _LIBCPP_LIBC_PICOLIBC': '#define _LIBCPP_LIBC_PICOLIBC 0',
 '#cmakedefine01 _LIBCPP_LIBC_NEWLIB': '#define _LIBCPP_LIBC_NEWLIB 0',
 '#cmakedefine01 _LIBCPP_LIBC_LLVM_LIBC': '#define _LIBCPP_LIBC_LLVM_LIBC 0',
}
for k, v in repl.items():
    s = s.replace(k, v)
s = re.sub(r'^#cmakedefine.*$', '', s, flags=re.M)
s = re.sub(r'^@[A-Z_]+@$', '', s, flags=re.M)   # CMake-only placeholder lines
open(p, 'w').write(s)
PYEOF
cp "$W/rt-src/libcxx/vendor/llvm/default_assertion_handler.in" \
   "$W/rt-src/libcxx/include/__assertion_handler"

A486="-O2 -march=i486 -mno-sse -mno-sse2 -mno-mmx"
# only the clang builtins + the mingw headers built here; the toolchain's
# own (newer) mingw headers must not mix in via #include_next
NOSTD="-nostdinc -isystem $R -isystem $B/include"

# mingw-w64 headers
cd "$MW/mingw-w64-headers" && mkdir -p b && cd b
../configure --host=i686-w64-mingw32 --prefix="$B" --with-default-msvcrt=crtdll
make install

# mingw-w64 CRT (i486)
cd "$MW/mingw-w64-crt" && mkdir -p b && cd b
../configure --host=i686-w64-mingw32 --prefix="$B" \
  --enable-lib32 --disable-lib64 --disable-libarm32 \
  --disable-libarm64 --with-default-msvcrt=crtdll \
  CC="i686-w64-mingw32-clang" CXX="i686-w64-mingw32-clang++" \
  CFLAGS="$NOSTD $A486 -Wno-implicit-function-declaration" \
  CXXFLAGS="$NOSTD $A486 -Wno-implicit-function-declaration"
make -j"${JOBS:-2}"
cp lib32/libmingw32.a lib32/libmingwex.a lib32/libcrtdll.a lib32/crt2.o "$S/"

# compiler-rt builtins
cd "$W/rt-src/compiler-rt/lib/builtins"
mkdir -p "$B/built"
for f in *.c i386/*.S; do
  case "$f" in
    apple_versioning.c|crtbegin.c|*bf2.c) continue ;;
    i386/float*.S) continue ;;
  esac
  i686-w64-mingw32-clang $A486 -std=c11 -ffreestanding -fno-builtin -fno-lto \
    -fno-exceptions -I. -c "$f" -o "$B/built/$(basename "$f" | sed 's/\.[cS]$/.o/')"
done
i686-w64-mingw32-ar rcs "$S/libclang_rt.builtins-i386.a" "$B/built"/*.o

# libunwind
cd "$W/rt-src/libunwind"
UFL="$NOSTD -I include -D_LIBUNWIND_DISABLE_VISIBILITY_ANNOTATIONS -fno-exceptions -fno-rtti -funwind-tables"
mkdir -p "$B/lu"
i686-w64-mingw32-clang++ $UFL $A486 -std=c++11 -c src/libunwind.cpp -o "$B/lu/libunwind.o"
i686-w64-mingw32-clang++ $UFL $A486 -std=c++11 -c src/Unwind-seh.cpp -o "$B/lu/Unwind-seh.o"
for f in UnwindLevel1 UnwindLevel1-gcc-ext Unwind-sjlj; do
  i686-w64-mingw32-clang $UFL $A486 -std=c11 -c src/$f.c -o "$B/lu/$f.o"
done
i686-w64-mingw32-clang $UFL $A486 -c src/UnwindRegistersRestore.S -o "$B/lu/regs_restore.o"
i686-w64-mingw32-clang $UFL $A486 -c src/UnwindRegistersSave.S -o "$B/lu/regs_save.o"
i686-w64-mingw32-ar rcs "$S/libunwind.a" "$B/lu"/*.o

# libc++abi
cd "$W/rt-src/libcxxabi"
# cxa_thread_atexit's TLS callback must be stdcall on i386
grep -q stdcall src/cxa_thread_atexit.cpp || sed -i \
  's/^  void run_dtors(void\*) {$/#if defined(_WIN32) \&\& defined(__i386__)\n  void __attribute__((stdcall)) run_dtors(void*) {\n#else\n  void run_dtors(void*) {\n#endif/' \
  src/cxa_thread_atexit.cpp
ABIFL="$NOSTD -I $W/rt-src/libcxx/include -I $W/rt-src/libcxx/src -I include -I src \
  -std=gnu++20 -D_LIBCPP_BUILDING_LIBRARY -D_LIBCXXABI_DISABLE_VISIBILITY_ANNOTATIONS"
mkdir -p "$B/abi"
for f in $(ls src/*.cpp | grep -v cxa_noexception); do
  i686-w64-mingw32-clang++ $ABIFL $A486 -c "$f" -o "$B/abi/$(basename "$f" .cpp).o"
done
i686-w64-mingw32-ar rcs "$S/libc++abi.a" "$B/abi"/*.o

# libc++ (src/w95extra.h declares the locale variants the CRT headers lack;
# the app's w95imports.cpp implements them)
cd "$W/rt-src/libcxx"
CXXFL="$NOSTD -include $ROOT/src/w95extra.h -I include -I src -I $W/rt-src/libcxxabi/include \
  -I $W/rt-src/libc -std=gnu++26 -fno-rtti -D_LIBCPP_BUILDING_LIBRARY \
  -DLIBCXX_BUILDING_LIBCXXABI -D_LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS"
mkdir -p "$B/cxx"
find src -name '*.cpp' | grep -v experimental | grep -v libdispatch.cpp | grep -v xlocale_zos.cpp |
  grep -v /support/ibm/ |
while read f; do
  i686-w64-mingw32-clang++ $CXXFL $A486 -c "$f" -o "$B/cxx/$(basename "$f" .cpp).o"
done
i686-w64-mingw32-ar rcs "$S/libc++.a" "$B/cxx"/*.o

# zero SSE anywhere
for a in "$S"/*.a; do
  n=$(i686-w64-mingw32-objdump -d "$a" | grep -c "%xmm\|%ymm\|%zmm" || true)
  [ "$n" -eq 0 ] || { echo "SSE in $a"; exit 1; }
done

rm -rf "$OUT"
mkdir -p "$OUT"
cp "$S"/* "$OUT/"
# the app and libopenmpt compile against the same CRT headers
cp -r "$B/include" "$OUT/include"
echo "OK: runtime stack in lib-rt/ (SSE-free)"
