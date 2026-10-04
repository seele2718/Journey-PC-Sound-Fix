#!/usr/bin/env python3
"""Inspect, patch, verify or undo the localized Journey Tone-cache repair.

Never overwrites a file. Compatibility uses local signatures and PE structure;
whole-file hashes appear only in reports. Undo metadata is embedded in .tna1.
Requires pefile, capstone, and GNU MinGW as (development patcher).
"""
from __future__ import annotations
import argparse
from array import array
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import capstone
import pefile
from build_tone_cache_native_a import plan

VERSION='0.1.0-alpha.1'
SECTION=b'.tna1'
MAGIC=b'JOURNEY_TONE_A_UNDO_V1'.ljust(32,b'\0')
CAPSULE_LIMIT=65536
SCHEMA='journey-tone-cache-a-undo-v1'


class PatchError(ValueError):
    pass


def require(condition, message):
    if not condition: raise PatchError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def align(n,a):
    return (n+a-1)&-a


def checksum(data,offset):
    words=array('H',data+(b'\0' if len(data)&1 else b''))
    if sys.byteorder!='little':words.byteswap()
    total=sum(words)-words[offset//2]-words[offset//2+1]
    while total>>16:total=(total&0xffff)+(total>>16)
    return (total+len(data))&0xffffffff


def parse(data,allow_patch=False):
    require(__debug__,'Optimized Python (-O) is unsupported: plan assertions must stay enabled.')
    require(len(data)>=0x100 and data[:2]==b'MZ','Truncated or invalid DOS header.')
    try: pe=pefile.PE(data=data,fast_load=True)
    except Exception as e:raise PatchError(f'Invalid PE: {e}') from e
    require(pe.FILE_HEADER.Machine==0x8664 and pe.OPTIONAL_HEADER.Magic==0x20b,'Only PE32+ x64 is supported.')
    require(pe.OPTIONAL_HEADER.ImageBase==0x140000000,'Unsupported preferred image base.')
    require((pe.OPTIONAL_HEADER.FileAlignment,pe.OPTIONAL_HEADER.SectionAlignment)==(512,4096),'Unsupported PE alignments.')
    require(pe.OPTIONAL_HEADER.SizeOfHeaders<=len(data),'Truncated PE headers.')
    require(pe.OPTIONAL_HEADER.NumberOfRvaAndSizes>=16,'Truncated data directories.')
    require(pe.FILE_HEADER.NumberOfSections==len(pe.sections) and len(pe.sections)>0,'Truncated section table.')
    names=[s.Name.rstrip(b'\0') for s in pe.sections]
    require(len(names)==len(set(names)),'Ambiguous duplicate section names.')
    if not allow_patch:require(SECTION not in names,'Already/partially patched .tna1 section; use verify or unpatch.')
    require(pe.OPTIONAL_HEADER.DATA_DIRECTORY[4].Size==0 and pe.OPTIONAL_HEADER.DATA_DIRECTORY[4].VirtualAddress==0,'Authenticode-signed inputs are unsupported.')
    raw_ranges=[];virtual_ranges=[]
    for s in pe.sections:
        if s.SizeOfRawData:
            require(s.PointerToRawData>=pe.OPTIONAL_HEADER.SizeOfHeaders,'Section overlaps PE headers.')
            require(s.PointerToRawData+s.SizeOfRawData<=len(data),'Truncated section '+repr(s.Name))
            raw_ranges.append((s.PointerToRawData,s.PointerToRawData+s.SizeOfRawData))
        end=s.VirtualAddress+max(s.Misc_VirtualSize,s.SizeOfRawData)
        require(s.VirtualAddress%4096==0 and end<=pe.OPTIONAL_HEADER.SizeOfImage,'Invalid virtual section containment.')
        virtual_ranges.append((s.VirtualAddress,end))
    for spans in (raw_ranges,virtual_ranges):
        spans.sort()
        require(all(a[1]<=b[0] for a,b in zip(spans,spans[1:])),'Overlapping PE sections.')
    require(pe.OPTIONAL_HEADER.SizeOfImage%4096==0,'Unaligned SizeOfImage.')
    return pe


def local_plan(data):
    try:return plan(data)
    except (AssertionError,KeyError,StopIteration,struct.error,ValueError) as e:
        raise PatchError('Local signature/structure plan rejected: '+(str(e) or type(e).__name__)) from e


def directory_bytes(pe,data,index):
    entry=pe.OPTIONAL_HEADER.DATA_DIRECTORY[index]
    if not entry.Size:return b''
    start=pe.get_offset_from_rva(entry.VirtualAddress)
    require(start+entry.Size<=len(data),'Truncated directory.')
    return data[start:start+entry.Size]


def entries(pe,data):
    raw=directory_bytes(pe,data,3)
    require(len(raw)%12==0,'Invalid exception-directory size.')
    result=list(struct.iter_unpack('<III',raw))
    require(all(a<b for a,b,_ in result),'Empty/reversed runtime-function range.')
    require(all(a[1]<=b[0] for a,b in zip(result,result[1:])),'Unsorted/overlapping runtime-function ranges.')
    return result


def make_patch(data):
    pe=parse(data)
    original_functions=entries(pe,data)
    p=local_plan(data)
    header=pe.sections[-1].get_file_offset()+40
    require(header+40<=pe.OPTIONAL_HEADER.SizeOfHeaders,'No section-header space.')
    require(data[header:header+40]==bytes(40),'Section-header slot is not empty.')
    raw_start=align(len(data),512)
    capsule_at=align(len(p['blob']),16)
    checksum_offset=pe.OPTIONAL_HEADER.get_field_absolute_offset('CheckSum')
    exception_end=p['exception_append_rva']
    pdata=next(s for s in pe.sections if s.VirtualAddress<=exception_end<s.VirtualAddress+s.SizeOfRawData)
    require(pdata.Name.rstrip(b'\0')==b'.pdata','Unsupported exception-directory section.')
    require(original_functions[-1][1]<=p['origin'],'New runtime functions are not appendable in order.')
    def journal(virtual_size,raw_size):
        records=[]
        def item(label,off,value):
            require(0<=off and off+len(value)<=len(data),'Write outside original prefix.')
            records.append({'name':label,'offset':off,'before':data[off:off+len(value)].hex(),'after':value.hex()})
        for c in p['changes']:
            item('code:'+c['name'],pe.get_offset_from_rva(c['rva']),c['bytes'])
        item('exception entries',pe.get_offset_from_rva(exception_end),p['exception_append_bytes'])
        item('pdata virtual size',pdata.get_field_absolute_offset('Misc_VirtualSize'),struct.pack('<I',max(pdata.Misc_VirtualSize,exception_end+48-pdata.VirtualAddress)))
        item('exception directory size',pe.OPTIONAL_HEADER.DATA_DIRECTORY[3].get_field_absolute_offset('Size'),struct.pack('<I',pe.OPTIONAL_HEADER.DATA_DIRECTORY[3].Size+48))
        item('section count',pe.FILE_HEADER.get_field_absolute_offset('NumberOfSections'),struct.pack('<H',len(pe.sections)+1))
        item('SizeOfCode',pe.OPTIONAL_HEADER.get_field_absolute_offset('SizeOfCode'),struct.pack('<I',pe.OPTIONAL_HEADER.SizeOfCode+raw_size))
        item('SizeOfImage',pe.OPTIONAL_HEADER.get_field_absolute_offset('SizeOfImage'),struct.pack('<I',align(p['origin']+virtual_size,4096)))
        section=struct.pack('<8sIIIIIIHHI',SECTION.ljust(8,b'\0'),virtual_size,p['origin'],raw_size,raw_start,0,0,0,0,0x60000020)
        item('new section header',header,section)
        records.append({'name':'checksum','offset':checksum_offset,'before':data[checksum_offset:checksum_offset+4].hex(),'after':None})
        records.sort(key=lambda r:r['offset'])
        require(all(a['offset']+len(bytes.fromhex(a['before']))<=b['offset'] for a,b in zip(records,records[1:])),'Overlapping patch writes.')
        return records
    def capsule(records):
        return json.dumps({'schema':SCHEMA,'version':VERSION,'base_size':len(data),'blob_size':len(p['blob']),'records':records},sort_keys=True,separators=(',',':')).encode()
    trial=capsule(journal(0,0))
    virtual_size=capsule_at+36+len(trial)
    raw_size=align(virtual_size,512)
    records=journal(virtual_size,raw_size)
    body=capsule(records)
    require(len(body)==len(trial) and len(body)<CAPSULE_LIMIT,'Unexpected undo-capsule growth.')
    output=bytearray(data)
    output+=bytes(raw_start-len(output))+p['blob']+bytes(capsule_at-len(p['blob']))+MAGIC+struct.pack('<I',len(body))+body
    output+=bytes(raw_start+raw_size-len(output))
    for record in records:
        off=record['offset'];value=bytes.fromhex(record['after']) if record['after'] is not None else bytes(4)
        output[off:off+len(value)]=value
    struct.pack_into('<I',output,checksum_offset,checksum(bytes(output),checksum_offset))
    return bytes(output),p


def restore_patched_image(data):
    pe=parse(data,allow_patch=True)
    require(pe.sections[-1].Name.rstrip(b'\0')==SECTION,'No final architecture-A section.')
    section=pe.sections[-1]
    require(section.Characteristics==0x60000020,'Patched section is not the expected RX code section.')
    require(section.PointerToRawData+section.SizeOfRawData==len(data),'Unexpected trailing bytes after patch section; refusing truncation.')
    payload=data[section.PointerToRawData:section.PointerToRawData+section.Misc_VirtualSize]
    require(payload.count(MAGIC)==1,'Missing/ambiguous undo capsule.')
    pos=payload.index(MAGIC)
    require(pos+36<=len(payload),'Truncated undo header.')
    length=struct.unpack_from('<I',payload,pos+32)[0]
    require(length<CAPSULE_LIMIT and pos+36+length==len(payload),'Truncated/invalid undo capsule.')
    try:meta=json.loads(payload[pos+36:])
    except (ValueError,UnicodeDecodeError) as e:raise PatchError('Invalid undo JSON.') from e
    require(isinstance(meta,dict),'Undo capsule must be an object.')
    require(meta.get('schema')==SCHEMA and meta.get('version')==VERSION,'Unsupported patch version.')
    size=meta.get('base_size')
    require(type(size) is int and 1024<=size<=section.PointerToRawData,'Invalid original size.')
    require(meta.get('blob_size')==808 and pos==align(meta['blob_size'],16),'Unexpected native blob size/layout.')
    require(data[size:section.PointerToRawData]==bytes(section.PointerToRawData-size),'Unexpected nonzero alignment gap.')
    require(data[section.PointerToRawData+section.Misc_VirtualSize:]==bytes(section.SizeOfRawData-section.Misc_VirtualSize),'Nonzero patch padding.')
    records=meta.get('records')
    require(isinstance(records,list) and 7<=len(records)<=32,'Invalid undo record count.')
    restored=bytearray(data[:size]);previous_end=0;checksums=0
    for r in records:
        require(isinstance(r,dict),'Undo record must be an object.')
        try:
            off=r['offset'];before=bytes.fromhex(r['before']);after=None if r['after'] is None else bytes.fromhex(r['after'])
        except (ValueError,KeyError,TypeError) as e:raise PatchError('Malformed undo record.') from e
        require(type(off) is int and previous_end<=off and 0<len(before)<=128 and off+len(before)<=size,'Overlapping/out-of-bounds undo record.')
        previous_end=off+len(before)
        if after is None:
            require(r.get('name')=='checksum' and len(before)==4 and off==pe.OPTIONAL_HEADER.get_field_absolute_offset('CheckSum'),'Invalid checksum record.')
            checksums+=1
        else:
            require(len(after)==len(before) and data[off:off+len(after)]==after,'Patched bytes changed: '+r.get('name','unknown'))
        restored[off:off+len(before)]=before
    require(checksums==1,'Missing/duplicate checksum record.')
    return bytes(restored),pe,meta


def verify(data):
    base,pe,meta=restore_patched_image(data)
    expected,p=make_patch(base)
    require(expected==data,'Patched image differs from the canonical localized patch of its recovered base.')
    off=pe.OPTIONAL_HEADER.get_field_absolute_offset('CheckSum')
    require(pe.OPTIONAL_HEADER.CheckSum==checksum(data,off),'Invalid PE checksum.')
    old=parse(base)
    old_entries=entries(old,base);new_entries=entries(pe,data)
    require(new_entries==old_entries+[tuple(e) for e in p['runtime_functions']],'Exception entries changed unexpectedly.')
    require(p['runtime_functions'][0][0]==p['origin'] and p['runtime_functions'][-1][1]==p['origin']+p['code_bytes'],'New code is not wholly covered by unwind entries.')
    require(all(a[1]==b[0] for a,b in zip(p['runtime_functions'],p['runtime_functions'][1:])),'Gap in new unwind coverage.')
    for index in (0,1,2,4,5,6,7,8,9,10,11,12,13,14,15):
        a,b=old.OPTIONAL_HEADER.DATA_DIRECTORY[index],pe.OPTIONAL_HEADER.DATA_DIRECTORY[index]
        require((a.VirtualAddress,a.Size)==(b.VirtualAddress,b.Size),'Unrelated PE directory changed.')
    require(directory_bytes(old,base,10)==directory_bytes(pe,data,10),'Load config changed.')
    require(old.OPTIONAL_HEADER.DllCharacteristics==pe.OPTIONAL_HEADER.DllCharacteristics,'ASLR/CFG/NX flags changed.')
    require(old.FILE_HEADER.Characteristics==pe.FILE_HEADER.Characteristics,'COFF characteristics changed.')
    for a,b in zip(old.sections,pe.sections):
        require((a.Name,a.VirtualAddress,a.PointerToRawData,a.SizeOfRawData,a.Characteristics)==(b.Name,b.VirtualAddress,b.PointerToRawData,b.SizeOfRawData,b.Characteristics),'Original section moved/resized/reflagged.')
    decoder=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);decoder.detail=True
    code=p['blob'][:p['code_bytes']]
    instructions=list(decoder.disasm(code,p['origin']))
    require(sum(i.size for i in instructions)==len(code),'Undecodable appended code.')
    starts={i.address for i in instructions}
    external={0x2c8641,0x2c864b,0x2c86fd,0x2c87db,0x2c0e44,0x2c0ef1,0x2ca102,0x2ca1b2,0x7b7c0,0x499df4,0x2d6c10,0x49ba82}
    branches=0;relative_imports=0
    for i in instructions:
        if i.group(capstone.CS_GRP_JUMP) or i.group(capstone.CS_GRP_CALL):
            for operand in i.operands:
                if operand.type==capstone.CS_OP_IMM:
                    require(operand.imm in starts or operand.imm in external,'Unexpected native branch target.')
                    branches+=1
                elif operand.type==capstone.CS_OP_MEM:
                    require(operand.mem.base==capstone.x86.X86_REG_RIP,'Non-relative imported call.')
                    require(i.address+i.size+operand.mem.disp in (0x5539d8,0x5539e0,0x554050),'Unexpected IAT target.')
                    relative_imports+=1
    # Relocation independence of the added instructions, not a claim that the
    # original relocation-stripped EXE gains ASLR support.
    rebased=list(decoder.disasm(code,p['origin']+0x40000000))
    for a,b in zip(instructions,rebased):
        for x,y in zip(a.operands,b.operands):
            if x.type==capstone.CS_OP_IMM and (a.group(capstone.CS_GRP_JUMP) or a.group(capstone.CS_GRP_CALL)):
                require(y.imm-x.imm==0x40000000,'Added branch is not relocation-independent.')
    overlay_start=max(s.PointerToRawData+s.SizeOfRawData for s in old.sections)
    return {'status':'verified','patch_version':VERSION,'input_sha256_provenance':digest(base),'output_sha256_provenance':digest(data),
            'original_size':len(base),'output_size':len(data),'section_rva':hex(p['origin']),'section_virtual_size':pe.sections[-1].Misc_VirtualSize,
            'section_raw_size':pe.sections[-1].SizeOfRawData,'native_blob_size':p['blob_bytes'],'code_sites':7,'code_bytes_changed':50,
            'undo_records':len(meta['records']),'preserved_original_overlay_bytes':len(base)-overlay_start,
            'new_runtime_functions':4,'direct_branch_targets_checked':branches,'relative_import_calls_checked':relative_imports,
            'checksum':hex(pe.OPTIONAL_HEADER.CheckSum),'dynamic_base':bool(pe.OPTIONAL_HEADER.DllCharacteristics&0x40),
            'relocation_directory_bytes':pe.OPTIONAL_HEADER.DATA_DIRECTORY[5].Size,'load_config_unchanged':True,
            'round_trip':'byte-identical to recovered original','qualification':'static PE/code verification; does not run the game'}


