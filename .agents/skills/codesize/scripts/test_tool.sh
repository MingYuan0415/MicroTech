#!/bin/bash
# Self-test for the MicroTech codesize skill.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../../.." && pwd)"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT
cd "${ROOT_DIR}"

echo "MicroTech codesize self-test"
echo "============================"

echo "Testing archive report..."
python3 "${SCRIPT_DIR}/analyze_size_output.py" --help >/dev/null
cat > "${TMP_DIR}/size.txt" <<'EOF'
   text	   data	    bss	    dec	    hex	filename
   1234	    100	     50	   1384	    568	main.o (ex /tmp/build/esp-idf/apps/libapps.a)
   5678	    200	    100	   5978	   175a	state.o (ex /tmp/build/esp-idf/apps/libapps.a)
   2000	     50	     25	   2075	     81b	driver.o (ex /tmp/build/esp-idf/esp_driver_gpio/libesp_driver_gpio.a)
  11912	    425	    225	  12562	   3112	(TOTALS)
EOF
python3 "${SCRIPT_DIR}/analyze_size_output.py" "${TMP_DIR}/size.txt" --format json --output "${TMP_DIR}/size.json" >/dev/null
python3 - "${TMP_DIR}/size.json" <<'PY'
import json
import sys

data = json.load(open(sys.argv[1], encoding="utf-8"))
assert data["summary"]["total_text"] == 8912
assert data["summary"]["total_flash"] == 9262
assert len(data["libraries"]) == 2
PY
echo "  PASSED"

echo "Testing ESP-IDF map report (real input names, COMMON, PSRAM noinit)..."
cat > "${TMP_DIR}/app.map" <<'EOF'
Memory Configuration

Linker script and memory map

.flash.text     0x42000020   0x1000
 .literal.main
                0x42000020        0x10 libapp.a(main.o)
 .text.main
                0x42000030       0x200 libapp.a(main.o)
 .rodata.title
                0x42000230       0x100 libapp.a(main.o)
 .text.dummy_handler
                0x42000330        0x20 libapp.a(dummy.o)
                0x42000330                dummy_handler

.flash.rodata   0x3c200120    0x400
 .rodata.str1.1
                0x3c200120       0x300 libapp.a(main.o)
 .rodata.first.str1.4
                0x3c200420       0x100 libapp.a(first.o)
                                 0x40 (size before relaxing)
 .rodata.second.str1.4
                0x3c200420        0x0 libapp.a(second.o)
                                 0xc0 (size before relaxing)

.dram0.data     0x3fca6000     0x100
 .data.counter
                0x3fca6000        0x20 libapp.a(main.o)

.dram0.bss      0x3fcad808     0x200
 .bss.buffer
                0x3fcad808       0x180 libapp.a(state.o)
 COMMON         0x3fcad988        0x20 libapp.a(state.o)
                0x3fcad988                common_value

.iram0.text     0x40378404     0x100
 .text.ramfunc
                0x40378404       0x100 libram.a(ram.o)

.ext_ram.bss    0x3c280000      0x80
 .ext_ram.bss.0
                0x3c280000        0x80 libapp.a(psram.o)

.ext_ram_noinit 0x3c280080      0x40
 .ext_ram_noinit.0
                0x3c280080        0x40 libapp.a(psram.o)
EOF
python3 "${SCRIPT_DIR}/analyze_map_file.py" "${TMP_DIR}/app.map" --format json --all-symbols --output "${TMP_DIR}/app.json" >/dev/null
python3 - "${TMP_DIR}/app.json" <<'PY'
import json
import sys

data = json.load(open(sys.argv[1], encoding="utf-8"))
summary = data["summary"]
assert summary["flash"] == 0x1600
assert summary["internal_ram"] == 0x400
assert summary["psram"] == 0xC0
assert summary["attributed_bytes"] + summary["unattributed"] == summary["accounted_sections"]
assert summary["alignment_fill"] >= 0
libraries = {item["name"]: item for item in data["libraries"]}
assert libraries["libapp.a"]["sections"][".text"] == 0x230
assert libraries["libapp.a"]["sections"][".rodata"] == 0x440
assert libraries["[merged literals]"]["sections"][".rodata"] == 0xC0
assert summary["merged_literals"] == 0xC0
assert libraries["libapp.a"]["sections"][".bss"] == 0x1A0
assert libraries["libapp.a"]["sections"][".psram_bss"] == 0xC0
assert libraries["libram.a"]["sections"][".iram_text"] == 0x100
names = {item["name"] for item in data["symbols"]}
assert "dummy_handler" in names
assert "common_value" in names
PY
echo "  PASSED"

