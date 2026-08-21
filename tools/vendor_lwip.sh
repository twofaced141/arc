#!/usr/bin/env bash
# tools/vendor_lwip.sh — clean lwIP vendor import without upstream clutter
#
# Upstream lwIP is noisy: doc/doxygen/output/*.html, contrib/ports/win32/*.sln,
# src/apps/http/fs/*.html, etc. They must not be committed to ARC — a Makefile
# wildcard would pull makefsdata.c into the kernel build.
#
# This script:
#   1. downloads the requested revision tarball from GitHub
#   2. extracts it to a temporary directory
#   3. copies only the whitelisted subset into bsd/net/lwip/
#   4. removes clutter (*.html, *.sln, *.vcxproj, doxygen)
#   5. writes a .vendor_rev marker
#
# Usage:
#   tools/vendor_lwip.sh --rev master              # latest master
#   tools/vendor_lwip.sh --rev STABLE-2_2_0        # tag
#   tools/vendor_lwip.sh --rev 631d4a0 --pin       # specific commit + pin to bsd/net/lwip/.pinned
#   tools/vendor_lwip.sh --clean                   # remove bsd/net/lwip/src
#
# Idempotent. Requires curl + tar.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DST="$ROOT/bsd/net/lwip"
SRC_SUB="src"

REV="master"
PIN=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --rev) REV="$2"; shift 2 ;;
    --pin) PIN="1"; shift ;;
    --clean) rm -rf "$DST/src" "$DST/COPYING" "$DST/.vendor_rev"; echo "cleaned $DST"; exit 0 ;;
    -h|--help) grep '^#' "$0" | cut -c4-; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 1 ;;
  esac
done

# Resolve URL
if [[ "$REV" =~ ^[0-9a-f]{7,40}$ ]]; then
  URL="https://github.com/lwip-tcpip/lwip/archive/${REV}.tar.gz"
else
  URL="https://github.com/lwip-tcpip/lwip/archive/refs/heads/${REV}.tar.gz"
  # fallback for tags: refs/tags/
  # try heads, fall back to tags on 404
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

echo "==> fetching lwIP rev=$REV"
if ! curl -LfsS "$URL" -o "$TMPDIR/lwip.tar.gz"; then
  URL2="https://github.com/lwip-tcpip/lwip/archive/refs/tags/${REV}.tar.gz"
  echo "    heads not found, trying $URL2"
  curl -LfsS "$URL2" -o "$TMPDIR/lwip.tar.gz"
fi

tar -xzf "$TMPDIR/lwip.tar.gz" -C "$TMPDIR"
EXTRACTED="$(find "$TMPDIR" -maxdepth 1 -type d -name 'lwip-*' | head -n1)"
if [[ -z "$EXTRACTED" ]]; then
  echo "extract failed" >&2; exit 1
fi
echo "    extracted to $EXTRACTED ($(du -sh "$EXTRACTED" | cut -f1))"

# Whitelist: only what the ARC kernel needs
# Copy the minimal required set (equivalent to COREFILES+CORE4FILES+APIFILES+NETIFFILES)
# Excludes apps/http/fs/*.html, contrib, doc, and test
echo "==> installing to $DST"
mkdir -p "$DST"

# Clean previous src if present
rm -rf "$DST/src"

mkdir -p "$DST/src"
cp -a "$EXTRACTED/src/core"          "$DST/src/"
cp -a "$EXTRACTED/src/api"           "$DST/src/"
cp -a "$EXTRACTED/src/include"       "$DST/src/"
cp -a "$EXTRACTED/src/netif"         "$DST/src/"
# Filelists needed by the Makefile
cp -a "$EXTRACTED/src/Filelists.mk"  "$DST/src/" 2>/dev/null || true
cp -a "$EXTRACTED/src/Filelists.cmake" "$DST/src/" 2>/dev/null || true
cp -a "$EXTRACTED/src/FILES"         "$DST/src/" 2>/dev/null || true
cp -a "$EXTRACTED/COPYING"           "$DST/"
cp -a "$EXTRACTED/README"            "$DST/" 2>/dev/null || true

# Remove leftover clutter inside the copy (e.g. http/fs/*.html, optional ppp)
# Keep only netif/ethernet.c, slipif.c for the ARC MVP; the rest (ppp, lowpan)
# is not needed but is harmless — the Makefile whitelist will not build it.

# Hard cleanup: html/sln/vcxproj/doxygen must never land in DST
find "$DST" -type f \( -name "*.html" -o -name "*.sln" -o -name "*.vcxproj" -o -name "*.vcfilters" -o -name "*.py" \) -delete 2>/dev/null || true
rm -rf "$DST/doc" "$DST/contrib" "$DST/test" 2>/dev/null || true

# Record revision
echo "$REV" > "$DST/.vendor_rev"
if [[ -n "$PIN" ]]; then
  echo "$REV" > "$DST/.pinned"
fi

# Stats
echo "==> done. DST size: $(du -sh "$DST" | cut -f1), files: $(find "$DST" -type f | wc -l)"
echo "    whitelist:"
find "$DST/src" -type f -name "*.c" | sort | sed 's|.*|      &|'
echo "    includes:"
find "$DST/src/include" -type f | sort | sed 's|.*|      &|'
echo ""
echo "    clutter check: html=$(find "$DST" -name "*.html" | wc -l) sln=$(find "$DST" -name "*.sln" | wc -l) vcxproj=$(find "$DST" -name "*.vcxproj" | wc -l)"
echo ""
echo "Next: git add $DST bsd/net/lwipopts.h && make ARCH=amd64"
