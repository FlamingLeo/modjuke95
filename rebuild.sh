#!/bin/sh -e

# Recreate the modjuke95 build environment on Linux, x86_64.
#
#   ./rebuild.sh              fetch the llvm-mingw cross toolchain only
#   ./rebuild.sh --libopenmpt also build the Win95 libopenmpt from source, twice:
#                             lib/libopenmpt.a (all formats) and
#                             lib/libopenmpt-common.a (common formats only,
#                             see scripts/formats.py), and install its
#                             headers into include/ (needs the runtime
#                             headers from ./rebuild-rt.sh)

cd "$(dirname "$0")"

case "${1:-}" in
  "") MODE=toolchain ;;
  --libopenmpt) MODE=win95 ;;
  *) echo "usage: $0 [--libopenmpt]"; exit 1 ;;
esac

TOOLCHAIN_URL="https://github.com/mstorsjo/llvm-mingw/releases/download/20260922/llvm-mingw-20260922-msvcrt-ubuntu-22.04-x86_64.tar.xz"
TOOLCHAIN_SHA256=17bbc667b96f7b1f02ce4a172d8cac4fa480927a1f649e788dee5914919566a1
LIBOPENMPT_URL="https://lib.openmpt.org/files/libopenmpt/src/libopenmpt-0.8.9+release.makefile.tar.gz"
LIBOPENMPT_SHA256=9273b88b67973cc69e54d748ab1b749399d6d07695f1c37d0c59f88b4106074f

DL=$(mktemp -d)
trap 'rm -rf "$DL"' EXIT
fetch() {  # url sha256 dest
  curl -fL --retry 3 -o "$3" "$1"
  echo "$2  $3" | sha256sum -c - >/dev/null || { echo "checksum mismatch: $1"; exit 1; }
}
unpack_libopenmpt() {  # fresh source tree in $1
  rm -rf "$1"
  mkdir "$1"
  fetch "$LIBOPENMPT_URL" "$LIBOPENMPT_SHA256" "$DL/libopenmpt-src.tar.gz"
  tar xzf "$DL/libopenmpt-src.tar.gz" -C "$1" --strip-components=1
}

# cross toolchain
if [ ! -x llvm-mingw/bin/i686-w64-mingw32-clang++ ]; then
  fetch "$TOOLCHAIN_URL" "$TOOLCHAIN_SHA256" "$DL/llvmmingw.tar.xz"
  tar xf "$DL/llvmmingw.tar.xz" -C "$DL"
  rm -rf llvm-mingw
  mv "$DL/llvm-mingw-20260922-msvcrt-ubuntu-22.04-x86_64" llvm-mingw
fi
echo OK: llvm-mingw/
[ "$MODE" = win95 ] || exit 0
export PATH="$PWD/llvm-mingw/bin:$PATH"

# compile against the CRTDLL-mode mingw headers of the runtime only
INC="$PWD/lib-rt/include"
[ -f "$INC/_mingw.h" ] || { echo "runtime headers missing - run ./rebuild-rt.sh first"; exit 1; }
R="$PWD/llvm-mingw/lib/clang/23/include"
V1="$PWD/llvm-mingw/i686-w64-mingw32/include/c++/v1"
printf '#!/bin/sh\nexec i686-w64-mingw32-clang -nostdinc -isystem "%s" -isystem "%s" "$@"\n' \
  "$R" "$INC" > "$DL/cc"
printf '#!/bin/sh\nexec i686-w64-mingw32-clang++ -nostdinc -isystem "%s" -isystem "%s" -isystem "%s" "$@"\n' \
  "$V1" "$R" "$INC" > "$DL/cxx"
chmod +x "$DL/cc" "$DL/cxx"
CC="$DL/cc"
CXX="$DL/cxx"
LIBDIR=lib

# libopenmpt 0.8.9 source, unpacked fresh into its own build tree, with the
# rare formats marked for the common-formats edition (no change without
# -DMODJUKE95_COMMON_FORMATS)
B=libopenmpt-win95-build
unpack_libopenmpt "$B"
python3 -B scripts/formats.py "$B"

# static libopenmpt for Win95
BASEFLAGS="-DMPT_LIBCXX_QUIRK_NO_STD_THREAD=1 -DMPT_LOG_GLOBAL_LEVEL_STATIC -DMPT_LOG_GLOBAL_LEVEL=0"
mk() {
  (cd "$B" && make CONFIG=mingw32crt \
     CC="$CC" CXX="$CXX" AR=i686-w64-mingw32-ar \
     WINDOWS_VERSION=win95 STDCXX=gnu++17 MPT_COMPILER_NOIPARA=0 \
     STATIC_LIB=1 SHARED_LIB=0 OPENMPT123=0 EXAMPLES=0 IN_OPENMPT=0 XMP_OPENMPT=0 -j2)
  sh scripts/ssescan.sh "$B/bin/libopenmpt.a" >/dev/null || { echo "FAIL: SSE in $B/bin/libopenmpt.a"; exit 1; }
}
mkdir -p "$LIBDIR" include/libopenmpt
export CPPFLAGS="$BASEFLAGS"
mk
cp "$B/bin/libopenmpt.a" "$LIBDIR/libopenmpt.a"
# common-formats edition: only the two patched tables differ, so only they
# are rebuilt (touch: make doesn't track CPPFLAGS)
export CPPFLAGS="$BASEFLAGS -DMODJUKE95_COMMON_FORMATS"
touch "$B/soundlib/Sndfile.cpp" "$B/soundlib/Tables.cpp"
mk
cp "$B/bin/libopenmpt.a" "$LIBDIR/libopenmpt-common.a"

# public C/C++ API headers
for h in "$B"/libopenmpt/libopenmpt*.h "$B"/libopenmpt/libopenmpt*.hpp; do
  case "$h" in *_internal.h) continue ;; esac
  cp "$h" include/libopenmpt/
done
rm -rf "$B"
echo "OK: $LIBDIR/libopenmpt.a, $LIBDIR/libopenmpt-common.a, include/libopenmpt/"