echo "Testing symbol limit..."
python3 "${SCRIPT_DIR}/analyze_map_file.py" "${TMP_DIR}/app.map" --format json --symbol-limit 2 --output "${TMP_DIR}/limited.json" >/dev/null
python3 - "${TMP_DIR}/limited.json" <<'PY'
import json
import sys

data = json.load(open(sys.argv[1], encoding="utf-8"))
assert len(data["top_symbols"]) == 2
PY
echo "  PASSED"

echo "Testing build comparison..."
python3 "${SCRIPT_DIR}/compare_codesize.py" "${TMP_DIR}/app.json" "${TMP_DIR}/app.json" --type json --format json --output "${TMP_DIR}/compare.json" >/dev/null
python3 - "${TMP_DIR}/compare.json" <<'PY'
import json
import sys

data = json.load(open(sys.argv[1], encoding="utf-8"))
assert data["summary"]["delta"]["flash"] == 0
assert data["summary"]["delta"]["ram"] == 0
PY
echo "  PASSED"

echo "Testing same-basename build comparison and report-dir/output..."
mkdir -p "${TMP_DIR}/before" "${TMP_DIR}/after"
cat > "${TMP_DIR}/before/microtech.map" <<'EOF'
Memory Configuration

Linker script and memory map

.flash.text     0x42000020     0x100
 .text.main
                0x42000020       0x100 libapp.a(main.o)
EOF
cp "${TMP_DIR}/before/microtech.map" "${TMP_DIR}/after/microtech.map"
sed -i 's/^\.flash.text     0x42000020     0x100$/.flash.text     0x42000020     0x101/' "${TMP_DIR}/after/microtech.map"
python3 "${ROOT_DIR}/.agents/skills/codesize/codesize.py" compare-builds \
    --before "${TMP_DIR}/before/microtech.map" \
    --after "${TMP_DIR}/after/microtech.map" \
    --format json --output "${TMP_DIR}/same-stem.json" >/dev/null
python3 - "${TMP_DIR}/same-stem.json" <<'PY'
import json
import sys

data = json.load(open(sys.argv[1], encoding="utf-8"))
assert data["summary"]["delta"]["flash"] == 1
assert data["before_file"].endswith("before/microtech.map")
assert data["after_file"].endswith("after/microtech.map")
PY
python3 "${ROOT_DIR}/.agents/skills/codesize/codesize.py" compare-builds \
    --before "${TMP_DIR}/before/microtech.map" \
    --after "${TMP_DIR}/after/microtech.map" \
    --report-dir "${TMP_DIR}/reports" \
    --format json --output "${TMP_DIR}/report-output.json" >/dev/null
