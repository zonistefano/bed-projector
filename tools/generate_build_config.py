"""Read the local .env as data and generate firmware defaults without printing secrets."""

from __future__ import annotations

import hashlib
from pathlib import Path
import re
import sys


KEYS = {"WIFI_SSID", "WIFI_PASSWORD", "API_TOKEN"}
TOKEN_PATTERN = re.compile(r"[0-9a-f]{32}\Z")


def parse_env(contents: str) -> dict[str, str]:
    """Parse literal KEY=VALUE lines; never run shell commands or expand variables."""
    values: dict[str, str] = {}
    for number, original in enumerate(contents.splitlines(), 1):
        line = original.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[7:].lstrip()
        if "=" not in line:
            continue  # Legacy entries outside this firmware's configuration.
        key, raw = line.split("=", 1)
        key = key.strip()
        if key not in KEYS:
            continue
        if key in values:
            raise ValueError(f"Duplicate {key} in .env (line {number})")
        raw = raw.strip()
        if raw.startswith(("'", '"')):
            if len(raw) < 2 or raw[-1] != raw[0]:
                raise ValueError(f"Unclosed quote for {key} in .env (line {number})")
            raw = raw[1:-1]
        values[key] = raw
    return values


def validate(values: dict[str, str]) -> None:
    ssid = values.get("WIFI_SSID", "")
    password = values.get("WIFI_PASSWORD", "")
    token = values.get("API_TOKEN", "")
    for key, value in (("WIFI_SSID", ssid), ("WIFI_PASSWORD", password), ("API_TOKEN", token)):
        if any(ord(character) < 32 or ord(character) == 127 for character in value):
            raise ValueError(f"{key} contains a control character")
    if len(ssid.encode("utf-8")) > 32:
        raise ValueError("WIFI_SSID must be at most 32 UTF-8 bytes")
    if password and not ssid:
        raise ValueError("WIFI_PASSWORD requires WIFI_SSID")
    if password and not 8 <= len(password.encode("utf-8")) <= 63:
        raise ValueError("WIFI_PASSWORD must be 8-63 UTF-8 bytes, or empty for an open network")
    if ssid and not token:
        raise ValueError("API_TOKEN is required when WIFI_SSID is set")
    if token and not TOKEN_PATTERN.fullmatch(token):
        raise ValueError("API_TOKEN must be exactly 32 lowercase hexadecimal characters")


def c_string(value: str) -> str:
    """Encode every UTF-8 byte as a fixed-width C octal escape."""
    return '"' + "".join(f"\\{byte:03o}" for byte in value.encode("utf-8")) + '"'


def render(values: dict[str, str]) -> str:
    validate(values)
    ssid = values.get("WIFI_SSID", "")
    password = values.get("WIFI_PASSWORD", "")
    token = values.get("API_TOKEN", "")
    fingerprint = hashlib.sha256((ssid + "\0" + password).encode("utf-8")).hexdigest()[:16]
    return (
        "/* Generated from the ignored local .env; do not commit this file. */\n"
        "#pragma once\n"
        f"#define BED_BUILD_WIFI_SSID {c_string(ssid)}\n"
        f"#define BED_BUILD_WIFI_PASSWORD {c_string(password)}\n"
        f"#define BED_BUILD_WIFI_FINGERPRINT \"{fingerprint}\"\n"
        f"#define BED_BUILD_API_TOKEN {c_string(token)}\n"
    )


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: generate_build_config.py ENV_FILE OUTPUT_HEADER", file=sys.stderr)
        return 2
    source, target = map(Path, sys.argv[1:])
    try:
        values = parse_env(source.read_text(encoding="utf-8") if source.exists() else "")
        output = render(values)
    except (OSError, UnicodeError, ValueError) as error:
        print(f"Build configuration error: {error}", file=sys.stderr)
        return 1
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.exists() or target.read_text(encoding="utf-8") != output:
        target.write_text(output, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
