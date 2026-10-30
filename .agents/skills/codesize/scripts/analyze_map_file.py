#!/usr/bin/env python3
"""Linker map analyzer for the MicroTech ESP32-S3 firmware."""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from datetime import datetime, timezone
from io import StringIO
from pathlib import Path
from typing import Any, Dict, List, Optional, Set, Tuple

from codesize_utils import Architecture, detect_architecture, format_size

PROJECT_LABEL = "MicroTech"
TARGET_NAME = "microtech (ESP32-S3)"
SCHEMA_VERSION = 2
TOOL_VERSION = "2.0"

SECTION_ORDER = [
    ".text",
    ".iram_text",
    ".rtc_text",
    ".rodata",
    ".data",
    ".bss",
    ".psram_bss",
    ".psram_data",
    ".reserved_ram",
    ".rodata_noload",
    "other",
]

PLACEHOLDER_SECTIONS: Set[str] = {
    ".dram0.dummy",
    ".ext_ram.dummy",
    ".flash_rodata_dummy",
    ".flash.rodata_dummy",
}

# Bytes stored in the application image.
FLASH_LOAD_SECTIONS: Set[str] = {".text", ".iram_text", ".rtc_text", ".rodata", ".data"}
# Static memory occupied at run time.
INTERNAL_RAM_SECTIONS: Set[str] = {".data", ".bss", ".iram_text", ".rtc_text", ".reserved_ram"}
PSRAM_SECTIONS: Set[str] = {".psram_bss", ".psram_data"}
L2RAM_SECTIONS: Set[str] = set()
NOLOAD_SECTIONS: Set[str] = {".bss", ".psram_bss", ".reserved_ram", ".rodata_noload"}
REGION_CONTAINER_SECTIONS: Set[str] = {".iram_text", ".rtc_text", ".psram_bss", ".psram_data", ".reserved_ram", ".rodata_noload"}

APP_COMPONENTS = {"main", "apps", "app_core", "app_theme"}
BSP_COMPONENTS = {"bsp"}
UI_COMPONENTS = {
    "lvgl__lvgl",
    "espressif__lvgl__lvgl",
    "freetype",
    "espressif__freetype",
    "libpng",
    "espressif__libpng",
    "zlib",
    "espressif__zlib",
    "esp_lv_decoder",
    "espressif__esp_lv_decoder",
    "esp_new_jpeg",
    "espressif__esp_new_jpeg",
    "esp_lvgl_adapter",
    "espressif__esp_lvgl_adapter",
    "esp_lcd",
    "esp_lcd_sh8601",
    "espressif__esp_lcd_sh8601",
    "knob",
    "espressif__knob",
}
AUDIO_COMPONENTS = {
    "audio_service",
    "recorder_service",
    "esp_codec_dev",
    "espressif__esp_codec_dev",
}
STORAGE_COMPONENTS = {
    "fatfs",
    "littlefs",
    "joltwallet__littlefs",
    "spiffs",
    "wear_levelling",
    "nvs_flash",
    "nvs_sec_provider",
    "sdmmc",
    "sd_storage_service",
    "file_service",
    "esp_partition",
    "esp_blockdev",
    "esp_blockdev_util",
}
BT_COMPONENTS = {"bt", "ble_runtime"}
NETWORK_COMPONENTS = {
    "lwip",
    "mbedtls",
    "esp-tls",
    "http_parser",
    "esp_http_client",
    "esp_http_server",
    "esp_https_ota",
    "esp_https_server",
    "tcp_transport",
    "esp_netif",
    "esp_wifi",
    "wpa_supplicant",
    "esp_coex",
    "esp_phy",
    "wifi_service",
    "connectivity_manager",
    "weather_service",
    "device_link_service",
}
RUNTIME_COMPONENTS = {
    "freertos",
    "esp_system",
    "esp_common",
    "esp_libc",
    "cxx",
    "pthread",
    "heap",
    "log",
    "esp_timer",
    "esp_event",
    "vfs",
    "esp_stdio",
    "xtensa",
    "rt",
    "esp_rom",
    "bootloader_support",
    "app_update",
    "esp_app_format",
    "esp_bootloader_format",
    "esp_security",
}
MIDDLEWARE_COMPONENTS = {
    "chore_service",
    "device_link",
    "event_bus",
    "factory_reset_service",
    "imu_service",
    "mt_log",
    "nv_storage",
    "onboarding_service",
    "power_service",
    "system_pm",
    "time_service",
    "timer_service",
}
DRIVER_COMPONENTS = {"driver", "hal", "soc", "spi_flash", "efuse", "esp_adc"}


@dataclass
class SymbolInfo:
    """One input-section row attributed to a library and object file."""

    name: str
    address: int
    size: int
    section: str
    section_full: str
    library: str
    library_key: str
    library_path: str
    object_file: str
    domain: str
    is_function: bool
    is_cpp: bool
    flash_load: int = 0
    internal_ram: int = 0
    psram: int = 0
    l2ram: int = 0


@dataclass
class ParsedMapData:
    """Parsed linker map."""

    architecture: Architecture = Architecture.UNKNOWN
    sections: Dict[str, int] = field(default_factory=dict)
    section_addresses: Dict[str, int] = field(default_factory=dict)
    libraries: Dict[str, Dict[str, Any]] = field(default_factory=dict)
    symbols: List[SymbolInfo] = field(default_factory=list)
    discarded_size: int = 0
    fill_size: int = 0


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def new_section_totals() -> Dict[str, int]:
    return {section: 0 for section in SECTION_ORDER}


def sum_sections(sections: Dict[str, int], names: Set[str]) -> int:
    return sum(sections.get(name, 0) for name in names)


def calc_flash_load(sections: Dict[str, int]) -> int:
    return sum_sections(sections, FLASH_LOAD_SECTIONS)