test -n "$(find "${TMP_DIR}/reports" -maxdepth 1 -name '*.md' -print -quit)"
test -n "$(find "${TMP_DIR}/reports" -maxdepth 1 -name '*.json' -print -quit)"
test -f "${TMP_DIR}/report-output.json"
grep -q 'before/microtech.map' "${TMP_DIR}/reports"/*.md
echo "  PASSED"

echo "Testing budget with a newly added library..."
cat > "${TMP_DIR}/budget-before.json" <<'EOF'
{
  "schema_version": 2,
  "project": "MicroTech",
  "summary": {"flash": 100, "ram": 0, "psram": 0, "sections": {".text": 100}, "accounted_sections": 100},
  "libraries": [
    {"key": "liba.a", "name": "liba.a", "domain": "other", "size": 100, "sections": {".text": 100}, "flash_load": 100, "internal_ram": 0}
  ]
}
EOF
cat > "${TMP_DIR}/budget-after.json" <<'EOF'
{
  "schema_version": 2,
  "project": "MicroTech",
  "summary": {"flash": 300, "ram": 0, "psram": 0, "sections": {".text": 300}, "accounted_sections": 300},
  "libraries": [
    {"key": "liba.a", "name": "liba.a", "domain": "other", "size": 100, "sections": {".text": 100}, "flash_load": 100, "internal_ram": 0},
    {"key": "libb.a", "name": "libb.a", "domain": "other", "size": 200, "sections": {".text": 200}, "flash_load": 200, "internal_ram": 0}
  ]
}
EOF
cat > "${TMP_DIR}/budget.json" <<'EOF'
{"libraries": {"libb.a": {"max_flash_growth": 0}}}
EOF
if python3 "${SCRIPT_DIR}/compare_codesize.py" "${TMP_DIR}/budget-before.json" "${TMP_DIR}/budget-after.json" \
    --type json --format markdown --budget "${TMP_DIR}/budget.json" --fail-on-budget \
    >"${TMP_DIR}/budget-report.md" 2>"${TMP_DIR}/budget.err"; then
    echo "Budget violation was not reported" >&2
    exit 1
fi
grep -q 'Library libb.a flash growth' "${TMP_DIR}/budget-report.md"
echo "  PASSED"

if [ -f "${ROOT_DIR}/build/microtech.map" ]; then
    echo "Testing current build map..."
    python3 "${ROOT_DIR}/.agents/skills/codesize/codesize.py" analyze-current --format json --output "${TMP_DIR}/current.json" >/dev/null
    python3 - "${TMP_DIR}/current.json" <<'PY'
import json
import sys

data = json.load(open(sys.argv[1], encoding="utf-8"))
summary = data["summary"]
assert data["project"] == "MicroTech"
assert summary["flash"] > 0
assert summary["internal_ram"] > 0
assert summary["psram"] > 0
assert summary["attributed_bytes"] + summary["unattributed"] == summary["accounted_sections"]
assert data["libraries"]
PY
    if command -v idf.py >/dev/null 2>&1 && [ -f "${ROOT_DIR}/build/microtech.elf" ] && \
        idf.py size --format json2 >"${TMP_DIR}/idf-size.txt" 2>/dev/null; then
        python3 - "${TMP_DIR}/current.json" "${TMP_DIR}/idf-size.txt" <<'PY'
import json
import sys

map_data = json.load(open(sys.argv[1], encoding="utf-8"))
text = open(sys.argv[2], encoding="utf-8", errors="ignore").read()
start = text.find("{")
if start < 0:
    raise SystemExit("idf.py size did not emit JSON")
idf_data, _ = json.JSONDecoder().raw_decode(text[start:])
layout = {item["name"]: item for item in idf_data["layout"]}
rtc_used = layout["RTC FAST"]["used"] + layout["RTC SLOW"]["used"]
assert map_data["summary"]["flash"] == idf_data["total_size"]
assert map_data["summary"]["internal_ram"] == layout["DIRAM"]["used"] + rtc_used
assert map_data["summary"]["psram"] == layout["External RAM"]["used"]
PY
    else
        echo "Skipping idf.py size cross-check (idf.py unavailable or build incomplete)"
    fi
    echo "  PASSED"

    echo "Testing default domain report..."
    python3 "${ROOT_DIR}/.agents/skills/codesize/codesize.py" analyze-current --format markdown --top 5 --output "${TMP_DIR}/domain-report.md" >/dev/null
    grep -q '^## By Domain$' "${TMP_DIR}/domain-report.md"
    echo "  PASSED"

    echo "Testing archive analysis..."
    python3 "${ROOT_DIR}/.agents/skills/codesize/codesize.py" analyze-archives --format json --output "${TMP_DIR}/archives.json" >/dev/null
    python3 - "${TMP_DIR}/archives.json" <<'PY'
import json
import sys

data = json.load(open(sys.argv[1], encoding="utf-8"))
assert data["summary"]["total_flash"] > 0
assert data["libraries"]
PY
    echo "  PASSED"

    echo "Testing package-size..."
    python3 "${ROOT_DIR}/.agents/skills/codesize/codesize.py" package-size --format json --output "${TMP_DIR}/package.json" >/dev/null
    python3 - "${TMP_DIR}/package.json" "${ROOT_DIR}/build/mmap_build/app_res_fs/res/res.bin" <<'PY'
import json
import sys
from pathlib import Path

data = json.load(open(sys.argv[1], encoding="utf-8"))
assert data["versions"][0]["sizes"]["app_bin"] > 0
assert data["versions"][0]["sizes"]["app_elf"] > 0
resource_image = Path(sys.argv[2])
if resource_image.is_file():
    assert data["versions"][0]["sizes"]["resources"] == resource_image.stat().st_size
partitions = {item["name"]: item for item in data["versions"][0]["partitions"]}
assert partitions["ota_0"]["image"] == "app_bin"
assert partitions["res"]["image"] == "resources"
PY
    echo "  PASSED"
else
    echo "Skipping current build tests: build/microtech.map not found"
fi

echo "All tests passed."
