#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build Wi-Fi component images and a full factory image including empty data."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parent.parent
IMAGE = "metalio-eink4-plus-wifi-flash.bin"
DATA_IMAGE = "data-empty.jffs2"
FULL_IMAGE = "metalio-eink4-plus-wifi-full-flash.bin"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def image_names(layout):
    return layout.get("image", IMAGE), layout.get("full_image", FULL_IMAGE)


def read_layout(path):
    layout = json.loads(path.read_text())
    require(layout["data_offset"] + layout["data_size"] == layout["flash_size"],
            "Data partition must end at the flash boundary")
    require(layout["data_size"] % layout["erase_size"] == 0,
            "Data partition must contain complete erase blocks")
    for name in image_names(layout):
        require(Path(name).name == name and name.endswith(".bin"), "Invalid merged image filename")
    previous = 0
    names = set()
    for slot in layout["slots"]:
        require(Path(slot["file"]).name == slot["file"] and slot["file"] not in names,
                "Invalid or duplicate slot filename")
        require(previous <= slot["offset"] < slot["limit"] <= layout["data_offset"],
                "Overlapping or invalid firmware slots")
        require(slot["offset"] % 4096 == 0 and slot["limit"] % 4096 == 0,
                "Firmware slots must align to flash erase sectors")
        previous = slot["limit"]
        names.add(slot["file"])
    output_names = image_names(layout)
    require(len(set(output_names)) == 2 and not set(output_names) & names,
            "Merged image filenames must be distinct from component images")
    return layout


def validate_partitions(data, layout):
    entries = {}
    checksum = False
    for position in range(0, len(data), 32):
        record = data[position:position + 32]
        require(len(record) == 32, "Truncated IDF partition table")
        if record[:2] == b"\xaa\x50":
            require(not checksum, "Partition entry follows table checksum")
            _, kind, subtype, offset, size, label, flags = struct.unpack("<2sBBII16sI", record)
            name = label.rstrip(b"\0").decode("ascii")
            require(name not in entries and flags == 0, "Duplicate or unsupported IDF partition")
            entries[name] = (kind, subtype, offset, size)
        elif record[:16] == b"\xeb\xeb" + b"\xff" * 14:
            require(not checksum and hashlib.md5(data[:position]).digest() == record[16:],
                    "Invalid IDF partition table checksum")
            checksum = True
        else:
            require(checksum and data[position:] == b"\xff" * (len(data) - position),
                    "Invalid IDF partition table padding")
            break
    require(checksum, "Missing IDF partition table checksum")
    expected = {"nvs": (1, 2, 0x9000, 0x6000), "phy_init": (1, 1, 0xF000, 0x1000)}
    dtb = [slot["file"] for slot in layout["slots"] if slot["file"].endswith(".dtb")]
    require(len(dtb) == 1, "Exactly one device tree slot is required")
    for name, filename in (("factory", "esp32s31-linux-companion.bin"), ("opensbi", "fw_jump.bin"),
                           ("dtb", dtb[0]), ("linux", "xipImage"),
                           ("rootfs", "rootfs.cramfs")):
        slot = next(slot for slot in layout["slots"] if slot["file"] == filename)
        expected[name] = (0, 0, slot["offset"], slot["limit"] - slot["offset"]) if name == "factory" else (
            1, 0x40, slot["offset"], slot["limit"] - slot["offset"])
    expected["data"] = (1, 0x40, layout["data_offset"], layout["data_size"])
    require(entries == expected, "IDF partition table does not match the Wi-Fi flash layout")


def collect_components(images, layout):
    components = {}
    for slot in layout["slots"]:
        data = (images / slot["file"]).read_bytes()
        require(data and slot["offset"] + len(data) <= slot["limit"],
                "Image exceeds flash slot: " + slot["file"])
        components[slot["file"]] = data
    validate_partitions(components["wifi-partition-table.bin"], layout)
    return components


def merged_image(components, layout):
    end = max(slot["offset"] + len(components[slot["file"]]) for slot in layout["slots"])
    # Round only to a 4 KiB erase sector, never into the separate data partition.
    end = (end + 4095) & ~4095
    require(end <= layout["data_offset"], "Firmware image overlaps persistent data")
    result = bytearray(b"\xff" * end)
    for slot in layout["slots"]:
        data = components[slot["file"]]
        result[slot["offset"]:slot["offset"] + len(data)] = data
    return bytes(result)


def full_image(image, data, layout):
    require(len(image) <= layout["data_offset"], "Firmware image overlaps persistent data")
    require(len(data) == layout["data_size"], "Factory data image has an unexpected size")
    return image.ljust(layout["data_offset"], b"\xff") + data


def empty_data_image(host, layout):
    tool = next((host / directory / "mkfs.jffs2" for directory in ("sbin", "bin")
                 if (host / directory / "mkfs.jffs2").is_file()), None)
    require(tool is not None, "Missing host mkfs.jffs2; enable BR2_PACKAGE_HOST_MTD")
    with tempfile.TemporaryDirectory(prefix="metalio-empty-data-") as temporary:
        directory = Path(temporary)
        tree = directory / "root"
        tree.mkdir(mode=0o755)
        (tree / "wifi").mkdir(mode=0o700)
        marker = tree / ".metalio-data-v1"
        marker.write_text("metalio-data-v1\n")
        marker.chmod(0o644)
        output = directory / DATA_IMAGE
        subprocess.run([str(tool), "--little-endian", "--squash-uids", "--faketime",
                        "--eraseblock", str(layout["erase_size"]), "--pad=" + str(layout["data_size"]),
                        "--root", str(tree), "--output", str(output)], check=True)
        result = output.read_bytes()
    require(len(result) == layout["data_size"], "Factory data image has an unexpected size")
    return result


def make_images(images, layout_path, host):
    layout = read_layout(layout_path)
    components = collect_components(images, layout)
    main = merged_image(components, layout)
    factory = empty_data_image(host, layout)
    full = full_image(main, factory, layout)
    image_name, full_name = image_names(layout)
    for name, data in ((image_name, main), (DATA_IMAGE, factory), (full_name, full)):
        with tempfile.NamedTemporaryFile(prefix="." + name, dir=images, delete=False) as stream:
            temporary = Path(stream.name)
            try:
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
                os.replace(temporary, images / name)
            finally:
                temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--images-dir", type=Path, required=True)
    parser.add_argument("--host-dir", type=Path, required=True)
    parser.add_argument("--layout", type=Path, default=ROOT / "board/metalio/eink4-plus/wifi/layout.json")
    args = parser.parse_args()
    try:
        make_images(args.images_dir, args.layout, args.host_dir)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    main()