def calc_internal_ram(sections: Dict[str, int]) -> int:
    return sum_sections(sections, INTERNAL_RAM_SECTIONS)


def calc_psram(sections: Dict[str, int]) -> int:
    return sum_sections(sections, PSRAM_SECTIONS)


def calc_l2ram(sections: Dict[str, int]) -> int:
    return sum_sections(sections, L2RAM_SECTIONS)


def section_metrics(section: str, size: int) -> Dict[str, int]:
    return {
        "flash_load": size if section in FLASH_LOAD_SECTIONS else 0,
        "internal_ram": size if section in INTERNAL_RAM_SECTIONS else 0,
        "psram": size if section in PSRAM_SECTIONS else 0,
        "l2ram": size if section in L2RAM_SECTIONS else 0,
    }


def normalize_library_identity(path: str) -> Tuple[str, str]:
    """Return a stable comparison key and a display name for a library path."""
    raw = path.strip().replace("\\", "/")
    if not raw:
        return "direct-object", "direct-object"

    normalized = raw
    marker = "/build/"
    if marker in normalized:
        normalized = normalized.split(marker, 1)[1]
    elif normalized.startswith("build/"):
        normalized = normalized[len("build/"):]

    if "/picolibc/" in normalized:
        normalized = "toolchain/" + normalized.split("/picolibc/", 1)[1]
    elif "/esp-idf/" in normalized:
        normalized = "esp-idf/" + normalized.split("/esp-idf/", 1)[1]

    normalized = normalized.lstrip("./")
    if not normalized:
        normalized = Path(raw).name
    return normalized, Path(normalized).name


def classify_domain(key: str, path: str, name: str) -> str:
    """Classify a linked library into a MicroTech engineering domain."""
    text = f"{key} {path} {name}".lower().replace("\\", "/")
    library_name = Path(name).name.lower()

    # Bluetooth controller/PHY archives are shipped under esp_phy but are not
    # part of the network/TLS stack.
    if library_name in {"libbt.a", "libbtbb.a", "libbtdm_app.a", "libbtdm_app_flash.a"}:
        return "bluetooth"

    component = ""
    match = re.search(r"esp-idf/components/([^/]+)/", text)
    if match:
        component = match.group(1)
    else:
        match = re.search(r"(?:^|/)esp-idf/([^/]+)/", text)
        if match:
            component = match.group(1)

    if component in APP_COMPONENTS or text.startswith("main/"):
        return "application"
    if component in BSP_COMPONENTS:
        return "bsp"
    if component in UI_COMPONENTS:
        return "ui/graphics"
    if component in AUDIO_COMPONENTS:
        return "audio/media"
    if component in STORAGE_COMPONENTS:
        return "storage"
    if component in BT_COMPONENTS:
        return "bluetooth"
    if component in NETWORK_COMPONENTS:
        return "network/tls"
    if component in RUNTIME_COMPONENTS:
        return "runtime/system"
    if component in MIDDLEWARE_COMPONENTS:
        return "middleware"
    if component in DRIVER_COMPONENTS or component.startswith(("esp_driver_", "esp_hal_")):
        return "driver/hal"

    if text.startswith("toolchain/") or Path(name).name in {
        "libc.a",
        "libm.a",
        "libgcc.a",
        "libstdc++.a",
        "libsupc++.a",
    }:
        return "toolchain"
    if any(token in text for token in ("libbtdm_app", "libbtbb", "libbt.a")):
        return "bluetooth"
    if any(token in text for token in ("libnet80211", "libcore.a", "libpp.a", "libespnow", "libmesh", "libphy", "libcoexist")):
        return "network/tls"
    return "other"


def is_ignored_section(section_name: str) -> bool:
    """Return True for metadata sections that do not occupy runtime memory."""
    return section_name.startswith(
        (
            ".debug",
            ".comment",
            ".note",
            ".eh_frame",
            ".riscv.attributes",
            ".gnu",
            ".rela",
            ".rel.",
            ".group",
            ".strtab",
            ".symtab",
            ".shstrtab",
            ".line",
            ".stab",
            ".xt.",
            ".xt",
        )
    )


def classify_runtime_section(section_name: str) -> Optional[str]:
    """Map a linker section name to a MicroTech runtime accounting bucket."""
    section = section_name.strip()
    if not section:
        return None
    if is_ignored_section(section):
        return None

    lower = section.lower()
    if lower in PLACEHOLDER_SECTIONS:
        return None
    if lower in (
        ".noinit",
        ".iram0.text_end",
        ".iram0.data_end",
        ".iram0.bss_end",
        ".dram0.heap_start",
        ".dram0.data_end",
        ".dram0.bss_end",
    ):
        return None

    if lower.startswith(".flash.rodata_noload"):
        return ".rodata_noload"
    if lower.startswith(".flash.text"):
        return ".text"
    if lower.startswith((".flash.rodata", ".flash.appdesc", ".flash.tdata")):
        return ".rodata"
    if lower.startswith((".iram0.text", ".iram0.vectors")):
        return ".iram_text"
    if lower.startswith((".rtc.text", ".rtc.force_fast", ".rtc.force_slow")):
        return ".rtc_text"
    if lower.startswith((".dram0.data", ".rtc.data", ".iram0.data")):
        return ".data"
    if lower.startswith((".dram0.bss", ".rtc.bss", ".rtc_noinit", ".iram0.bss", ".noinit")):
        return ".bss"
    if lower.startswith((".ext_ram.bss", ".ext_ram_noinit")):
        return ".psram_bss"
    if lower.startswith(".ext_ram.data"):
        return ".psram_data"
    if lower.startswith(".rtc_reserved"):
        return ".reserved_ram"
    if lower.startswith(".noload"):
        return None

    # Input section names.
    if lower.startswith((".text", ".literal", ".initcall", ".zinit")):
        return ".text"
    if lower.startswith((".rodata", ".srodata", ".version_str", ".flash_version", ".app_manager_apps")):
        return ".rodata"
    if lower.startswith((".data", ".sdata")):
        return ".data"
    if lower.startswith((".bss", ".sbss")) or lower == "common":
        return ".bss"
    return "other"


