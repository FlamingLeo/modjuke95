#!/bin/sh

# Scan archives/objects for SSE+ instructions.
#
# Usage: scripts/ssescan.sh file.o|file.obj|lib.a ...
# Prints "<file>[(member)] : <hits>" for every object with SSE+ register use and exits 1 if any were found.

D="$(cd "$(dirname "$0")" && pwd)"
P="$D/../llvm-mingw/bin"
[ -x "$P/i686-w64-mingw32-objdump" ] || { echo "toolchain missing - run ./rebuild.sh"; exit 2; }

found=0
for target in "$@"; do
  case "$target" in
    *.a|*.o|*.obj) ;;
    *) echo "skipping $target (not .a/.o/.obj)"; continue ;;
  esac
  [ -f "$target" ] || { echo "missing: $target"; exit 2; }

  # count %xmm/%ymm/%zmm operands
  out=$("$P/i686-w64-mingw32-objdump" -d "$target" 2>/dev/null | awk '
    /:\tfile format / { cur = $0; sub(/:\tfile format .*/, "", cur); next }
    /%[xyz]mm[0-9]/ { hits[cur]++ }
    END { for (m in hits) printf "%s : %d\n", m, hits[m] }')
  [ -n "$out" ] && { echo "$out"; found=1; }
done
[ "$found" = 0 ] && echo "SSE scan clean"
exit "$found"
