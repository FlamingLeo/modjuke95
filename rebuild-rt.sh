#!/bin/sh
set -e

# Rebuild the safe static runtime stack for modjuke95.
#
# llvm-mingw ships libc++/libc++abi/libunwind/mingw-CRT/compiler-rt
# precompiled with SSE enabled and pulling Vista+ CRT exports. Real Win95
# machines and CPU-accurate VMs fault on the first SSE instruction. 
# This script rebuilds every static runtime input with -march=i486 using 
# the llvm-mingw toolchain binaries. Outputs land in lib-rt/.
cd "$(dirname "$0")"
ROOT="$PWD"   # the build steps below cd into the source trees
export PATH="$PWD/llvm-mingw/bin:$PATH"
R="$PWD/llvm-mingw/lib/clang/23/include"
W=/tmp/w95build
rm -rf "$W" && mkdir -p "$W/lib" "$W/include"

# sources
[ -d "$W/llsrc" ] || {
  git clone --depth 1 --filter=blob:none --sparse \
    https://github.com/llvm/llvm-project --branch llvmorg-23.1.2 "$W/llsrc"
  cd "$W/llsrc" && git sparse-checkout set libc/shared libc/src/__support libc/hdr libc/include \
    compiler-rt/lib/builtins
  cd "$PWD/../.."
}
[ -d "$W/rt-src" ] || {
  curl -sL -o "$W/llvm-project.tar.xz" \
    https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.2/llvm-project-23.1.2.src.tar.xz
  mkdir -p "$W/rt-src" && tar xf "$W/llvm-project.tar.xz" -C "$W/rt-src" --strip-components=1 \
    llvm-project-23.1.2.src/libcxx llvm-project-23.1.2.src/libcxxabi llvm-project-23.1.2.src/libunwind
  rm "$W/llvm-project.tar.xz"
}
[ -d "$W/mingw" ] || {
  curl -sL -o "$W/mingw64.tar.gz" https://github.com/mingw-w64/mingw-w64/archive/refs/tags/v12.0.0.tar.gz
  tar xzf "$W/mingw64.tar.gz" -C "$W" && mv "$W/mingw-w64-12.0.0" "$W/mingw"
}

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
open(p, 'w').write(s)
PYEOF
cp "$W/rt-src/libcxx/vendor/llvm/default_assertion_handler.in" \
   "$W/rt-src/libcxx/include/__assertion_handler"
   
# v12 mingw headers lack the libc++ windows locale support references
cat > "$W/w95extra.h" <<'EOF'
#pragma once
#include <wctype.h>
#include <wchar.h>
extern "C"
{
    int _iswctype_l(wint_t, wctype_t, void *);
    int _iswalpha_l(wint_t, void *);
    int _iswupper_l(wint_t, void *);
    int _iswlower_l(wint_t, void *);
    int _iswdigit_l(wint_t, void *);
    int _iswxdigit_l(wint_t, void *);
    int _iswspace_l(wint_t, void *);
    int _iswprint_l(wint_t, void *);
    int _iswpunct_l(wint_t, void *);
    int _iswcntrl_l(wint_t, void *);
    wint_t _towlower_l(wint_t, void *);
    wint_t _towupper_l(wint_t, void *);
    int rand_s(unsigned int *);
}
EOF

A486="-O2 -march=i486 -mno-sse -mno-sse2 -mno-mmx"
NOSTD="-nostdinc -isystem $R -isystem $W/include"

# mingw-w64 v12 headers
cd "$W/mingw/mingw-w64-headers" && mkdir -p b && cd b
../configure --host=i686-w64-mingw32 --prefix="$W" --with-default-msvcrt=msvcrt
make install

# mingw-w64 CRT (i486)
cd "$W/mingw/mingw-w64-crt" && mkdir -p b && cd b
../configure --host=i686-w64-mingw32 --prefix="$W" \
  --enable-lib32 --disable-lib64 --disable-libx32 --disable-libarm32 \
  --disable-libarm64 --with-default-msvcrt=msvcrt \
  CC="i686-w64-mingw32-clang" CXX="i686-w64-mingw32-clang++" \
  CFLAGS="-I$W/include $A486 -Wno-implicit-function-declaration" \
  CXXFLAGS="-I$W/include $A486 -Wno-implicit-function-declaration"
make -j2
cp lib32/libmingw32.a lib32/libmingwex.a lib32/libmsvcrt-os.a lib32/crt2.o "$W/lib/"