def resolve_symbol_section(input_section: str, current_section: Optional[str]) -> Optional[str]:
    """Resolve an input section inside its enclosing output section."""
    classified = classify_runtime_section(input_section)
    if classified is None:
        return None
    if current_section in REGION_CONTAINER_SECTIONS:
        return current_section
    if classified == "other" and current_section not in (None, "other"):
        return current_section
    return classified


def extract_library_object(path: str) -> Tuple[str, str, str, str]:
    """Extract library key, display name, library path, and object name."""
    lib_name = "unknown"
    lib_key = "unknown"
    lib_path = ""
    obj_name = "unknown"

    archive_match = re.search(r"([^\s:]+\.a)\(([^)]+)\)", path, re.IGNORECASE)
    if archive_match:
        lib_path = archive_match.group(1)
        obj_name = archive_match.group(2)
        lib_key, lib_name = normalize_library_identity(lib_path)
        return lib_name, lib_key, lib_path, obj_name

    direct_match = re.search(r"([^\s:]+\.(?:o|obj))(?:\s|$)", path, re.IGNORECASE)
    if direct_match:
        lib_path = direct_match.group(1)
        obj_name = Path(lib_path).name
        lib_name = "[direct link]"
        lib_key = "[direct link]"
        return lib_name, lib_key, lib_path, obj_name

    return lib_name, lib_key, lib_path, obj_name


def extract_symbol_name_from_section(section_name: str) -> Tuple[str, bool, bool]:
    """Derive a display symbol name from a `.text.*`, `.rodata.*`, or `.bss.*` name."""
    if not section_name.startswith("."):
        return section_name, False, False

    parts = section_name.split(".")
    is_function = len(parts) > 1 and parts[1] in ("text", "literal", "initcall", "zinit")
    if len(parts) >= 3:
        symbol_name = ".".join(parts[2:])
        return symbol_name, is_function, symbol_name.startswith("_Z")
    return section_name, is_function, False


def ensure_library(data: ParsedMapData, key: str, name: str, path: str, domain: str) -> Dict[str, Any]:
    if key not in data.libraries:
        data.libraries[key] = {
            "key": key,
            "name": name,
            "path": path,
            "domain": domain,
            "objects": set(),
            "raw_paths": set(),
            "size": 0,
            "sections": defaultdict(int),
            "symbols": [],
        }
    return data.libraries[key]


def add_symbol_to_library(data: ParsedMapData, symbol: SymbolInfo) -> None:
    stats = ensure_library(data, symbol.library_key, symbol.library, symbol.library_path, symbol.domain)
    stats["objects"].add(symbol.object_file)
    if symbol.library_path:
        stats["raw_paths"].add(symbol.library_path)
    stats["size"] += symbol.size
    stats["sections"][symbol.section] += symbol.size
    stats["symbols"].append(symbol)


def build_project_specific_suggestions(data: ParsedMapData) -> List[str]:
    """Build MicroTech-specific optimization notes from library rankings."""
    suggestions: List[str] = []
    top_libs = sorted(data.libraries.values(), key=lambda item: item["size"], reverse=True)[:20]
    if not top_libs:
        return suggestions

    if any(stats.get("domain") == "ui/graphics" for stats in top_libs):
        suggestions.append(
            "UI/graphics libraries rank high; inspect LVGL widget coverage, default-theme "
            "references to every enabled widget class, font sizes, Freetype face configuration, "
            "and PNG/JPEG decoder retention."
        )
    if any(stats.get("domain") == "bluetooth" for stats in top_libs):
        suggestions.append(
            "Bluetooth libraries rank high; inspect the NimBLE role set, GATT profile set, "
            "and BLE controller/PHY library selection."
        )
    if any(stats.get("domain") == "network/tls" for stats in top_libs):
        suggestions.append(
            "Network/TLS libraries rank high; inspect mbedTLS feature trimming, the certificate "
            "bundle, and the Wi-Fi/PHY prebuilt library set."
        )
    if any(stats.get("domain") == "audio/media" for stats in top_libs):
        suggestions.append(
            "Audio/media libraries rank high; inspect codec support, sample assets, and recorder "
            "or player paths retained in the application."
        )
    if any(stats.get("domain") == "storage" for stats in top_libs):
        suggestions.append(
            "Storage libraries rank high; inspect FATFS/LittleFS/SPIFFS backends and NVS "
            "namespace usage."
        )
    if any(stats.get("domain") == "toolchain" for stats in top_libs):
        suggestions.append(
            "Toolchain libraries are visible in the ranking; inspect float printf/scanf, locale, "
            "and heavy math call sites before changing compiler options."
        )
    if merged_literals_size(data) > 0:
        suggestions.append(
            f"Merged literal pools account for {format_size(merged_literals_size(data))}; "
            "reduce log format strings and inspect `.rodata.str*` producers instead of attributing "
            "the pool to the first archive shown by the linker map."
        )

    psram_hotspots = [
        f'{stats["name"]}={format_size(calc_psram(stats["sections"]))}'
        for stats in top_libs
        if calc_psram(stats["sections"]) > 0
    ]
    if psram_hotspots:
        suggestions.append(
            "PSRAM static allocations are present: " + ", ".join(psram_hotspots) +
            "; inspect large external-RAM buffers and their placement."
        )

    if data.discarded_size > 0:
        suggestions.append(
            "The map lists discarded input sections; inspect archives that use whole-archive "
            "linkage when image size headroom becomes tight."
        )
    return suggestions


