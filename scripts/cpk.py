#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""CRI CPK archives (Unicorn Overlord's Unicorn*.CPK): read the file table and write a merged
archive. Files are copied as stored, CRILAYLA-compressed or not."""
import argparse
from pathlib import Path
import struct
import sys

# @UTF column types: struct format and size.
TYPES = {0: 'B', 1: 'b', 2: 'H', 3: 'h', 4: 'I', 5: 'i', 6: 'Q', 7: 'q', 8: 'f'}
STRING, DATA = 0xa, 0xb
ZERO, CONSTANT, ROW = 0x10, 0x30, 0x50


def decrypt(table):
    """A table as stored: plain, or XOR-masked (the PS4 archives)."""
    if table[:4] == b'@UTF':
        return table
    out, mask = bytearray(table), 0x655f
    for i in range(len(out)):
        out[i] ^= mask & 0xff
        mask = (mask * 0x4115) & 0xffffffff
    return bytes(out)


def read_table(table):
    """The rows of an @UTF table, as dicts."""
    table = decrypt(table)
    if table[:4] != b'@UTF':
        raise ValueError('not a CRI table')
    size, rows_at, strings_at, data_at, _, ncols, row_length, nrows = struct.unpack('>IIIIIHHI', table[4:32])
    body = table[8:8 + size]

    def string(at):
        return body[strings_at + at:body.index(b'\0', strings_at + at)].decode('utf-8', 'replace')

    def value(kind, at):
        if kind == STRING:
            return string(struct.unpack_from('>I', body, at)[0]), 4
        if kind == DATA:
            offset, length = struct.unpack_from('>II', body, at)
            return body[data_at + offset:data_at + offset + length], 8
        return struct.unpack_from('>' + TYPES[kind], body, at)[0], struct.calcsize(TYPES[kind])

    columns, at = [], 24
    for _ in range(ncols):
        flags, name = body[at], string(struct.unpack_from('>I', body, at + 1)[0])
        at += 5
        constant = None
        if flags & 0xf0 == CONSTANT:
            constant, length = value(flags & 0xf, at)
            at += length
        columns.append((flags, name, constant))
    rows = []
    for r in range(nrows):
        at, row = rows_at + r * row_length, {}
        for flags, name, constant in columns:
            if flags & 0xf0 == CONSTANT:
                row[name] = constant
            elif flags & 0xf0 == ROW:
                row[name], length = value(flags & 0xf, at)
                at += length
        rows.append(row)
    return rows


def write_table(name, columns, rows):
    """An @UTF table: columns are (name, type), every value stored per row."""
    strings, offsets = bytearray(), {}

    def string(text):
        if text not in offsets:
            offsets[text] = len(strings)
            strings.extend(text.encode() + b'\0')
        return offsets[text]

    string('<NULL>')
    string(name)
    schema = b''.join(struct.pack('>BI', ROW | kind, string(column)) for column, kind in columns)
    packed = []
    for row in rows:
        packed.append(b''.join(struct.pack('>I', string(row[column])) if kind == STRING else
                               struct.pack('>' + TYPES[kind], row[column]) for column, kind in columns))
    row_length = len(packed[0]) if packed else 0
    rows_at = 24 + len(schema)
    strings_at = rows_at + row_length * len(rows)
    body = struct.pack('>IIIIHHI', rows_at, strings_at, strings_at + len(strings), string(name),
                       len(columns), row_length, len(rows))
    body += schema + b''.join(packed) + strings
    body += b'\0' * (-len(body) % 8)
    return b'@UTF' + struct.pack('>I', len(body)) + body


class Archive:
    """A CPK's files: path -> (offset, stored size, extract size)."""

    def __init__(self, path):
        self.path = Path(path)
        with open(self.path, 'rb') as f:
            head = f.read(16)
            if head[:4] != b'CPK ':
                raise ValueError(f'{self.path.name}: not a CPK archive')
            header = read_table(f.read(struct.unpack_from('<Q', head, 8)[0]))[0]
            if not header.get('TocOffset'):
                raise ValueError(f'{self.path.name}: no file names (an ID-only CPK)')
            f.seek(header['TocOffset'])
            toc = f.read(header['TocSize'])  # with the 16-byte section header
        if toc[:4] != b'TOC ':
            raise ValueError(f'{self.path.name}: bad file table')
        # File offsets count from the table or the content, whichever comes first.
        base = min(header['TocOffset'], header.get('ContentOffset') or header['TocOffset'])
        self.files = {}
        for row in read_table(toc[16:]):
            name = '/'.join(p for p in (row.get('DirName'), row['FileName']) if p and p != '<NULL>')
            self.files[name] = (base + row['FileOffset'], row['FileSize'], row['ExtractSize'])

    def read(self, name):
        offset, size, _ = self.files[name]
        with open(self.path, 'rb') as f:
            f.seek(offset)
            return f.read(size)

    def extract(self, name):
        """A file's contents, decompressed when stored as CRILAYLA."""
        return crilayla(self.read(name))


class Memory:
    """Files held in memory, written into an archive as they are (not compressed)."""

    def __init__(self):
        self.path, self.files, self.data = None, {}, {}

    def add(self, name, data):
        self.files[name] = (0, len(data), len(data))
        self.data[name] = data

    def read(self, name):
        return self.data[name]


