#!/usr/bin/env python3
"""Assemble the Tone-cache repair into a code and patch-site plan.

GNU MinGW as emits a temporary COFF object; extraction requires relocation-free
code. The patch-site contexts are stored separately as compatibility signatures.
The caller applies the returned plan to the executable.
"""
from __future__ import annotations
import hashlib
import json
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path
from types import SimpleNamespace
import pefile

HERE = Path(__file__).resolve().parent
SIGNATURES = HERE / 'tone_cache_native_a_signatures.json'


def coff(data):
    machine, count, _, symptr, symcount, opt, _ = struct.unpack_from('<HHIIIHH', data)
    assert machine == 0x8664 and opt == 0
    strings = data[symptr + 18 * symcount:]
    def name(raw):
        if raw[:4] == bytes(4):
            start = struct.unpack_from('<I', raw, 4)[0]
            return strings[start:strings.index(b'\0', start)].decode()
        return raw.rstrip(b'\0').decode()
    sections, symbols = {}, {}
    for i in range(count):
        off = 20 + 40 * i
        n = data[off:off+8].rstrip(b'\0').decode()
        size, ptr, relocptr, _, relocs = struct.unpack_from('<IIIIH', data, off+16)
        sections[n] = {'bytes': data[ptr:ptr+size], 'relocations': relocs}
    i = 0
    while i < symcount:
        off = symptr + 18*i
        raw, value, section, _, _, aux = struct.unpack_from('<8sIhHBB', data, off)
        if section == 1:
            symbols[name(raw)] = value
        i += 1 + aux
    return sections, symbols


def plan(data):
    pe = pefile.PE(data=data, fast_load=True)
    pe.parse_data_directories(directories=[1])
    assert pe.FILE_HEADER.Machine == 0x8664
    assert pe.OPTIONAL_HEADER.Magic == 0x20b
    exception_dir=pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
    exception_entries=[SimpleNamespace(BeginAddress=a,EndAddress=b,UnwindData=c)
                       for a,b,c in struct.iter_unpack('<III',pe.get_data(exception_dir.VirtualAddress,exception_dir.Size))]
    imports={i.address-pe.OPTIONAL_HEADER.ImageBase:i.name
             for e in pe.DIRECTORY_ENTRY_IMPORT for i in e.imports}
    assert imports[0x5539d8]==b'_aligned_malloc'
    assert imports[0x5539e0]==b'_aligned_free'
    assert b'release' in imports[0x554050]
    signatures = json.loads(SIGNATURES.read_text())
    # The signature file's provenance hash identifies the reference image with
    # the animation-register repair already applied, not the final sound fix.
    # Hash is provenance only. Compatibility is the complete set of unique
    # contexts and the supported image-relative layout / unwind contracts.
    for site in signatures['contexts']:
        expected = bytes.fromhex(site['bytes'])
        assert data.count(expected) == 1, site['name'] + ': non-unique/missing signature'
        assert pe.get_data(site['rva'], len(expected)) == expected, site['name'] + ': layout mismatch'
    origin = (pe.OPTIONAL_HEADER.SizeOfImage + 0xfff) & ~0xfff
    assembler = shutil.which('x86_64-w64-mingw32-as')
    assert assembler, 'GNU MinGW assembler is required'
    with tempfile.TemporaryDirectory(prefix='journey-tone-a-') as tmp:
        obj = Path(tmp) / 'tone.o'
        subprocess.run([assembler, '--defsym', f'REGION_RVA={origin}',
                        str(HERE / 'tone_cache_native_a.s'), '-o', str(obj)], check=True)
        sections, symbols = coff(obj.read_bytes())
    assert sections['.text']['relocations'] == 0
    assert sections['.xdata']['relocations'] == 0
    code = sections['.text']['bytes'][:symbols['blob_end']]
    changes = []
    for rva, label, width in ((0x2c86a7,'first_search',8), (0x2c8727,'creator_return',8),
                              (0x2c0e0b,'playback_search',8), (0x2ca0fa,'creator_alloc_guard',5)):
        dest = origin + symbols[label]
        payload = b'\xe9' + struct.pack('<i', dest-rva-5) + b'\x90'*(width-5)
        changes.append({'rva': rva, 'bytes': payload, 'name': label})
    for rva in (0x2c7f4c, 0x2c86fd, 0x2c8896):
        changes.append({'rva': rva, 'bytes': bytes.fromhex('39 c0 90 90 90 90 90'),
                        'name': 'pin bank loader to normal quality'})
    # Out-of-line fragments use chained unwind records for their unchanged
    # original stack frame. reserve_vector has assembler-produced own metadata.
    metadata = bytearray()
    entries = []
    for begin,end,parent_rva in (
        ('first_search','loader_fragments_end',0x2c86a7),
        ('playback_search','playback_end',0x2c0e0b),
        ('creator_alloc_guard','creator_alloc_end',0x2ca0fa)):
        parent = next(e for e in exception_entries
                      if e.BeginAddress <= parent_rva < e.EndAddress)
        expected_unwind={
            0x2c86a7:(0x2c7ae0,0x2c8975,0x6fb64c,'01250a002501bd0016f014e012d010c00e700d600c300b50'),
            0x2c0e0b:(0x2c0d00,0x2c0f33,0x6faf2c,'011c0a001c0157000df00be009d007c00570046003300250'),
            0x2ca0fa:(0x2ca000,0x2ca1d0,0x6fb7f4,'01190b0019542c0019342b001901240012f010e00ec00c700b600000'),
        }[parent_rva]
        assert (parent.BeginAddress,parent.EndAddress,parent.UnwindData)==expected_unwind[:3]
        expected=bytes.fromhex(expected_unwind[3])
        assert pe.get_data(parent.UnwindData,len(expected))==expected
        unwind = origin + len(code) + ((-len(code)) % 4) + len(metadata)
        metadata += bytes([0x21, 0, 0, 0]) + struct.pack('<III',parent.BeginAddress,parent.EndAddress,parent.UnwindData)
        entries.append([origin+symbols[begin],origin+symbols[end],unwind])
    unwind = origin + len(code) + ((-len(code)) % 4) + len(metadata)
    metadata += sections['.xdata']['bytes']
    entries.append([origin+symbols['reserve_vector'],origin+symbols['reserve_vector_end'],unwind])
    blob = code + bytes((-len(code)) % 4) + metadata
    olddir = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
    exception_end = olddir.VirtualAddress + olddir.Size
    pdata = next(s for s in pe.sections if s.VirtualAddress <= exception_end < s.VirtualAddress+s.SizeOfRawData)
    exception_bytes = b''.join(struct.pack('<III',*entry) for entry in entries)
    assert pdata.VirtualAddress+pdata.SizeOfRawData >= exception_end+len(exception_bytes)
    assert pe.get_data(exception_end,len(exception_bytes)) == bytes(len(exception_bytes))
    assert max(e.EndAddress for e in exception_entries) <= origin
    return {'origin':origin,'blob':blob,'symbols':symbols,'changes':changes,
            'runtime_functions':entries,'exception_append_rva':exception_end,
            'exception_append_bytes':exception_bytes,
            'sha256_provenance':hashlib.sha256(data).hexdigest(),
            'code_bytes':len(code),'blob_bytes':len(blob)}