def build_domain_stats(data: ParsedMapData) -> List[Dict[str, Any]]:
    domains: Dict[str, Dict[str, Any]] = {}
    for stats in data.libraries.values():
        domain = stats.get("domain", "other")
        item = domains.setdefault(
            domain,
            {
                "name": domain,
                "size": 0,
                "library_count": 0,
                "object_count": 0,
                "sections": defaultdict(int),
            },
        )
        item["size"] += stats["size"]
        item["library_count"] += 1
        item["object_count"] += len(stats["objects"])
        for section, size in stats["sections"].items():
            item["sections"][section] += size
    return sorted(domains.values(), key=lambda item: item["size"], reverse=True)


def build_object_stats(data: ParsedMapData) -> List[Dict[str, Any]]:
    objects: Dict[str, Dict[str, Any]] = {}
    for symbol in data.symbols:
        key = f"{symbol.library_key}({symbol.object_file})"
        item = objects.setdefault(
            key,
            {
                "key": key,
                "name": symbol.object_file,
                "library": symbol.library,
                "library_key": symbol.library_key,
                "domain": symbol.domain,
                "size": 0,
                "sections": defaultdict(int),
            },
        )
        item["size"] += symbol.size
        item["sections"][symbol.section] += symbol.size
    return sorted(objects.values(), key=lambda item: item["size"], reverse=True)


def build_input_section_totals(data: ParsedMapData) -> Dict[str, int]:
    totals = new_section_totals()
    for stats in data.libraries.values():
        for section, size in stats["sections"].items():
            totals[section] += size
    return totals


def attributed_size(data: ParsedMapData) -> int:
    return sum(stats["size"] for stats in data.libraries.values())


def merged_literals_size(data: ParsedMapData) -> int:
    return data.libraries.get(MERGED_LITERALS_KEY, {}).get("size", 0)


def unattributed_size(data: ParsedMapData) -> int:
    return max(sum(data.sections.values()) - attributed_size(data), 0)


def make_library_json(stats: Dict[str, Any]) -> Dict[str, Any]:
    sections = dict(stats["sections"])
    return {
        "key": stats["key"],
        "name": stats["name"],
        "path": stats["path"],
        "domain": stats.get("domain", "other"),
        "size": stats["size"],
        "flash_load": calc_flash_load(sections),
        "internal_ram": calc_internal_ram(sections),
        "psram": calc_psram(sections),
        "l2ram": calc_l2ram(sections),
        "object_count": len(stats["objects"]),
        "sections": sections,
        "raw_paths": sorted(stats.get("raw_paths", []))[:20],
    }


def make_object_json(stats: Dict[str, Any]) -> Dict[str, Any]:
    sections = dict(stats["sections"])
    return {
        "key": stats["key"],
        "name": stats["name"],
        "library": stats["library"],
        "library_key": stats["library_key"],
        "domain": stats.get("domain", "other"),
        "size": stats["size"],
        "flash_load": calc_flash_load(sections),
        "internal_ram": calc_internal_ram(sections),
        "psram": calc_psram(sections),
        "l2ram": calc_l2ram(sections),
        "sections": sections,
    }


def make_symbol_json(symbol: SymbolInfo) -> Dict[str, Any]:
    identity = symbol.name or symbol.section_full or "unknown"
    return {
        "key": f"{symbol.library_key}({symbol.object_file})::{symbol.section}::{symbol.section_full}::{identity}",
        "name": symbol.name,
        "address": symbol.address,
        "size": symbol.size,
        "section": symbol.section,
        "section_full": symbol.section_full,
        "library": symbol.library,
        "library_key": symbol.library_key,
        "object_file": symbol.object_file,
        "domain": symbol.domain,
        "flash_load": symbol.flash_load,
        "internal_ram": symbol.internal_ram,
        "psram": symbol.psram,
        "l2ram": symbol.l2ram,
        "is_function": symbol.is_function,
        "is_cpp": symbol.is_cpp,
    }


def _is_hex(token: str) -> bool:
    return bool(re.fullmatch(r"0x[0-9a-fA-F]+", token))


MERGED_LITERALS_KEY = "[merged literals]"
MERGED_LITERALS_NAME = "[merged literals]"


def _new_symbol(section: str, full_section: str, address: int, size: int, path: str) -> Optional[SymbolInfo]:
    if address == 0 or size == 0:
        return None
    if not re.search(r"\.a\([^)]+\)", path) and not re.search(r"\.(?:o|obj)(?:\s|$)", path, re.IGNORECASE):
        return None

    lib_name, lib_key, lib_path, obj_name = extract_library_object(path)
    if lib_name == "unknown":
        return None

    symbol_name, is_function, is_cpp = extract_symbol_name_from_section(full_section)
    if not symbol_name:
        symbol_name = f"[{obj_name}]"
    domain = classify_domain(lib_key, lib_path, lib_name)
    metrics = section_metrics(section, size)
    return SymbolInfo(
        name=symbol_name,
        address=address,
        size=size,
        section=section,
        section_full=full_section,
        library=lib_name,
        library_key=lib_key,
        library_path=lib_path,
        object_file=obj_name,
        domain=domain,
        is_function=is_function,
        is_cpp=is_cpp,
        flash_load=metrics["flash_load"],
        internal_ram=metrics["internal_ram"],
        psram=metrics["psram"],
        l2ram=metrics["l2ram"],
    )


