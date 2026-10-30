---
name: codesize
description: MicroTech ESP32-S3 firmware code size analysis from the ESP-IDF linker map, ELF, build archives, and saved build snapshots. Use when identifying large libraries, objects, or symbols, comparing firmware builds for size regressions, checking Flash or internal RAM growth, or producing code size optimization guidance for this project.
---

# MicroTech Firmware Code Size

This skill analyzes the MicroTech ESP32-S3 application built with ESP-IDF. It
reports image storage, internal RAM, PSRAM, and RTC memory from the linker map,
attributes bytes to libraries, objects, and input sections, and compares two
builds through normalized map JSON.

## Inputs

- Application map: `build/microtech.map`
- Application ELF: `build/microtech.elf` (used by `package-size`; map analysis reads the map)
- Application binary: `build/microtech.bin`
- Build metadata: `build/project_description.json`
- Application archives: `build/esp-idf/**/*.a`
- Toolchain size command: `xtensa-esp32s3-elf-size`, located through
  `build/project_description.json` (`monitor_toolprefix`) or `~/.espressif/tools`
- Saved comparison input: any build directory or map file produced by this
  project

The map file and ELF must come from the same build.

## Entry Point

Run the wrapper from the repository root:

```bash
python3 .agents/skills/codesize/codesize.py <subcommand>
```

Supported subcommands:

- `analyze-current`: analyze `build/microtech.map`
- `analyze-archives`: run the toolchain `size -t` over all application archives
- `compare-builds`: compare two build directories or map files
- `package-size`: summarize built package artifacts

## Current Map

```bash
python3 .agents/skills/codesize/codesize.py analyze-current --top 30
python3 .agents/skills/codesize/codesize.py analyze-current --group-by domain
python3 .agents/skills/codesize/codesize.py analyze-current --group-by object --top 50
python3 .agents/skills/codesize/codesize.py analyze-current --show-symbols
```

`--group-by library` is the default. The report includes the largest libraries
and input sections, plus Flash Load share and the MicroTech domain breakdown.
`--group-by object` ranks object files, and `--group-by domain` aggregates the
engineering domains. `--map` selects another map file, and `--build-dir`
selects another ESP-IDF build directory.

When estimating LVGL widget savings, check the default theme: in LVGL 9,
`lv_theme_default.c` references every enabled widget class, so `--gc-sections`
retains objects such as `lv_scale.c.obj` even when application code never calls
the widget. Disabling the widget Kconfig or replacing the default theme is
required; a build with all widget options enabled cannot rely on link-time
garbage collection alone.

## Archive Sizes

```bash
python3 .agents/skills/codesize/codesize.py analyze-archives --top 30
```

This measures every archive under `build/esp-idf/` with the Xtensa size tool.
Archive totals describe archive contents; they are not image contribution.
Use the map report for bytes that are actually linked into the application.
LVGL demo/example objects can dominate archive totals while being discarded by
`--gc-sections`; never use archive rankings alone to estimate app image growth.

## Build Comparison

Save the current map before rebuilding, then compare:

```bash
cp build/microtech.map /tmp/microtech-before.map
# rebuild
python3 .agents/skills/codesize/codesize.py compare-builds \
  --before /tmp/microtech-before.map \
  --after build/microtech.map \
  --object-diff \
  --report-dir
```

`--before` and `--after` accept a map file or a build directory containing the
application map. `--report-dir` writes Markdown and JSON under `build/codesize/`
unless a directory is supplied; `--output` can be used at the same time. Both
inputs may have the same application map basename. `--symbol-diff` includes
input-section-level deltas (the map parser uses input sections as its symbol
unit, not ELF symbol-table entries) and normalizes every symbol; it is slower
than the default library and object view.

Use `--budget <json>` to report budget violations without failing by default.
Budget values are bytes and describe allowed positive growth; negative values
are allowed for code removal. The supported shape is:

```json
{
  "max_flash_growth": 4096,
  "max_ram_growth": 1024,
  "max_psram_growth": 0,
  "libraries": {
    "libfreetype.a": {"max_flash_growth": 512, "max_ram_growth": 0}
  },
  "domains": {
    "ui/graphics": {"max_flash_growth": 2048, "max_ram_growth": 512}
  }
}
```

