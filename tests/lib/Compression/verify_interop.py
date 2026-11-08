"""Independent ZIP interoperability and malformed-input checks (optional Python oracle)."""

import io
import struct
import subprocess
import sys
import zipfile


def invoke(executable, operation, *arguments, data=b""):
    result = subprocess.run(
        [executable, operation, *(str(value) for value in arguments)],
        input=data, capture_output=True, check=True, timeout=30,
    )
    status, separator, body = result.stdout.partition(b"\n")
    assert separator, result
    return int(status), body


def make_zip(entries, method=zipfile.ZIP_DEFLATED):
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", compression=method) as archive:
        for name, contents in entries:
            archive.writestr(name, contents)
    return output.getvalue()


def main():
    executable = sys.argv[1]
    entries = [("binary.bin", bytes(range(256)) * 16), ("empty", b""),
               ("nested/\u96ea.txt", b"payload")]
    for method in (0, 6):
        status, data = invoke(executable, "create", method)
        assert status == 0, status
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            assert archive.namelist() == [name for name, _ in entries]
            for name, expected in entries:
                assert archive.read(name) == expected
            assert archive.testzip() is None
        if method == 6:
            assert len(data) < 2048, len(data)

    for method in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
        data = make_zip(entries, method)
        status, names = invoke(executable, "list", 3, 100, data=data)
        assert status == 0 and names.decode("utf-8").splitlines() == [name for name, _ in entries]
        for index, (_, expected) in enumerate(entries):
            assert invoke(executable, "extract", index, len(expected), data=data) == (0, expected)
        assert invoke(executable, "list", 2, 100, data=data) == (4, b"")
        assert invoke(executable, "list", 3, 1, data=data) == (4, b"")
        assert invoke(executable, "extract", 0, 4095, data=data) == (4, b"")
        assert invoke(executable, "extract", 3, 100, data=data) == (5, b"")
        assert invoke(executable, "extract", -1, 100, data=data) == (1, b"")
        assert invoke(executable, "extract", 0, -1, data=data) == (1, b"")

    long_name = "x" * 800
    long_zip = make_zip([(long_name, b"long name")])
    assert invoke(executable, "list", 1, 800, data=long_zip) == (0, long_name.encode() + b"\n")
    assert invoke(executable, "list", 1, 799, data=long_zip) == (4, b"")
    for invalid in (b"", b"PK", b"not an archive" * 10, long_zip[:-10]):
        assert invoke(executable, "list", 10, 1000, data=invalid)[0] == 2

    damaged = bytearray(make_zip([("file", b"checksum")], zipfile.ZIP_STORED))
    name_size, extra_size = struct.unpack_from("<HH", damaged, 26)
    damaged[30 + name_size + extra_size] ^= 0xFF
    assert invoke(executable, "extract", 0, 100, data=damaged) == (2, b"")
    for mode in ("crc", "header"):
        damaged = bytearray(make_zip([("empty", b"")], zipfile.ZIP_STORED))
        central = damaged.index(b"PK\x01\x02")
        if mode == "crc":
            struct.pack_into("<I", damaged, 14, 1)
            struct.pack_into("<I", damaged, central + 16, 1)
        else:
            damaged[0] = 0
        assert invoke(executable, "extract", 0, 0, data=damaged) == (2, b"")
    for flags, expected in ((0, 3), (0x800, 2)):
        damaged = bytearray(make_zip([("file", b"payload")]))
        central = damaged.index(b"PK\x01\x02")
        struct.pack_into("<H", damaged, central + 8, flags)
        damaged[central + 46] = 0x82
        assert invoke(executable, "list", 1, 100, data=damaged) == (expected, b"")
    for mode in ("encrypted", "method", "symlink"):
        damaged = bytearray(make_zip([("file", b"payload")]))
        central = damaged.index(b"PK\x01\x02")
        if mode == "encrypted":
            struct.pack_into("<H", damaged, 6, 1)
            struct.pack_into("<H", damaged, central + 8, 1)
        elif mode == "method":
            struct.pack_into("<H", damaged, 8, 99)
            struct.pack_into("<H", damaged, central + 10, 99)
        else:
            struct.pack_into("<I", damaged, central + 38, 0o120777 << 16)
        assert invoke(executable, "extract", 0, 100, data=damaged) == (3, b"")

    bomb = make_zip([("large", b"A" * 300000)])
    assert invoke(executable, "extract", 0, 1024, data=bomb) == (4, b"")
    damaged = bytearray(bomb)
    central = damaged.index(b"PK\x01\x02")
    struct.pack_into("<I", damaged, 22, 1)
    struct.pack_into("<I", damaged, central + 24, 1)
    status, body = invoke(executable, "extract", 0, 1024, data=damaged)
    assert status in (2, 4) and not body

    previous = zipfile.ZIP64_LIMIT
    try:
        zipfile.ZIP64_LIMIT = 1
        zip64 = make_zip([("zip64", b"large enough")])
    finally:
        zipfile.ZIP64_LIMIT = previous
    assert invoke(executable, "list", 1, 100, data=zip64) == (3, b"")
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w") as archive:
        with archive.open("local-zip64", "w", force_zip64=True) as entry:
            entry.write(b"small")
    assert invoke(executable, "extract", 0, 100, data=output.getvalue()) == (3, b"")
    check_entry_metadata(executable)
    print("ZIP interoperability, CRC, encoding, unsupported formats and limits passed")


