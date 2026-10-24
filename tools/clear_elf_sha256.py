#!/usr/bin/env python3
"""Zero the app_elf_sha256 field in an ESP32 app image, in place.

esptool's elf2image stamps a SHA-256 of the whole ELF into the app
descriptor. The ELF carries debug info and toolchain-library metadata that
differ between the macOS and Linux builds of the same toolchain, so that
stamp was the only thing keeping otherwise-identical images from matching
across hosts. Zeroing it (and fixing up the image checksum and appended
SHA-256 that cover it) makes the image depend only on what gets flashed.

Idempotent: an image whose field is already zero is left byte-identical.
"""

import hashlib
import struct
import sys

IMAGE_MAGIC = 0xE9
APP_DESC_OFFSET = 0x20          # after 24-byte image header + 8-byte segment header
APP_DESC_MAGIC = 0xABCD5432
ELF_SHA_OFFSET = APP_DESC_OFFSET + 0x90
SHA_LEN = 32


def xor_bytes(data):
    x = 0
    for b in data:
        x ^= b
    return x


def main():
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} IMAGE.bin")
    path = sys.argv[1]
    img = bytearray(open(path, "rb").read())

    if img[0] != IMAGE_MAGIC:
        sys.exit(f"{path}: not an ESP image")
    if img[23] != 1:
        sys.exit(f"{path}: no appended SHA-256 (hash_appended=0)")
    (desc_magic,) = struct.unpack_from("<I", img, APP_DESC_OFFSET)
    if desc_magic != APP_DESC_MAGIC:
        sys.exit(f"{path}: app descriptor not at {APP_DESC_OFFSET:#x}")
    if hashlib.sha256(img[:-SHA_LEN]).digest() != img[-SHA_LEN:]:
        sys.exit(f"{path}: appended SHA-256 does not verify; refusing to patch")

    old = img[ELF_SHA_OFFSET:ELF_SHA_OFFSET + SHA_LEN]
    new = bytes(SHA_LEN)
    if old == new:
        return

    # The checksum byte (last byte before the appended digest) is 0xEF XORed
    # with every segment data byte, so patch it by the XOR of what changed.
    img[ELF_SHA_OFFSET:ELF_SHA_OFFSET + SHA_LEN] = new
    img[-SHA_LEN - 1] ^= xor_bytes(old) ^ xor_bytes(new)
    img[-SHA_LEN:] = hashlib.sha256(img[:-SHA_LEN]).digest()

    with open(path, "wb") as f:
        f.write(img)


if __name__ == "__main__":
    main()
