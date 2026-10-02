#!/bin/sh -e

# Recreate the modjuke95 build environment on Linux, x86_64.
#
#   ./rebuild.sh              fetch the llvm-mingw cross toolchain only
#   ./rebuild.sh --libopenmpt also build the Win95 lib/libopenmpt.a from source
#                             and install its headers into include/

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

# libopenmpt 0.8.9 source, unpacked fresh into its own build tree
B=libopenmpt-win95-build
unpack_libopenmpt "$B"

# static libopenmpt for Win95
cd "$B"
export CPPFLAGS="-DMPT_LIBCXX_QUIRK_NO_STD_THREAD=1 -DMPT_LOG_GLOBAL_LEVEL_STATIC -DMPT_LOG_GLOBAL_LEVEL=0"
make CONFIG=mingw32crt \
     CC=i686-w64-mingw32-clang CXX=i686-w64-mingw32-clang++ AR=i686-w64-mingw32-ar \
     WINDOWS_VERSION=win95 STDCXX=gnu++17 MPT_COMPILER_NOIPARA=0 \
     STATIC_LIB=1 SHARED_LIB=0 OPENMPT123=0 EXAMPLES=0 IN_OPENMPT=0 XMP_OPENMPT=0 -j2
cd ..
sh scripts/ssescan.sh "$B/bin/libopenmpt.a" >/dev/null || { echo "FAIL: SSE in $B/bin/libopenmpt.a"; exit 1; }
mkdir -p lib include/libopenmpt
cp "$B/bin/libopenmpt.a" lib/libopenmpt.a

# public C/C++ API headers
for h in "$B"/libopenmpt/libopenmpt*.h "$B"/libopenmpt/libopenmpt*.hpp; do
  case "$h" in *_internal.h) continue ;; esac
  cp "$h" include/libopenmpt/
done
rm -rf "$B"
echo "OK: lib/libopenmpt.a, include/libopenmpt/"