def check_entry_metadata(executable):
    """Entry metadata matches zipfile, and the local-header read path matches extraction."""
    entries = [("dir/", b""), ("dir/file.txt", b"hello" * 100), ("empty", b""),
               ("binary.bin", bytes(range(256)) * 4)]
    for method in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
        data = make_zip(entries, method)
        status, body = invoke(executable, "entries", 100, 10000, data=data)
        assert status == 0, status
        rows = [line.split("\t") for line in body.decode("utf-8").splitlines()]
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            infos = archive.infolist()
        assert len(rows) == len(infos)
        for row, info in zip(rows, infos):
            assert row[0] == info.filename, row
            assert int(row[1]) == int(info.is_dir()), row
            assert int(row[2]) == info.file_size and int(row[3]) == info.compress_size, row
            assert int(row[4]) == info.CRC and int(row[5]) == 1, row
        for index, (name, expected) in enumerate(entries):
            if name.endswith("/"):
                assert invoke(executable, "read", index, 10000, data=data) == (3, b"")
            else:
                assert invoke(executable, "read", index, len(expected), data=data) == (0, expected)
        assert invoke(executable, "read", 1, 499, data=data) == (4, b"")
        assert invoke(executable, "read", 1, 500, data=data)[0] == 0
        assert invoke(executable, "read", 9, 100, data=data) == (5, b"")
        assert invoke(executable, "entries", 3, 10000, data=data) == (4, b"")
        assert invoke(executable, "entries", 100, 3, data=data) == (4, b"")

    # Damaged and unsupported archives behave the same through both read paths.
    for method in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
        base = make_zip([("file", b"payload" * 20)], method)
        central = base.index(b"PK\x01\x02")
        variants = []
        flipped = bytearray(base)
        flipped[30 + 4] ^= 0xFF
        variants.append(flipped)
        wrong_crc = bytearray(base)
        struct.pack_into("<I", wrong_crc, 14, 1)
        struct.pack_into("<I", wrong_crc, central + 16, 1)
        variants.append(wrong_crc)
        bad_header = bytearray(base)
        bad_header[0] = 0
        variants.append(bad_header)
        encrypted = bytearray(base)
        struct.pack_into("<H", encrypted, 6, 1)
        struct.pack_into("<H", encrypted, central + 8, 1)
        variants.append(encrypted)
        other_method = bytearray(base)
        struct.pack_into("<H", other_method, 8, 99)
        struct.pack_into("<H", other_method, central + 10, 99)
        variants.append(other_method)
        symlink = bytearray(base)
        struct.pack_into("<I", symlink, central + 38, 0o120777 << 16)
        variants.append(symlink)
        short_size = bytearray(base)
        struct.pack_into("<I", short_size, 22, 1)
        struct.pack_into("<I", short_size, central + 24, 1)
        variants.append(short_size)
        for damaged in variants:
            extracted = invoke(executable, "extract", 0, 1000, data=bytes(damaged))
            read = invoke(executable, "read", 0, 1000, data=bytes(damaged))
            assert extracted[0] != 0 and read[0] != 0, (extracted, read)
            # Corrupt DEFLATE data is a generic miniz failure (1000+) when extracted by
            # index, and a plain invalid-archive error (2) on the local-header path.
            assert read == extracted or {extracted[0], read[0]} <= {2, 4} or                 (extracted[0] >= 1000 and read[0] == 2), (extracted, read)

    # A truncated archive and garbage never produce metadata.
    good = make_zip([("file", b"x")])
    for invalid in (b"", good[:-5], b"not a zip" * 20):
        assert invoke(executable, "entries", 10, 1000, data=invalid)[0] == 2
        assert invoke(executable, "read", 0, 100, data=invalid)[0] == 2


if __name__ == "__main__":
    main()
