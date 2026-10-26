#!/usr/bin/env python3
"""Package built ESP32-S3 firmware targets into .zip archives for flasher.meshtastic.org."""

import argparse
import hashlib
import json
import sys
import zipfile
from pathlib import Path

# Target ESP32-S3 boards to package
DEFAULT_BOARDS = [
    "heltec-v3",
    "heltec-v4",
    "seeed-xiao-s3",
]


def detect_base_dir() -> Path:
    """Detect base mesh directory (C:\\portable\\mesh on build PC, C:\\mesh locally)."""
    portable_path = Path(r"C:\portable\mesh")
    if (portable_path / "meshtastic_firmware_obn" / ".pio" / "build").is_dir():
        return portable_path

    # Resolve relative to this script: <base_dir>/meshtastic_firmware_obn/bin/package_esp32s3_fw.py
    script_Path = Path(__file__).resolve()
    if script_Path.parent.name == "bin":
        candidate = script_Path.parent.parent.parent
    else:
        candidate = script_Path.parent.parent

    if (candidate / "meshtastic_firmware_obn" / ".pio" / "build").is_dir():
        return candidate

    local_path = Path(r"C:\mesh")
    if (local_path / "meshtastic_firmware_obn" / ".pio" / "build").is_dir():
        return local_path

    return portable_path


def md5_of_file(path: Path) -> str:
    h = hashlib.md5()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def package_board(board: str, build_root: Path, bleota_path: Path, out_dir: Path) -> Path:
    board_dir = build_root / board
    if not board_dir.is_dir():
        raise FileNotFoundError(f"Build directory not found for '{board}': {board_dir}")

    # Find firmware-<board>-<version>.mt.json
    manifests = sorted(board_dir.glob(f"firmware-{board}-*.mt.json"))
    if not manifests:
        raise FileNotFoundError(
            f"Manifest firmware-{board}-*.mt.json not found in {board_dir}. "
            f"Make sure the target was built cleanly."
        )

    manifest_path = manifests[-1]
    manifest_data = json.loads(manifest_path.read_text(encoding="utf-8"))

    mcu = manifest_data.get("mcu", "")
    if mcu != "esp32s3":
        raise ValueError(f"Target '{board}' has mcu='{mcu}', expected 'esp32s3'")

    version = manifest_data.get("version")
    if not version:
        prefix = f"firmware-{board}-"
        suffix = ".mt.json"
        version = manifest_path.name[len(prefix) : -len(suffix)]

    base_name = f"firmware-{board}-{version}"
    factory_bin = board_dir / f"{base_name}.factory.bin"
    app_bin = board_dir / f"{base_name}.bin"
    littlefs_bin = board_dir / f"littlefs-{board}-{version}.bin"

    for req_file in (factory_bin, app_bin, littlefs_bin, bleota_path):
        if not req_file.is_file():
            raise FileNotFoundError(f"Required file missing: {req_file}")

    # IMPORTANT for flasher.meshtastic.org when uploading a custom .zip file:
    # Web-flasher uses the legacy regex extractor on local .zip uploads (hasManifest = false):
    #   1) Clean Install (Full Erase, offset 0x00):
    #      Matches `firmware-<board>-.+.bin` where `filename.endsWith('update.bin') == false`.
    #      This MUST match the combined factory image (`.factory.bin` with bootloader at 0x00).
    #      If the bare 0x10000 `app0` image were stored as `firmware-<board>-<ver>.bin`,
    #      the regex could match it and flash bare app0 to 0x00, breaking boot!
    #   2) Update Only (no erase, offset 0x10000):
    #      Matches `firmware-<board>-.+-update.bin` where `filename.endsWith('update.bin') == true`.
    #      Therefore the bare 0x10000 `app0` binary (`firmware-<board>-<ver>.bin`) MUST be
    #      stored inside the .zip as `firmware-<board>-<ver>-update.bin`.
    update_bin_arcname = f"{base_name}-update.bin"

    # Update the in-zip manifest metadata to match the archived filenames and include bleota-s3.bin
    bleota_md5 = md5_of_file(bleota_path)
    bleota_size = bleota_path.stat().st_size
    updated_files = []
    for entry in manifest_data.get("files", []):
        name = entry.get("name", "")
        if name.endswith(".elf"):
            continue
        if name == app_bin.name:
            entry = dict(entry)
            entry["name"] = update_bin_arcname
        updated_files.append(entry)

    if not any(f.get("name") == "bleota-s3.bin" for f in updated_files):
        updated_files.append(
            {
                "name": "bleota-s3.bin",
                "md5": bleota_md5,
                "bytes": bleota_size,
                "part_name": "app1",
            }
        )
    manifest_data["files"] = updated_files

    out_dir.mkdir(parents=True, exist_ok=True)
    zip_path = out_dir / f"{base_name}.zip"

    with zipfile.ZipFile(zip_path, mode="w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        # 1. Full factory image (0x00: bootloader + partitions + app0)
        zf.write(factory_bin, arcname=factory_bin.name)
        # 2. Update image (0x10000: app0 only, named *-update.bin for flasher.meshtastic.org)
        zf.write(app_bin, arcname=update_bin_arcname)
        # 3. LittleFS filesystem image (spiffs partition)
        zf.write(littlefs_bin, arcname=littlefs_bin.name)
        # 4. BLE OTA image (app1 partition)
        zf.write(bleota_path, arcname="bleota-s3.bin")
        # 5. Target metadata manifest (.mt.json)
        zf.writestr(manifest_path.name, json.dumps(manifest_data, indent=2) + "\n")

    return zip_path


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Package ESP32-S3 Meshtastic builds into flasher.meshtastic.org .zip archives."
    )
    parser.add_argument(
        "--base-dir",
        type=Path,
        default=None,
        help="Base mesh directory (default: auto-detect C:\\portable\\mesh or C:\\mesh)",
    )
    parser.add_argument(
        "--boards",
        nargs="+",
        default=DEFAULT_BOARDS,
        help=f"Target boards to package (default: {' '.join(DEFAULT_BOARDS)})",
    )
    args = parser.parse_args()

    base_dir = args.base_dir.resolve() if args.base_dir else detect_base_dir()
    build_root = base_dir / "meshtastic_firmware_obn" / ".pio" / "build"
    bleota_path = base_dir / "bleota-s3.bin"
    out_dir = base_dir / "fw"

    print(f"Base directory : {base_dir}")
    print(f"Build directory: {build_root}")
    print(f"BLE OTA binary : {bleota_path}")
    print(f"Output folder  : {out_dir}")
    print("-" * 60)

    if not build_root.is_dir():
        print(f"ERROR: Build root does not exist: {build_root}", file=sys.stderr)
        return 1

    if not bleota_path.is_file():
        print(f"ERROR: bleota-s3.bin not found at: {bleota_path}", file=sys.stderr)
        return 1

    errors = 0
    for board in args.boards:
        try:
            zip_path = package_board(board, build_root, bleota_path, out_dir)
            size_mb = zip_path.stat().st_size / (1024 * 1024)
            print(f"[OK] {board:<15} -> {zip_path} ({size_mb:.2f} MB)")
        except Exception as exc:
            errors += 1
            print(f"[ERROR] {board:<15} -> {exc}", file=sys.stderr)

    print("-" * 60)
    if errors:
        print(f"Finished with {errors} error(s).", file=sys.stderr)
        return 1

    print(f"Successfully packaged {len(args.boards)} ESP32-S3 target(s) into {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

