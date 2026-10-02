#!/bin/sh
set -e

# Build modjuke95.exe against the Win95 runtime stack, in two editions:
#   build/modjuke95.exe          full: every format libopenmpt plays
#   build-common/modjuke95.exe   common formats only
#   sh build.sh                  both
#   sh build.sh full|common      one of them
#
# Needs the llvm-mingw toolchain, the rebuilt runtime libraries and libopenmpt,
# in this order (the runtime also provides the CRT headers):
#   ./rebuild.sh
#   ./rebuild-rt.sh
#   ./rebuild.sh --libopenmpt
# The exe uses CRTDLL.DLL, the C runtime that ships with every Windows 95
# install.

cd "$(dirname "$0")"
ROOT="$PWD"
case "${1:-both}" in
  both) EDITIONS="full common" ;;
  full|common) EDITIONS="$1" ;;
  *) echo "usage: $0 [full|common]"; exit 1 ;;
esac
TOOLCHAIN="$ROOT/llvm-mingw/bin"
RT="$ROOT/lib-rt"
OBJ=build   # objects shared by both editions
# only libc++, the clang builtins and the runtime's CRTDLL-mode mingw hdrs
SYSINC="-nostdinc -isystem $ROOT/llvm-mingw/i686-w64-mingw32/include/c++/v1 \
 -isystem $ROOT/llvm-mingw/lib/clang/23/include -isystem $RT/include"
[ -x "$TOOLCHAIN/i686-w64-mingw32-clang++" ] || { echo "toolchain missing - run ./rebuild.sh"; exit 1; }
[ -f "$ROOT/lib/libopenmpt.a" ] && [ -f "$ROOT/lib/libopenmpt-common.a" ] &&
  [ -f "$ROOT/include/libopenmpt/libopenmpt.h" ] ||
  { echo "libopenmpt missing - run ./rebuild.sh --libopenmpt"; exit 1; }
[ -f "$RT/crt2.o" ] && [ -f "$RT/include/_mingw.h" ] ||
  { echo "runtime libs missing - run ./rebuild-rt.sh"; exit 1; }
export PATH="$TOOLCHAIN:$PATH"
mkdir -p "$OBJ"
cd "$OBJ"

CXX=i686-w64-mingw32-clang++
WRC=i686-w64-mingw32-windres
FLAGS="$SYSINC -std=gnu++20 -march=i486 -DWINVER=0x0400 -D_WIN32_WINDOWS=0x0400 \
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

for ed in $EDITIONS; do
  if [ "$ed" = full ]; then
    OUT="$ROOT/build"; MPT="$ROOT/lib/libopenmpt.a"; EDEF=""
  else
    OUT="$ROOT/build-common"; MPT="$ROOT/lib/libopenmpt-common.a"; EDEF="-DM95_EDITION_COMMON"
  fi
  mkdir -p "$OUT"
  echo "[CXX] edition.cpp ($ed)"
  $CXX $FLAGS -Os $EDEF -c "$ROOT/src/edition.cpp" -o "edition-$ed.o"

  # our rebuilt CRT/C++ stack from lib-rt (including the CRTDLL import
  # library) shadows the toolchain's SSE-enabled libraries
  #
  # -s: no COFF symbol table in the exe
  # crash addresses are mapped with modjuke95.map (next to each exe)
  $CXX -mwindows -nodefaultlibs -nostartfiles -s -o "$OUT/modjuke95.exe" \
    "$RT/crt2.o" \
    common.o mptwrap.o model.o engine.o ui.o w95compat.o w95imports.o \
    "edition-$ed.o" app.res \
    -L"$RT" \
    "$MPT" \
    -lwinmm -lcomctl32 -lcomdlg32 -lshell32 -lole32 -luser32 -lgdi32 \
    -lmingw32 -lmingwex -lcrtdll -lc++ -lc++abi -lunwind \
    -lclang_rt.builtins-i386 -lcrtdll -lmingw32 -lkernel32 -ladvapi32 \
    -Wl,--gc-sections -Wl,-Map="$OUT/modjuke95.map" \
    -Wl,--major-os-version,4 -Wl,--minor-os-version,0 \
    -Wl,--major-subsystem-version,4 -Wl,--minor-subsystem-version,0
  echo "[OK] ${OUT#$ROOT/}/modjuke95.exe ($ed edition, $(wc -c < "$OUT/modjuke95.exe") bytes)"

  # --- Win95 native-compatibility gate ---
  python3 -B "$ROOT/scripts/check95.py" "$OUT/modjuke95.exe"
  echo "[OK] import + PE header audit passed"
done

# --- SSE gate --------
for o in common.o mptwrap.o model.o engine.o ui.o w95compat.o w95imports.o \
         edition-*.o "$RT"/*.a "$ROOT"/lib/libopenmpt*.a; do
  # % prefix: objdump AT&T register ref
  n=$(i686-w64-mingw32-objdump -d "$o" 2>/dev/null | grep -c "%xmm\|%ymm\|%zmm" || true)
  [ "$n" -eq 0 ] || { echo "FAIL: SSE in $o"; exit 1; }
done
echo "[OK] SSE audit passed (0 SSE instructions)"
