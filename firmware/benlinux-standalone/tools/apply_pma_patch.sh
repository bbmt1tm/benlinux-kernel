#!/usr/bin/env bash
# Apply the BenLinux Core 1 PMA patch to an ESP-IDF tree.
#
# Marks the CPU subsystem (CLIC) and HP peripherals (systimer, INTMTX) as
# non-cacheable for Core 1 only, so Linux reads live register values.
# Core 0 entries are unchanged.
#
# Usage: tools/apply_pma_patch.sh [IDF_PATH]
# Refuses to touch a file that is neither stock v5.5.1 nor already patched.
set -euo pipefail

IDF="${1:-${IDF_PATH:-}}"
if [ -z "$IDF" ]; then
  echo "Usage: $0 <IDF_PATH>  (or set IDF_PATH)" >&2
  exit 1
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/esp-idf-patches/cpu_region_protect.c"
DST="$IDF/components/esp_hw_support/port/esp32p4/cpu_region_protect.c"

STOCK_SHA="87fa41f05afc0d7782c4c74aa067cb20b77513ae1e8ee483bbfd74834fd6230b"   # ESP-IDF v5.5.1
PATCHED_SHA="7ac47f5ad7f7c6578fd91153cbdcd502f2862eb65b349d73157ec566df9f4088"

[ -f "$DST" ] || { echo "Not found: $DST" >&2; exit 1; }
CUR_SHA="$(sha256sum "$DST" | cut -d' ' -f1)"

if [ "$CUR_SHA" = "$PATCHED_SHA" ]; then
  echo "PMA patch already applied: $DST"
  exit 0
fi
if [ "$CUR_SHA" != "$STOCK_SHA" ]; then
  echo "Refusing: $DST is not stock ESP-IDF v5.5.1 (sha256 $CUR_SHA)." >&2
  echo "Port the change by hand using esp-idf-patches/0001-esp32p4-core1-pma-noncacheable.patch" >&2
  exit 1
fi

cp "$SRC" "$DST"
echo "PMA patch applied: $DST"
