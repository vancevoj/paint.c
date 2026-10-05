# Lane L6C (.pdn codec): attribution and provenance

The .pdn reader and writer (src/codec/fmt_pdn.c, pdn_write.c, pdn_png.c,
nrbf.c, nrbf.h, pdn.h) were written for paint.c. No Paint.NET 4.x, 5.x or
6.x code, binary, decompiled IL or disassembly was used (P-01, ADR-002). The
format knowledge comes from the sources below.

## Paint.NET 3.36 source (MIT), via the OpenPDN mirror

https://github.com/rivy/OpenPDN, files studied:

- `src/Core/MemoryBlock.cs`: layout of deferred memory block data after the
  serialized object graph (format byte 0 = gzip chunks / 1 = raw chunks,
  big-endian uint32 chunk size, then per chunk a big-endian chunk number and
  data size, chunks in any order, chunk count = ceil(length / chunk size),
  bounds checks per chunk). fmt_pdn.c's `read_block` and pdn_write.c's
  `write_block` implement this layout; the code is not a translation.
- `src/Data/Document.cs`: container layout ("PDN3" magic, 24-bit little-endian
  header length, UTF-8 XML header, the 0x00 0x01 marker before the
  BinaryFormatter stream, and the legacy 0x1F 0x8B gzip marker that the
  loader now reports as unsupported).

The 3.36 source is MIT-licensed except for artwork, resources and GPC
(see NOTICE). Copyright (C) dotPDN LLC, Rick Brewster, Chris Crosetto, Tom
Jackson, Michael Kelsey, Brandon Ortiz, Craig Taylor, Chris Trevino and Luke
Walker.

## pypdn (MIT)

https://github.com/addisonElliott/pypdn, Copyright (c) 2018 Addison Elliott,
MIT License. Studied for the member names a reader needs
(`ArrayList+_items`, `Layer+properties`, `blendMode.value__`), the
`UserBlendOps+<Name>BlendOp` class names of 3.x files, and the BlendType
values 0..13. pypdn is also the independent reader used to verify our
output (tests/codec/pdn_pypdn_check.py, run outside CTest). The pypdn test
documents are redistributed in tests/codec/samples/pdn (license text in
SOURCES.md there).

## [MS-NRBF] specification

".NET Remoting: Binary Format Data Structure", Microsoft Open
Specifications (https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-nrbf/).
nrbf.c implements the record grammar from the specification. The object id
assignment and metadata reuse that pdn_write.c mirrors (one id consumed per
object lookup, negative ids for value types, ClassWithId only while member
types match the first record of a class) were derived by comparing record
dumps of 489 real documents, not from any implementation.

## Real documents (black-box observation)

The writer's record sequence, metadata entries, header XML, thumbnail size
rule and gzip parameters were derived from dumps of real .pdn files saved by
Paint.NET 3.0 to 5.1.12 and published on GitHub (list and results in
docs/codecs/pdn.md). Only redistributable files are in the repository.