def _split_merged_literals(result: ParsedMapData, symbol: SymbolInfo, size_before_relaxing: int) -> None:
    """Split a linker-merged literal pool from the first input section's owner."""
    if (
        symbol.section != ".rodata"
        or size_before_relaxing < 0
        or symbol.size <= size_before_relaxing
    ):
        return

    merged_size = symbol.size - size_before_relaxing
    stats = result.libraries.get(symbol.library_key)
    if stats is not None:
        stats["size"] -= merged_size
        stats["sections"][symbol.section] -= merged_size
    symbol.size = size_before_relaxing
    symbol.flash_load = size_before_relaxing

    merged = SymbolInfo(
        name=f"merged literals ({symbol.section_full})",
        address=symbol.address + size_before_relaxing,
        size=merged_size,
        section=".rodata",
        section_full=".rodata",
        library=MERGED_LITERALS_NAME,
        library_key=MERGED_LITERALS_KEY,
        library_path="",
        object_file="[merged]",
        domain="shared",
        is_function=False,
        is_cpp=False,
        flash_load=merged_size,
    )
    result.symbols.append(merged)
    add_symbol_to_library(result, merged)


def parse_map_file(filename: str) -> ParsedMapData:
    """Parse a GNU ld map file produced by the ESP-IDF build."""
    result = ParsedMapData()
    result.sections = new_section_totals()
    result.section_addresses = {section: 0 for section in SECTION_ORDER}

    try:
        content = Path(filename).read_text(encoding="utf-8", errors="ignore")
    except OSError as exc:
        raise FileNotFoundError(f"cannot read map file: {exc}") from exc

    result.architecture = detect_architecture(content)

    output_header_re = re.compile(
        r"^(?P<name>\.[A-Za-z0-9_.$]+)"
        r"(?:\s+(?P<addr>0x[0-9a-fA-F]+)\s+(?P<size>0x[0-9a-fA-F]+))?"
        r"(?:\s+load address\s+0x[0-9a-fA-F]+)?\s*$"
    )
    input_section_re = re.compile(
        r"^(?P<name>(?:\.[A-Za-z0-9_.$]+|COMMON))"
        r"\s+(?P<addr>0x[0-9a-fA-F]+)\s+(?P<size>0x[0-9a-fA-F]+)\s+(?P<path>.+)$"
    )

    in_map = False
    in_discarded = False
    current_section: Optional[str] = None
    pending_output: Optional[str] = None
    pending_input: Optional[str] = None
    pending_discard: Optional[str] = None
    last_symbol: Optional[SymbolInfo] = None

    def close_output(name: str, address: int, size: int) -> None:
        nonlocal current_section
        bucket = classify_runtime_section(name)
        current_section = bucket
        if bucket is not None:
            result.sections[bucket] += size
            if result.section_addresses.get(bucket, 0) == 0:
                result.section_addresses[bucket] = address

    def consume_input(full_section: Optional[str], address: int, size: int, path: str) -> None:
        nonlocal last_symbol
        if full_section is None:
            section = current_section
        else:
            section = resolve_symbol_section(full_section, current_section)
        if section is None:
            last_symbol = None
            return
        symbol = _new_symbol(section, full_section or "", address, size, path)
        if symbol is None:
            last_symbol = None
            return
        last_symbol = symbol
        result.symbols.append(symbol)
        add_symbol_to_library(result, symbol)

    for raw_line in content.split("\n"):
        line = raw_line.rstrip()
        stripped = line.strip()
        if not stripped:
            current_section = None
            pending_output = None
            pending_input = None
            pending_discard = None
            last_symbol = None
            continue

        if "Discarded input sections" in line:
            in_discarded = True
            in_map = False
            continue
        if line.startswith("Memory Configuration"):
            in_discarded = False
            in_map = False
            continue
        if line.startswith("Linker script and memory map"):
            in_discarded = False
            in_map = True
            continue
        if line.startswith("Cross Reference Table"):
            break

        if in_discarded:
            match = re.match(
                r"^\s+(?P<name>\.[A-Za-z0-9_.$]+)"
                r"(?:\s+(?:0x[0-9a-fA-F]+)\s+(?P<size>0x[0-9a-fA-F]+))?",
                line,
            )
            if match:
                if match.group("size"):
                    bucket = classify_runtime_section(match.group("name"))
                    if bucket is not None:
                        result.discarded_size += int(match.group("size"), 16)
                else:
                    pending_discard = match.group("name")
                continue
            if pending_discard:
                parts = stripped.split()
                if len(parts) >= 2 and _is_hex(parts[0]) and _is_hex(parts[1]):
                    bucket = classify_runtime_section(pending_discard)
                    if bucket is not None:
                        result.discarded_size += int(parts[1], 16)
                pending_discard = None
            continue

        if not in_map:
            continue

        relax_match = re.match(r"^\s+0x([0-9a-fA-F]+) \(size before relaxing\)\s*$", line)
        if relax_match:
            if last_symbol is not None:
                _split_merged_literals(result, last_symbol, int(relax_match.group(1), 16))
            continue

        if not line[0].isspace():
            match = output_header_re.match(line)
            if match:
                if match.group("addr"):
                    close_output(match.group("name"), int(match.group("addr"), 16), int(match.group("size"), 16))
                    pending_output = None
                else:
                    pending_output = match.group("name")
                    current_section = None
                pending_input = None
                last_symbol = None
            continue

        if pending_output is not None:
            parts = stripped.split()
            if len(parts) >= 2 and _is_hex(parts[0]) and _is_hex(parts[1]):
                close_output(pending_output, int(parts[0], 16), int(parts[1], 16))
                pending_output = None
                continue
            close_output(pending_output, 0, 0)
            pending_output = None

        if stripped.startswith("*fill*"):
            parts = stripped.split()
            if len(parts) >= 3 and current_section is not None:
                try:
                    result.fill_size += int(parts[2], 16)
                except ValueError:
                    pass
            continue
        if stripped.startswith("*"):
            continue

        if len(stripped.split()) == 1 and re.fullmatch(r"\.[A-Za-z0-9_.$]+|COMMON", stripped):
            pending_input = stripped
            last_symbol = None
            continue

        match = input_section_re.match(stripped)
        if match:
            consume_input(match.group("name"), int(match.group("addr"), 16), int(match.group("size"), 16), match.group("path").strip())
            pending_input = None
            continue

        parts = stripped.split()
        if len(parts) == 2 and _is_hex(parts[0]) and not _is_hex(parts[1]):
            if last_symbol is not None:
                symbol_address = int(parts[0], 16)
                if last_symbol.address <= symbol_address < last_symbol.address + last_symbol.size:
                    last_symbol.name = parts[1]
                    last_symbol.is_function = last_symbol.is_function or last_symbol.section in (
                        ".text",
                        ".iram_text",
                        ".rtc_text",
                    )
            continue

        parts = stripped.split(None, 2)
        if len(parts) == 3 and _is_hex(parts[0]) and _is_hex(parts[1]):
            path = parts[2].strip()
            if path.startswith("("):
                pending_input = None
                last_symbol = None
                continue
            consume_input(pending_input, int(parts[0], 16), int(parts[1], 16), path)
            pending_input = None

    return result