def unpatch(data):
    verify(data)
    return restore_patched_image(data)[0]


def inspect(data):
    pe=parse(data,allow_patch=True)
    if any(s.Name.rstrip(b'\0')==SECTION for s in pe.sections):return verify(data)
    _,p=make_patch(data)
    return {'status':'compatible-unpatched','patch_version':VERSION,'sha256_provenance':digest(data),'size':len(data),'planned_native_blob_bytes':p['blob_bytes']}


def write_new(path,data):
    path=Path(path);path.parent.mkdir(parents=True,exist_ok=True)
    require(not path.exists(),'Refusing existing output: '+str(path))
    temp=None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent,prefix='.'+path.name+'.',delete=False) as f:
            temp=Path(f.name);f.write(data);f.flush();os.fsync(f.fileno())
        os.link(temp,path)  # Atomic exclusive destination creation, never rename-overwrite.
    finally:
        if temp is not None:temp.unlink(missing_ok=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode',choices=['inspect','patch','verify','unpatch'])
    parser.add_argument('input',type=Path)
    parser.add_argument('--output',type=Path)
    parser.add_argument('--report',type=Path)
    args=parser.parse_args()
    try:
        require((args.mode in ('patch','unpatch'))==bool(args.output),'Use --output exactly for patch/unpatch.')
        if args.output:require(not args.output.exists(),'Output already exists.')
        if args.report:require(not args.report.exists(),'Report already exists.')
        data=args.input.read_bytes()
        if args.mode=='patch':
            output,_=make_patch(data);report=verify(output)
            require(unpatch(output)==data,'Internal round-trip mismatch.')
        elif args.mode=='unpatch':
            output=unpatch(data);report={'status':'unpatched','sha256_provenance':digest(output),'size':len(output)}
        else:report=inspect(data) if args.mode=='inspect' else verify(data)
        report['input_path']=str(args.input.resolve())
        if args.output:
            write_new(args.output,output);report['output_path']=str(args.output.resolve())
        if args.report:write_new(args.report,(json.dumps(report,indent=2)+'\n').encode())
        print(json.dumps(report,indent=2))
        return 0
    except (PatchError,OSError,struct.error,pefile.PEFormatError) as e:
        print(json.dumps({'status':'rejected','error':str(e)}),file=sys.stderr)
        return 2


if __name__=='__main__':raise SystemExit(main())