def child_file(folder, name):
    """`name` in `folder`, matched without case as the game does."""
    for entry in Path(folder).iterdir():
        if entry.name.casefold() == name.casefold():
            return entry
    raise OSError(f'{name} not found in {folder}')


def crilayla(data):
    """CRILAYLA decompression; other data is returned as is. The stream is read from its end
    backwards, bits from the top, and fills the output from its end; the first 0x100 bytes are
    stored plain after the stream."""
    if data[:8] != b'CRILAYLA':
        return data
    size, packed = struct.unpack_from('<II', data, 8)
    if 16 + packed + 0x100 > len(data):
        raise ValueError('CRILAYLA data is cut short')
    out = bytearray(0x100 + size)
    out[:0x100] = data[16 + packed:16 + packed + 0x100]
    at, pool, left = 16 + packed - 1, 0, 0

    def bits(count):
        nonlocal at, pool, left
        value = 0
        while count:
            if not left:
                if at < 16:
                    raise ValueError('CRILAYLA stream is cut short')
                pool, left, at = data[at], 8, at - 1
            take = min(left, count)
            value = (value << take) | ((pool >> (left - take)) & ((1 << take) - 1))
            left -= take
            count -= take
        return value

    end = len(out) - 1
    done = 0
    while done < size:
        if not bits(1):
            out[end - done] = bits(8)
            done += 1
            continue
        back, length = bits(13) + 3, 3
        for width in (2, 3, 5):
            more = bits(width)
            length += more
            if more != (1 << width) - 1:
                break
        else:
            while True:
                more = bits(8)
                length += more
                if more != 0xff:
                    break
        if done + length > size or back > done:
            raise ValueError('CRILAYLA back reference out of range')
        for _ in range(length):
            out[end - done] = out[end - done + back]
            done += 1
    return bytes(out)


HEADER = [('UpdateDateTime', 6), ('ContentOffset', 6), ('ContentSize', 6), ('TocOffset', 6), ('TocSize', 6),
          ('EnabledPackedSize', 6), ('EnabledDataSize', 6), ('Files', 4), ('Groups', 4), ('Attrs', 4),
          ('Version', 2), ('Revision', 2), ('Align', 2), ('Sorted', 2), ('EnableFileName', 2),
          ('CpkMode', 4), ('Tvers', STRING), ('Codec', 4), ('DpkItoc', 4)]
TOC = [('DirName', STRING), ('FileName', STRING), ('FileSize', 4), ('ExtractSize', 4), ('FileOffset', 6),
       ('ID', 4)]
ALIGN = 2048


def chunk(data, label):
    """A CPK section: its label, the table's size, the table (unmasked)."""
    return label + struct.pack('<IQ', 0xff, len(data)) + data


def write(out, files):
    """Writes a CPK of `files`: archive path -> (source Archive, its name there). The table
    (sorted by upper-case path, as in the game's archives) comes first, then the files, each
    aligned."""
    names = sorted(files, key=str.upper)

    def toc(content):
        rows, at = [], content - ALIGN
        for i, name in enumerate(names):
            source, original = files[name]
            _, size, extract = source.files[original]
            folder, _, file = name.rpartition('/')
            rows.append({'DirName': folder or '<NULL>', 'FileName': file, 'FileSize': size,
                         'ExtractSize': extract, 'FileOffset': at, 'ID': i})
            at += size + (-size % ALIGN)
        return chunk(write_table('CpkTocInfo', TOC, rows), b'TOC '), at - (content - ALIGN)

    table, _ = toc(ALIGN)  # the size only: offsets are fixed-width
    content = ALIGN + len(table) + (-len(table) % ALIGN)
    table, content_size = toc(content)
    packed = sum(files[n][0].files[files[n][1]][1] for n in names)
    extracted = sum(files[n][0].files[files[n][1]][2] for n in names)
    header = write_table('CpkHeader', HEADER, [{
        'UpdateDateTime': 1, 'ContentOffset': content, 'ContentSize': content_size, 'TocOffset': ALIGN,
        'TocSize': len(table), 'EnabledPackedSize': packed, 'EnabledDataSize': extracted,
        'Files': len(names), 'Groups': 0, 'Attrs': 0, 'Version': 7, 'Revision': 14, 'Align': ALIGN,
        'Sorted': 1, 'EnableFileName': 1, 'CpkMode': 1, 'Tvers': 'vwport mods.py', 'Codec': 0,
        'DpkItoc': 0}])
    head = chunk(header, b'CPK ')
    head += b'\0' * (ALIGN - 6 - len(head)) + b'(c)CRI'
    with open(out, 'wb') as f:
        f.write(head + table + b'\0' * (content - ALIGN - len(table)))
        for name in names:
            source, original = files[name]
            size = source.files[original][1]
            data = source.read(original)
            if len(data) != size:
                raise ValueError(f'{original} is cut short')
            f.write(data + b'\0' * (-size % ALIGN))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    args = parser.parse_args()
    for name, (_, size, extract) in Archive(args.archive).files.items():
        print(f'{name}\t{size}\t{extract}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError) as error:
        print(f'cpk: {error}', file=sys.stderr)
        sys.exit(1)
