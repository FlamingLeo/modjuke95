#!/bin/sh
set -e

# Build build/modjuke95.exe against the Win95 runtime stack.
#
# Needs the llvm-mingw toolchain and libopenmpt.a plus the rebuilt runtime libraries:
#   ./rebuild.sh --libopenmpt
#   ./rebuild-rt.sh
cd "$(dirname "$0")"
ROOT="$PWD"
TOOLCHAIN="$ROOT/llvm-mingw/bin"
RT="$ROOT/lib-rt"
MPT="$ROOT/lib/libopenmpt.a"
[ -x "$TOOLCHAIN/i686-w64-mingw32-clang++" ] || { echo "toolchain missing - run ./rebuild.sh"; exit 1; }
[ -f "$MPT" ] && [ -f "$ROOT/include/libopenmpt/libopenmpt.h" ] ||
  { echo "libopenmpt missing - run ./rebuild.sh --libopenmpt"; exit 1; }
[ -f "$RT/crt2.o" ] || { echo "runtime libs missing - run ./rebuild-rt.sh"; exit 1; }
export PATH="$TOOLCHAIN:$PATH"
mkdir -p build
cd build

CXX=i686-w64-mingw32-clang++
WRC=i686-w64-mingw32-windres
FLAGS="-std=gnu++20 -march=i486 -DWINVER=0x0400 -D_WIN32_WINDOWS=0x0400 \
 -D_WIN32_WINNT=0x0400 -ffunction-sections -fdata-sections -fno-threadsafe-statics \
 -include $ROOT/src/w95extra.h -I $ROOT/include -I $ROOT/src"

$WRC -I "$ROOT/src" -I "$ROOT/resources" "$ROOT/src/app.rc" -O coff -o app.res
for f in common mptwrap model engine ui w95compat w95imports; do
  # -O2 only where playback runs (audio thread + libopenmpt wrapper); the
  # UI/model/shim code isn't hot and is 20-30% smaller at -Os
  case $f in engine|mptwrap) OPT=-O2 ;; *) OPT=-Os ;; esac
  echo "[CXX] $f.cpp ($OPT)"
  $CXX $FLAGS $OPT -c "$ROOT/src/$f.cpp" -o $f.o
done

# our rebuilt CRT/C++ stack from lib-rt shadows the toolchain's SSE-enabled libraries
# msvcrt import thunks still come from the toolchain
# -s: no COFF symbol table in the exe
# crash addresses are mapped with modjuke95.map
$CXX -mwindows -nodefaultlibs -nostartfiles -s -o modjuke95.exe \
  "$RT/crt2.o" \
  common.o mptwrap.o model.o engine.o ui.o w95compat.o w95imports.o app.res \
  -L"$RT" \
  "$MPT" \
  -lwinmm -lcomctl32 -lcomdlg32 -lshell32 -lole32 -luser32 -lgdi32 \
  -lmingw32 -lmingwex -lmsvcrt-os -lc++ -lc++abi -lunwind \
  -lclang_rt.builtins-i386 -lmsvcrt -lmingw32 -lkernel32 -ladvapi32 \
  -Wl,--gc-sections -Wl,-Map=modjuke95.map \
  -Wl,--major-os-version,4 -Wl,--minor-os-version,0 \
  -Wl,--major-subsystem-version,4 -Wl,--minor-subsystem-version,0
echo "[OK] build/modjuke95.exe"

# --- Win95 native-compatibility gate ---
python3 -B "$ROOT/scripts/check95.py" modjuke95.exe
echo "[OK] import + PE header audit passed"

# --- SSE gate --------
for o in common.o mptwrap.o model.o engine.o ui.o w95compat.o w95imports.o \
         "$RT"/*.a "$MPT"; do
  # % prefix: objdump AT&T register ref
  n=$(i686-w64-mingw32-objdump -d "$o" 2>/dev/null | grep -c "%xmm\|%ymm\|%zmm" || true)
  [ "$n" -eq 0 ] || { echo "FAIL: SSE in $o"; exit 1; }
done
echo "[OK] SSE audit passed (0 SSE instructions)"