def sorted_libraries(data: ParsedMapData) -> List[Dict[str, Any]]:
    return sorted(data.libraries.values(), key=lambda stats: stats["size"], reverse=True)


def generate_markdown_report(data: ParsedMapData, args: argparse.Namespace) -> str:
    """Generate the Markdown size report."""
    lines: List[str] = []
    top_n = args.top
    threshold = args.threshold * 1024
    group_by = getattr(args, "group_by", "library")

    lines.append(f"# {PROJECT_LABEL} Map File Size Report")
    lines.append("")
    lines.append(f"- **Target**: {TARGET_NAME}")
    lines.append(f"- **Architecture**: {data.architecture.value.upper()}")
    if data.discarded_size > 0:
        lines.append(f"- **Discarded input sections**: {format_size(data.discarded_size)}")
    lines.append("")

    flash_total = calc_flash_load(data.sections)
    internal_total = calc_internal_ram(data.sections)
    psram_total = calc_psram(data.sections)
    noload_total = sum_sections(data.sections, NOLOAD_SECTIONS)
    accounted_total = sum(data.sections.values())
    attributed_total = attributed_size(data)
    unattributed_total = unattributed_size(data)

    lines.append("## Overview")
    lines.append("")
    lines.append("```")
    lines.append("Flash Load (bytes stored in the application image)")
    lines.append(f"  .text (flash code):       {format_size(data.sections.get('.text', 0)):>12}")
    lines.append(f"  .iram_text:               {format_size(data.sections.get('.iram_text', 0)):>12}")
    lines.append(f"  .rtc_text:                {format_size(data.sections.get('.rtc_text', 0)):>12}")
    lines.append(f"  .rodata:                  {format_size(data.sections.get('.rodata', 0)):>12}")
    lines.append(f"  .data:                    {format_size(data.sections.get('.data', 0)):>12}")
    lines.append(f"  Flash Load Total:         {format_size(flash_total):>12}")
    lines.append("")
    lines.append("Runtime Memory")
    lines.append(f"  Internal RAM:             {format_size(internal_total):>12}")
    lines.append(f"  PSRAM:                    {format_size(psram_total):>12}")
    lines.append(f"  NOLOAD/Reserved (overlap): {format_size(noload_total):>12}")
    lines.append("")
    lines.append("Attribution")
    lines.append(f"  Library-attributed bytes: {format_size(attributed_total):>12}")
    lines.append(f"  Alignment fill:           {format_size(data.fill_size):>12}")
    lines.append(f"  Unattributed bytes:       {format_size(unattributed_total):>12}")
    lines.append(f"  Accounted section bytes:  {format_size(accounted_total):>12}")
    lines.append("```")
    lines.append("")
    lines.append("## Accounting")
    lines.append("")
    lines.append("- Overview values come from linker output sections and describe image storage and runtime memory.")
    lines.append("- Library, object, and domain tables come from input-section attribution.")
    lines.append("- `.rodata.*` input sections placed inside `.flash.text` remain `.rodata` in library tables.")
    lines.append("- NOLOAD/Reserved overlaps Internal RAM and PSRAM; it is not additional memory.")
    lines.append("")

    libraries = sorted_libraries(data)
    if group_by == "domain":
        lines.append("## By Domain")
        lines.append("")
        lines.append("| Domain | Libraries | Objects | Total | Flash Load | Internal RAM | PSRAM |")
        lines.append("|--------|-----------|---------|-------|------------|--------------|-------|")
        for item in build_domain_stats(data)[:top_n]:
            sections = item["sections"]
            lines.append(
                f"| {item['name']} | {item['library_count']} | {item['object_count']} | {format_size(item['size'])} | "
                f"{format_size(calc_flash_load(sections))} | {format_size(calc_internal_ram(sections))} | "
                f"{format_size(calc_psram(sections))} |"
            )
        lines.append("")
    elif group_by == "object":
        lines.append(f"## Top {top_n} Object Files")
        lines.append("")
        lines.append("| Rank | Object | Library | Domain | Total | Flash Load | Internal RAM | PSRAM |")
        lines.append("|------|--------|---------|--------|-------|------------|--------------|-------|")
        for index, item in enumerate(build_object_stats(data)[:top_n], 1):
            sections = item["sections"]
            lines.append(
                f"| {index} | {item['name']} | {item['library']} | {item['domain']} | {format_size(item['size'])} | "
                f"{format_size(calc_flash_load(sections))} | {format_size(calc_internal_ram(sections))} | "
                f"{format_size(calc_psram(sections))} |"
            )
        lines.append("")
    elif group_by == "library":
        lines.append("## By Library")
        lines.append("")
        lines.append("| Library | Domain | Objects | Total | Flash Load | Internal RAM | PSRAM | .text | .iram_text | .rodata | .data | .bss |")
        lines.append("|---------|--------|---------|-------|------------|--------------|-------|-------|------------|---------|-------|------|")
        for stats in libraries[:top_n]:
            sections = stats["sections"]
            lines.append(
                f"| {stats['name']} | {stats.get('domain', 'other')} | {len(stats['objects'])} | {format_size(stats['size'])} | "
                f"{format_size(calc_flash_load(sections))} | {format_size(calc_internal_ram(sections))} | "
                f"{format_size(calc_psram(sections))} | {format_size(sections['.text'])} | "
                f"{format_size(sections['.iram_text'])} | {format_size(sections['.rodata'])} | "
                f"{format_size(sections['.data'])} | {format_size(sections['.bss'])} |"
            )
        lines.append("")

        limit = min(top_n, 15)
        lines.append(f"### Flash Load Share (Top {limit})")
        lines.append("")
        lines.append("```")
        for stats in libraries[:limit]:
            lib_flash = calc_flash_load(stats["sections"])
            percent = lib_flash / flash_total * 100 if flash_total else 0
            bar = "#" * int(percent / 2)
            lines.append(f"{stats['name']:<44} {bar} {percent:5.1f}% ({format_size(lib_flash)})")
        lines.append("```")
        lines.append("")

        lines.append("## By Domain")
        lines.append("")
        lines.append("| Domain | Libraries | Objects | Total | Flash Load | Internal RAM | PSRAM |")
        lines.append("|--------|-----------|---------|-------|------------|--------------|-------|")
        for item in build_domain_stats(data)[:top_n]:
            sections = item["sections"]
            lines.append(
                f"| {item['name']} | {item['library_count']} | {item['object_count']} | {format_size(item['size'])} | "
                f"{format_size(calc_flash_load(sections))} | {format_size(calc_internal_ram(sections))} | "
                f"{format_size(calc_psram(sections))} |"
            )
        lines.append("")

    sorted_symbols = sorted(data.symbols, key=lambda symbol: symbol.size, reverse=True)
    lines.append(f"## Top {top_n} Symbols")
    lines.append("")
    if args.show_symbols:
        lines.append("| Rank | Symbol | Section | Size | Library | Object |")
        lines.append("|------|--------|---------|------|---------|--------|")
        for index, symbol in enumerate(sorted_symbols[:top_n], 1):
            name = f"`{symbol.name}()`" if symbol.is_function else f"`{symbol.name}`"
            lines.append(
                f"| {index} | {name} | {symbol.section} | {format_size(symbol.size)} | "
                f"{symbol.library} | {symbol.object_file} |"
            )
    else:
        lines.append("| Rank | Section | Size | Library | Object |")
        lines.append("|------|---------|------|---------|--------|")
        for index, symbol in enumerate(sorted_symbols[:top_n], 1):
            lines.append(
                f"| {index} | {symbol.section} | {format_size(symbol.size)} | "
                f"{symbol.library} | {symbol.object_file} |"
            )
    lines.append("")

    lines.append("## Optimization Guidance")
    lines.append("")
    large_symbols = [symbol for symbol in sorted_symbols if symbol.size > threshold]
    if large_symbols:
        lines.append(f"### 1. Large Input Sections (size > {format_size(int(threshold))})")
        lines.append("")
        by_section: Dict[str, List[SymbolInfo]] = defaultdict(list)
        for symbol in large_symbols[:30]:
            by_section[symbol.section].append(symbol)
        for section, symbols in by_section.items():
            lines.append(f"**{section}** ({len(symbols)} entries):")
            lines.append("")
            for symbol in symbols[:10]:
                lines.append(f"- {format_size(symbol.size):<10} {symbol.library} / `{symbol.name}`")
            lines.append("")

    project_suggestions = build_project_specific_suggestions(data)
    if project_suggestions:
        lines.append("### 2. MicroTech Optimization Notes")
        lines.append("")
        for item in project_suggestions:
            lines.append(f"- {item}")
        lines.append("")

    section_index = 3 if project_suggestions else 2
    lines.append(f"### {section_index}. Build and Link Checks")
    lines.append("")
    lines.append("- Confirm the map file and ELF come from the same build.")
    lines.append("- `sdkconfig.defaults` sets `CONFIG_COMPILER_OPTIMIZATION_PERF`; size-oriented optimization is a project configuration decision.")
    lines.append("- ESP-IDF enables `-ffunction-sections`, `-fdata-sections`, and `--gc-sections` by default; inspect components that opt into whole-archive linkage.")
    lines.append("- Keep test, demo, and console components out of release images unless they are required.")
    lines.append("")

    if data.architecture == Architecture.XTENSA:
        lines.append(f"### {section_index + 1}. Xtensa Notes")
        lines.append("")
        lines.append("- Inspect literal-pool placement and IRAM-only functions for avoidable `.iram_text` growth.")
        lines.append("- Check whether performance-critical code is placed in IRAM without a measured need.")
        lines.append("")

    return "\n".join(lines)