# compiler-rt builtins
cd "$W/llsrc/compiler-rt/lib/builtins"
mkdir -p "$W/built"

for f in *.c i386/*.S; do
  case "$f" in
    apple_versioning.c|crtbegin.c|*bf2.c) continue ;;
    i386/float*.S) continue ;;
  esac
  i686-w64-mingw32-clang $A486 -std=c11 -ffreestanding -fno-builtin -fno-lto \
    -fno-exceptions -I. -c "$f" -o "$W/built/$(basename "$f" | sed 's/\.[cS]$/.o/')"
done
i686-w64-mingw32-ar rcs "$W/lib/libclang_rt.builtins-i386.a" "$W/built"/*.o

# libunwind
cd "$W/rt-src/libunwind"
UFL="$NOSTD -I include -D_LIBUNWIND_DISABLE_VISIBILITY_ANNOTATIONS -fno-exceptions -fno-rtti -funwind-tables"
mkdir -p "$W/lu"
i686-w64-mingw32-clang++ $UFL $A486 -std=c++11 -c src/libunwind.cpp -o "$W/lu/libunwind.o"
i686-w64-mingw32-clang++ $UFL $A486 -std=c++11 -c src/Unwind-seh.cpp -o "$W/lu/Unwind-seh.o"
for f in UnwindLevel1 UnwindLevel1-gcc-ext Unwind-sjlj; do
  i686-w64-mingw32-clang $UFL $A486 -std=c11 -c src/$f.c -o "$W/lu/$f.o"
done
i686-w64-mingw32-clang $UFL $A486 -c src/UnwindRegistersRestore.S -o "$W/lu/regs_restore.o"
i686-w64-mingw32-clang $UFL $A486 -c src/UnwindRegistersSave.S -o "$W/lu/regs_save.o"
i686-w64-mingw32-ar rcs "$W/lib/libunwind.a" "$W/lu"/*.o

# libc++abi
cd "$W/rt-src/libcxxabi"
# cxa_thread_atexit's TLS callback must be stdcall on i386
grep -q stdcall src/cxa_thread_atexit.cpp || sed -i \
  's/^  void run_dtors(void\*) {$/#if defined(_WIN32) \&\& defined(__i386__)\n  void __attribute__((stdcall)) run_dtors(void*) {\n#else\n  void run_dtors(void*) {\n#endif/' \
  src/cxa_thread_atexit.cpp
ABIFL="$NOSTD -I $W/rt-src/libcxx/include -I $W/rt-src/libcxx/src -I include -I src \
  -std=gnu++20 -D_LIBCPP_BUILDING_LIBRARY -D_LIBCXXABI_DISABLE_VISIBILITY_ANNOTATIONS"
mkdir -p "$W/abi"
for f in $(ls src/*.cpp | grep -v cxa_noexception); do
  i686-w64-mingw32-clang++ $ABIFL $A486 -c "$f" -o "$W/abi/$(basename "$f" .cpp).o"
done
i686-w64-mingw32-ar rcs "$W/lib/libc++abi.a" "$W/abi"/*.o

# libc++
cd "$W/rt-src/libcxx"
CXXFL="$NOSTD -include $W/w95extra.h -I include -I src -I $W/rt-src/libcxxabi/include \
  -I $W/llsrc/libc -std=gnu++26 -fno-rtti -D_LIBCPP_BUILDING_LIBRARY \
  -DLIBCXX_BUILDING_LIBCXXABI -D_LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS"
mkdir -p "$W/cxx"
find src -name '*.cpp' | grep -v experimental | grep -v libdispatch.cpp | grep -v xlocale_zos.cpp |
while read f; do
  i686-w64-mingw32-clang++ $CXXFL $A486 -c "$f" -o "$W/cxx/$(basename "$f" .cpp).o"
done
i686-w64-mingw32-ar rcs "$W/lib/libc++.a" "$W/cxx"/*.o

# zero SSE anywhere
for a in "$W/lib"/*.a; do
  n=$(i686-w64-mingw32-objdump -d "$a" | grep -c "%xmm\|%ymm\|%zmm" || true)
  [ "$n" -eq 0 ] || { echo "SSE in $a"; exit 1; }
done

mkdir -p "$ROOT/lib-rt"
cp "$W/lib"/* "$ROOT/lib-rt/"
echo "OK: runtime stack in lib-rt/ (SSE-free)"
