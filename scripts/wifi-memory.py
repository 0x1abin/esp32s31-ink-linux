#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check the linked CPU0 firmware, including its IDF heap reservations."""
import hashlib
import struct


SHM = (0x2F020000, 0x2F02D000)
NATIVE_DMA = (0x2F030000, 0x2F070000)
ROM_STACK = (0x2F07E200, 0x2F07E600)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def check_firmware_memory(path):
    data = path.read_bytes()
    require(len(data) >= 52 and data[:7] == b"\x7fELF\x01\x01\x01",
            "Firmware must be an ELF32 little-endian image")
    header = struct.unpack_from("<16sHHIIIIIHHHHHH", data)
    (_, kind, machine, version, entry, phoff, shoff, _, ehsize,
     phsize, phnum, shsize, shnum, names_index) = header
    require((kind, machine, version, ehsize) == (2, 243, 1, 52),
            "Firmware must be a RISC-V ELF32 executable")
    require(phsize == 32 and phnum and phoff >= 52 and phoff + phnum * phsize <= len(data) and
            shsize == 40 and shnum and shoff >= 52 and shoff + shnum * shsize <= len(data) and
            names_index < shnum, "Invalid firmware ELF header tables")
    sections = [struct.unpack_from("<IIIIIIIIII", data, shoff + index * shsize)
                for index in range(shnum)]

    def contents(section):
        _, section_type, _, _, offset, size, *_ = section
        require(section_type != 8 and offset + size <= len(data),
                "Truncated firmware ELF section")
        return data[offset:offset + size]

    def string(table, offset):
        require(offset < len(table) and b"\0" in table[offset:], "Invalid firmware ELF string")
        return table[offset:table.index(0, offset)].decode("ascii")

    names = contents(sections[names_index])
    allowed = ((0x2E000000, 0x2E008000), (0x2F000000, SHM[0]),
               (0x40000000, 0x40400000))

    def check_range(address, size, label):
        end = address + size
        require(end <= 0x100000000 and
                any(start <= address < limit and end <= limit for start, limit in allowed),
                "Firmware " + label + " overlaps shared/display/ROM reservations or leaves CPU0 memory")

    loads = []
    for index in range(phnum):
        ptype, offset, virtual, physical, filesz, memsz, flags, _ = struct.unpack_from(
            "<IIIIIIII", data, phoff + index * phsize)
        if ptype != 1:
            continue
        require(virtual == physical and filesz <= memsz and offset + filesz <= len(data),
                "Invalid firmware PT_LOAD mapping")
        if memsz:
            check_range(physical, memsz, "PT_LOAD")
            loads.append({"address": physical, "size": memsz, "file_size": filesz, "flags": flags})
    require(any(load["address"] <= entry < load["address"] + load["size"] for load in loads),
            "Firmware entry point is outside PT_LOAD memory")
    sram_sections = []
    for section in sections:
        name, section_type, flags, address, _, size, *_ = section
        if not flags & 2 or not size:
            continue
        label = string(names, name)
        check_range(address, size, "section " + label)
        if section_type != 8:
            contents(section)
        require(any(load["address"] <= address and address + size <= load["address"] + load["size"]
                    for load in loads), "Allocated firmware section is outside PT_LOAD: " + label)
        if 0x2F000000 <= address < SHM[0]:
            sram_sections.append({"name": label, "address": address, "size": size})
    require(sram_sections, "Firmware ELF contains no allocated SRAM sections")

    expected = {"reserved_region_hosted_shm": SHM,
                "reserved_region_linux_dma_pool": NATIVE_DMA,
                "reserved_region_rom_hart1_stack": ROM_STACK}
    reservations = {}
    for section in sections:
        if section[1] != 2:
            continue
        require(section[6] < shnum and section[9] == 16 and section[5] % 16 == 0,
                "Invalid firmware ELF symbol table")
        strings = contents(sections[section[6]])
        for name, value, size, _, _, index in struct.iter_unpack("<IIIBBH", contents(section)):
            label = string(strings, name)
            if label not in expected:
                continue
            key = label.removeprefix("reserved_region_")
            require(key not in reservations and size == 8 and index < shnum,
                    "Invalid firmware reservation symbol: " + label)
            owner = sections[index]
            offset = value - owner[3]
            body = contents(owner)
            require(0 <= offset and offset + 8 <= len(body), "Invalid reservation location: " + label)
            start, end = struct.unpack_from("<II", body, offset)
            require((start, end) == expected[label], "Incorrect firmware heap reservation: " + label)
            reservations[key] = {"address": start, "size": end - start}
    require(set(reservations) == {name.removeprefix("reserved_region_") for name in expected},
            "Missing firmware heap reservation symbols")
    return {"elf_sha256": hashlib.sha256(data).hexdigest(), "entry": entry,
            "sram_static_end": max(section["address"] + section["size"] for section in sram_sections),
            "sram_sections": sram_sections, "load_segments": loads, "heap_reservations": reservations}