def generate_json_report(data: ParsedMapData, args: argparse.Namespace) -> str:
    """Generate the normalized JSON report used by comparison workflows."""
    flash_total = calc_flash_load(data.sections)
    internal_total = calc_internal_ram(data.sections)
    psram_total = calc_psram(data.sections)
    sorted_symbols = sorted(data.symbols, key=lambda symbol: symbol.size, reverse=True)
    symbol_limit = max(args.symbol_limit, 0)
    top_symbols = sorted_symbols[:symbol_limit]
    output_sections = {name: size for name, size in data.sections.items() if size > 0}
    input_sections = {name: size for name, size in build_input_section_totals(data).items() if size > 0}
    noload_sections = {
        name: data.sections.get(name, 0)
        for name in NOLOAD_SECTIONS
        if data.sections.get(name, 0) > 0
    }

    report = {
        "schema_version": SCHEMA_VERSION,
        "tool_version": TOOL_VERSION,
        "project": PROJECT_LABEL,
        "target": TARGET_NAME,
        "generated_at": utc_now_iso(),
        "source_map": str(Path(args.map_file).resolve()),
        "architecture": data.architecture.value,
        "discarded_size": data.discarded_size,
        "summary": {
            "flash": flash_total,
            "flash_load": flash_total,
            "ram": internal_total,
            "internal_ram": internal_total,
            "psram": psram_total,
            "l2ram": 0,
            "noload": sum_sections(data.sections, NOLOAD_SECTIONS),
            "noload_sections": noload_sections,
            "accounted_sections": sum(data.sections.values()),
            "attributed_bytes": attributed_size(data),
            "merged_literals": merged_literals_size(data),
            "alignment_fill": data.fill_size,
            "unattributed": unattributed_size(data),
            "sections": output_sections,
            "output_sections": output_sections,
            "input_sections": input_sections,
        },
        "libraries": [make_library_json(stats) for stats in sorted_libraries(data)],
        "domains": [
            {
                "name": item["name"],
                "library_count": item["library_count"],
                "object_count": item["object_count"],
                "size": item["size"],
                "flash_load": calc_flash_load(item["sections"]),
                "internal_ram": calc_internal_ram(item["sections"]),
                "psram": calc_psram(item["sections"]),
                "l2ram": 0,
                "sections": dict(item["sections"]),
            }
            for item in build_domain_stats(data)
        ],
        "objects": [make_object_json(item) for item in build_object_stats(data)],
        "top_symbols": [make_symbol_json(symbol) for symbol in top_symbols],
    }
    if args.all_symbols:
        report["symbols"] = [make_symbol_json(symbol) for symbol in sorted_symbols]
    return json.dumps(report, indent=2, ensure_ascii=False)


