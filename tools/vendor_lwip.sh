#!/usr/bin/env bash
# tools/vendor_lwip.sh — чистая выгрузка lwIP без мусора
#
# lwIP upstream грязный: doc/doxygen/output/*.html, contrib/ports/win32/*.sln,
# src/apps/http/fs/*.html и т.д. Коммитить их в ARC нельзя - Makefile wildcard
# соберет makefsdata.c как часть ядра.
#
# Этот скрипт:
#   1. качает tarball нужного ревиза с GitHub
#   2. распаковывает во временную папку
#   3. копирует только whitelist в bsd/net/lwip/
#   4. удаляет мусор (*.html, *.sln, *.vcxproj, doxygen)
#   5. ставит маркер .vendor_rev
#
# Использование:
#   tools/vendor_lwip.sh --rev master              # последний master
#   tools/vendor_lwip.sh --rev STABLE-2_2_0        # тег
#   tools/vendor_lwip.sh --rev 631d4a0 --pin       # конкретный коммит + записать в bsd/net/lwip/.pinned
#   tools/vendor_lwip.sh --clean                   # удалить bsd/net/lwip/src
#
# Идемпотентен. Требует curl + tar.

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

# Определить URL
if [[ "$REV" =~ ^[0-9a-f]{7,40}$ ]]; then
  URL="https://github.com/lwip-tcpip/lwip/archive/${REV}.tar.gz"
else
  URL="https://github.com/lwip-tcpip/lwip/archive/refs/heads/${REV}.tar.gz"
  # fallback для тегов: refs/tags/
  # пробуем heads, если 404 - tags
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

# Whitelist: только то что нужно ядру ARC
# Копируем минимально необходимый набор (аналог COREFILES+CORE4FILES+APIFILES+NETIFFILES)
# Без apps/http/fs/*.html, без contrib, без doc, без test
echo "==> installing to $DST"
mkdir -p "$DST"

# Очистить старое src если есть
rm -rf "$DST/src"

mkdir -p "$DST/src"
cp -a "$EXTRACTED/src/core"          "$DST/src/"
cp -a "$EXTRACTED/src/api"           "$DST/src/"
cp -a "$EXTRACTED/src/include"       "$DST/src/"
cp -a "$EXTRACTED/src/netif"         "$DST/src/"
# Filelists нужны для Makefile
cp -a "$EXTRACTED/src/Filelists.mk"  "$DST/src/" 2>/dev/null || true
cp -a "$EXTRACTED/src/Filelists.cmake" "$DST/src/" 2>/dev/null || true
cp -a "$EXTRACTED/src/FILES"         "$DST/src/" 2>/dev/null || true
cp -a "$EXTRACTED/COPYING"           "$DST/"
cp -a "$EXTRACTED/README"            "$DST/" 2>/dev/null || true

# Удалить мусор внутри скопированного (на всякий: http/fs/*.html, ppp если не нужен)
# Оставляем только netif/ethernet.c, slipif.c - остальное (ppp, lowpan) не нужно для ARC MVP
# Но не удаляем автоматически - Makefile whitelist их не соберет

# Жесткая чистка: html/sln/vcxproj/doxygen никогда не должны попасть в DST
find "$DST" -type f \( -name "*.html" -o -name "*.sln" -o -name "*.vcxproj" -o -name "*.vcfilters" -o -name "*.py" \) -delete 2>/dev/null || true
rm -rf "$DST/doc" "$DST/contrib" "$DST/test" 2>/dev/null || true

# Записать ревизию
echo "$REV" > "$DST/.vendor_rev"
if [[ -n "$PIN" ]]; then
  echo "$REV" > "$DST/.pinned"
fi

# Статистика
echo "==> done. DST size: $(du -sh "$DST" | cut -f1), files: $(find "$DST" -type f | wc -l)"
echo "    whitelist:"
find "$DST/src" -type f -name "*.c" | sort | sed 's|.*|      &|'
echo "    includes:"
find "$DST/src/include" -type f | sort | sed 's|.*|      &|'
echo ""
echo "    мусора нет: html=$(find "$DST" -name "*.html" | wc -l) sln=$(find "$DST" -name "*.sln" | wc -l) vcxproj=$(find "$DST" -name "*.vcxproj" | wc -l)"
echo ""
echo "Next: git add $DST bsd/net/lwipopts.h && make ARCH=amd64"