Use `--fail-on-budget` to return a non-zero exit code when a budget is exceeded.
New libraries and their domains are included in library and domain budget
checks. `--fail-on-growth <percent>` is available when a total Flash percentage
limit is more appropriate than byte budgets.

## Package Sizes

```bash
python3 .agents/skills/codesize/codesize.py package-size
python3 .agents/skills/codesize/codesize.py package-size \
  --before /path/to/older-build \
  --after /path/to/newer-build
```

The report covers the application binary, ELF, map, bootloader binary, partition
table binary, OTA data, the final generated resource image, and release zip
files present in the directory. The `resources` metric is the final `res.bin`
image; temporary staging trees such as `.res.bin.tmp` are excluded. The
`Partition Headroom` table compares `app_bin` with each OTA app slot and the
resource image with the `res` data partition.

## Accounting Model

The map report uses the linker output sections produced by the ESP-IDF linker
scripts:

| Bucket | Linker sections | Flash Load | Internal RAM |
| --- | --- | --- | --- |
| `.text` | `.flash.text` | yes | no |
| `.iram_text` | `.iram0.text`, `.iram0.vectors` | yes | yes |
| `.rtc_text` | `.rtc.text`, `.rtc.force_fast`, `.rtc.force_slow` | yes | yes |
| `.rodata` | `.flash.rodata`, `.flash.appdesc`, `.flash.tdata` | yes | no |
| `.data` | `.dram0.data`, `.rtc.data`, `.iram0.data` | yes | yes |
| `.bss` | `.dram0.bss`, `.rtc.bss`, `.rtc_noinit`, `.iram0.bss`, `.noinit.*` | no | yes |
| `.psram_bss` | `.ext_ram.bss`, `.ext_ram_noinit` | no | no (PSRAM) |
| `.psram_data` | `.ext_ram.data` | no | no (PSRAM) |
| `.reserved_ram` | `.rtc_reserved` | no | yes |
| `.rodata_noload` | `.flash.rodata_noload` | no | no |

The output-section alignment placeholders `.dram0.dummy`, `.ext_ram.dummy`,
and `.flash_rodata_dummy`, plus the `.iram0.text_end`, `.noinit`, and
`.dram0.heap_start` markers, are excluded from runtime totals. `.rodata_noload`
is accounted separately and is not part of Flash Load. `NOLOAD/Reserved` in the
Markdown overview overlaps Internal RAM and PSRAM; it is not additional memory.

Library, object, and domain tables use input-section attribution. Input
`.rodata.*` sections placed inside `.flash.text` remain `.rodata` in those
tables. The linker merges mergeable `.rodata.str*` input sections into larger
literal pools; the map records the first section with its total size and a
`size before relaxing` value. The parser attributes only that first section's
pre-merge size to its archive and reports the remainder as `[merged literals]`
in the `shared` domain. The JSON `summary` exposes `attributed_bytes`,
`merged_literals`, `alignment_fill`, and `unattributed`; `unattributed` is
alignment filler not assigned to a library. `COMMON` input rows and input
sections whose names contain `dummy` are attributed normally.

## Verification

Cross-check the map report against ESP-IDF:

```bash
idf.py size --format json2
```

For a matching map and ELF, the map report `Flash Load` equals the `total_size`
field, and `Internal RAM` equals the sum of the `DIRAM`, `RTC FAST`, and
`RTC SLOW` used bytes. A mismatch usually means the map and ELF are from
different builds, but can also indicate a parser accounting defect; confirm
both artifacts and the parser version before using the result.

## Output

- Default format: Markdown.
- JSON is the normalization format for map-to-map comparison.
- Map JSON contains `schema_version`, `tool_version`, `project`, `target`,
  `generated_at`, `architecture`, `summary`, `libraries`, `domains`, `objects`,
  and `top_symbols`; `summary` also contains `attributed_bytes`,
  `merged_literals`, `alignment_fill`, and `unattributed`.
- Package JSON adds a `partitions` array per version with partition size,
  matched image, image size, free bytes, and used percentage.
- Comparison keys are stable library paths, not basename-only names.
- Reports are local engineering output; they are not firmware artifacts.