def generate_csv_report(data: ParsedMapData) -> str:
    """Generate one CSV row per input section."""
    output = StringIO()
    writer = csv.writer(output)
    writer.writerow(
        [
            "Symbol Name",
            "Size",
            "Section",
            "Library",
            "Library Key",
            "Object File",
            "Domain",
            "Flash Load",
            "Internal RAM",
            "PSRAM",
            "Is Function",
            "Is C++",
        ]
    )
    for symbol in sorted(data.symbols, key=lambda item: item.size, reverse=True):
        writer.writerow(
            [
                symbol.name,
                symbol.size,
                symbol.section,
                symbol.library,
                symbol.library_key,
                symbol.object_file,
                symbol.domain,
                symbol.flash_load,
                symbol.internal_ram,
                symbol.psram,
                symbol.is_function,
                symbol.is_cpp,
            ]
        )
    return output.getvalue()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Analyze an ESP-IDF linker map and report MicroTech firmware code size.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s build/microtech.map
  %(prog)s build/microtech.map --show-symbols --top 30
  %(prog)s build/microtech.map --group-by domain
  %(prog)s build/microtech.map --format json --all-symbols
        """,
    )
    parser.add_argument("map_file", help="Path to the linker-generated .map file")
    parser.add_argument("--top", type=int, default=20, help="Show Top N entries (default: 20)")
    parser.add_argument("--threshold", type=float, default=1.0, help="Large input-section threshold in KB (default: 1.0)")
    parser.add_argument("--output", "-o", help="Write the report to this file")
    parser.add_argument("--format", "-f", choices=["markdown", "json", "csv"], default="markdown")
    parser.add_argument("--show-symbols", "-s", action="store_true", help="Show symbol names in the symbol table")
    parser.add_argument("--group-by", choices=["library", "domain", "object"], default="library")
    parser.add_argument("--symbol-limit", type=int, default=100, help="Symbol count in JSON top_symbols")
    parser.add_argument("--all-symbols", action="store_true", help="Include every parsed symbol in JSON output")
    parser.add_argument("--fail-on-threshold", type=float, default=None, help="Fail if Flash Load exceeds this value in KB")
    args = parser.parse_args()

    try:
        print(f"Parsing map file: {args.map_file}", file=sys.stderr)
        data = parse_map_file(args.map_file)
        if sum(data.sections.values()) == 0:
            print("Error: no runtime sections parsed from the map file", file=sys.stderr)
            return 2
        print(f"Parsing complete: arch={data.architecture.value}, symbols={len(data.symbols)}", file=sys.stderr)
    except FileNotFoundError:
        print(f"Error: file not found - {args.map_file}", file=sys.stderr)
        return 2
    except Exception as exc:  # pragma: no cover - CLI boundary
        print(f"Error: {exc}", file=sys.stderr)
        return 2

    if args.format == "json":
        output = generate_json_report(data, args)
    elif args.format == "csv":
        output = generate_csv_report(data)
    else:
        output = generate_markdown_report(data, args)

    if args.output:
        try:
            Path(args.output).write_text(output, encoding="utf-8")
            print(f"Report generated: {args.output}", file=sys.stderr)
        except OSError as exc:
            print(f"Error: cannot write {args.output}: {exc}", file=sys.stderr)
            return 2
    else:
        print(output)

    if args.fail_on_threshold is not None:
        flash_total = calc_flash_load(data.sections)
        threshold_bytes = int(args.fail_on_threshold * 1024)
        if flash_total > threshold_bytes:
            print(f"Flash Load {format_size(flash_total)} exceeds threshold {format_size(threshold_bytes)}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
