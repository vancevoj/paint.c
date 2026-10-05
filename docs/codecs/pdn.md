# .pdn (Paint.NET image) codec

Lane L6C. Import and export of Paint.NET's native layered format (ADR-007).
Code: `src/codec/fmt_pdn.c` (loader, dump, codec descriptor `pc_codec_pdn`),
`src/codec/pdn_write.c` (writer), `src/codec/pdn_png.c` (thumbnail PNG,
base64), `src/codec/nrbf.c`/`nrbf.h` (bounded MS-NRBF reader and record
writer), `src/codec/pdn.h` (internal API for the codec files and tests).
Attribution: docs/notice/l6c.md.

## File layout

```
"PDN3"                         magic (4 bytes)
uint24 LE                      header length H
H bytes UTF-8 XML              <pdnImage width=".." height=".." layers=".."
                               savedWithVersion="A.B.C.D"><custom><thumb png="BASE64" />
                               </custom></pdnImage>
0x00 0x01                      "BinaryFormatter data follows" (0x1F 0x8B = Paint.NET 2.x gzip)
MS-NRBF stream                 root object PaintDotNet.Document, ends with MessageEnd
deferred data                  for every PaintDotNet.MemoryBlock with deferred = true,
                               in the order the blocks appear in the NRBF stream:
  uint8   format               0 = gzip chunks, 1 = raw chunks
  uint32 BE chunk size         262144 in every 3.5+ file seen (1 MiB in 3.0)
  ceil(length / chunk size) x  uint32 BE chunk number, uint32 BE data size, data
                               (chunks may appear in any order; 2 of 489 real files
                               store them out of order)
```

Every gzip chunk is one complete gzip member whose output is exactly
`min(chunk size, length - number * chunk size)` bytes. Paint.NET 4.x/5.x
writes the header bytes `1F 8B 08 00 00000000 00 0A` (mtime 0, XFL 0, OS 10)
for .NET's Optimal level; some 4.x files used Fastest (XFL 4). Pixel data is
BGRA, straight alpha, stride = width x 4, rows top to bottom; transparent
pixels may carry color, which is preserved.

## Object graph (Paint.NET 4.0 to 5.1)

Record sequence of a document written by Paint.NET 5.1.12 (and by our
writer), ids as Paint.NET assigns them (n layers, k metadata items):

```
Header root=1 header=-1
Library 2 "PaintDotNet.Data, Version=V, Culture=neutral, PublicKeyToken=null"
Class 1 PaintDotNet.Document {isDisposed:Boolean, layers:Class(LayerList), width:Int32,
        height:Int32, savedWith:SystemClass(System.Version),
        userMetadataItems:SystemClass(KeyValuePair<string,string>[])}
Class 3 PaintDotNet.LayerList {parent:Class(Document), ArrayList+_items:Object[],
        ArrayList+_size:Int32, ArrayList+_version:Int32}        parent -> 1 (id 6 consumed)
SystemClass 4 System.Version {_Major, _Minor, _Build, _Revision: Int32}
BinaryArray 5 KeyValuePair[k]: SystemClass -8 {key:String, value:String}, ClassWithId -11 ...
ArraySingleObject 7 [capacity 4, 8, 16, ...]: refs to the BitmapLayers, then nulls
Library "PaintDotNet.Core, ..."             (before the first BitmapLayer)
Class PaintDotNet.BitmapLayer {properties:Class(BitmapLayer+BitmapLayerProperties),
        surface:Class(Surface, Core), Layer+isDisposed:Boolean, Layer+width:Int32,
        Layer+height:Int32, Layer+properties:Class(Layer+LayerProperties)}   x n
per layer:
  Class PaintDotNet.BitmapLayer+BitmapLayerProperties {blendOp:Class(UserBlendOps+XBlendOp)}
  Class PaintDotNet.Surface {width, height, stride: Int32, scan0:Class(MemoryBlock, Core)}
  Class PaintDotNet.Layer+LayerProperties {name:String, userMetadataItems:KeyValuePair[],
        visible:Boolean, isBackground:Boolean, opacity:Byte, blendMode:Class(LayerBlendMode)}
        blendMode is an inline value type: Class -37 PaintDotNet.LayerBlendMode {value__:Int32}
then, in queue order: UserBlendOps+XBlendOp {} (per layer), MemoryBlock {length64:Int64,
  hasParent:Boolean, deferred:Boolean} (per layer), the shared empty KeyValuePair[0]
MessageEnd
```

Id rules (verified on 278 real files byte for byte): every object lookup
consumes one id, including lookups of objects that already have one (so
`parent -> 1` consumes 6), except a lookup of the same object as the
previous lookup; value types (KeyValuePair, LayerBlendMode) get the negated
next id; libraries take an id when first needed. Class metadata is written
in full the first time a class name appears and reused with ClassWithId only
while the member types equal those of that first record (so
BitmapLayerProperties is written in full again for every layer whose blend
op class differs from the first layer's). A layer duplicated in Paint.NET
shares the name string and the blend op object of its source.

Document metadata (`userMetadataItems`) holds EXIF entries keyed
`$exif.tagN[0]` with values `<exif id=".." len=".." type=".." value="BASE64" />`
(305 Software, 296 ResolutionUnit, 282/283 X/YResolution RATIONAL, 34675 ICC
profile), and sometimes `$xmp.packetN`.

Older layouts the reader also accepts: 3.x documents carry `userMetaData`
(a .NET NameValueCollection) instead of `userMetadataItems`, no `blendMode`
(the blend mode is the class name of `blendOp`), PaintDotNet 3.0 names its
core assembly `PdnLib`, and 3.0 MemoryBlocks add `bitmapWidth` and
`bitmapHeight`. Non-deferred MemoryBlocks (2.1 beta) carry `pointerData`
(byte[]) inline.

## Reader

`pdn_load_ex` / `pc_codec_pdn.load`:

1. Container: "PDN3", header length within the file, header must start with
   `<pdnImage` (after an optional BOM and whitespace). `savedWithVersion`
   major >= 6 is PC_ERR_UNSUPPORTED (Paint.NET 6.0 announces a new format).
   Other "PDN" + digit magics, the 2.x gzip container and unknown data
   markers are PC_ERR_UNSUPPORTED; truncated or broken containers are
   PC_ERR_FORMAT.
2. NRBF (`nrbf_parse`, X-19): only the records SerializedStreamHeader,
   BinaryLibrary, ClassWithMembersAndTypes, ClassWithMembers,
   SystemClassWithMembersAndTypes, SystemClassWithMembers, ClassWithId,
   BinaryObjectString, MemberReference, ObjectNull, ObjectNullMultiple(256),
   MemberPrimitiveTyped, BinaryArray (single or jagged, rank 1),
   ArraySinglePrimitive, ArraySingleObject, ArraySingleString and MessageEnd.
   Class names must be on the whitelist (the PaintDotNet types above, the 14
   UserBlendOps classes, System.Version, System.Collections.ArrayList, the
   NameValueCollection internals of 3.x files and KeyValuePair<string, ...>);
   library names must start with `PaintDotNet.`, `PdnLib,`, `System,`,
   `mscorlib,` or `System.Private.CoreLib,`. Caps (defaults): 2^18 objects,
   2^21 value slots, nesting depth 32, 256 members per class, 2^14 class
   records, 64 libraries, 16 MiB per string, 2^20 elements per array, and the
   input size. Parsing is iterative with an explicit frame stack; ids must be
   unique, every reference and ClassWithId must resolve, libraries must exist
   before use. Unknown records are PC_ERR_FORMAT, MethodCall/MethodReturn and
   unknown classes PC_ERR_UNSUPPORTED, caps PC_ERR_LIMIT.
3. Typed extraction walks only Document -> LayerList -> items[0.._size) ->
   BitmapLayer -> {LayerProperties, BitmapLayerProperties, Surface ->
   MemoryBlock}. A layer, surface or memory block reached twice is
   PC_ERR_FORMAT, so cyclic or shared graphs cannot alias pixel data. Width,
   height and layer count are checked with `pc_codec_check_size` before any
   pixel memory is allocated. Surface size must equal the document, stride
   must be width x 4 (width x 3 = 24-bit is PC_ERR_UNSUPPORTED: no real file
   uses it), length64 = stride x height, hasParent must be false
   (PC_ERR_UNSUPPORTED otherwise).
4. Blend mode: `LayerBlendMode.value__` 0..13 maps 1:1 to `pc_blend_mode`;
   other values load as Normal with a note in `pc_image_meta.note`. Without
   `blendMode`, the `UserBlendOps+<Name>BlendOp` class name decides.
5. Deferred data: blocks are matched to MemoryBlocks in stream order; blocks
   of memory blocks no layer uses are validated and skipped. Chunk numbers
   must be unique and in range, data sizes inside the file, raw chunks exactly
   the chunk length, gzip chunks must inflate (pc_zlib, CRC checked) to
   exactly the chunk length with no trailing bytes. Output goes through a
   256 KiB window straight into the layer tiles with `pc_layer_store_rect`, so
   no full-layer buffer is ever allocated and all-zero areas stay NULL tiles.
   Chunk sizes that are not a multiple of 4 are supported (a 5-byte record per
   boundary joins the pixel split between two chunks); chunk sizes below 4
   are PC_ERR_UNSUPPORTED. Trailing bytes after the last block are ignored.
6. Layer names are copied as UTF-8, invalid sequences and control characters
   become '?', names longer than 63 bytes are cut at a character boundary
   (note in `pc_image_meta.note`). Resolution comes from EXIF 282/283/296
   (cm converted to inches), the ICC profile from EXIF 34675.

`pdn_info` (tests, diagnostics) reports savedWith, the first block's format
and chunk size, out-of-order chunks, legacy blend ops, isBackground flags,
the ArrayList capacity and name sharing.

## Writer

`pdn_save_ex` / `pc_codec_pdn.save` writes exactly the 5.1 layout above with
`savedWithVersion` and assembly versions `PDN_COMPAT_VERSION`
(5.112.9563.32325, the release whose structure is mirrored; it is a format
compatibility level) and EXIF Software "paint.c". Metadata items:
`$exif.tag0[0]` Software, `tag1` ResolutionUnit (inches), `tag2`/`tag3`
X/YResolution (closest rational with denominator up to 10^6, default 96),
`tag4` ICC profile (type 7) when `meta->icc` is set. isBackground is true
for the bottom layer. Equal layer names share one string record and, when
the blend modes match too, one blend op object (Paint.NET's duplicate
layer). ArrayList capacity follows .NET growth (4, 8, 16, ...), `_version` =
layer count. Pixels: format 0, 256 KiB chunks, each one gzip member at level
6 with header OS = 10 and mtime 0 (byte-identical gzip headers to Paint.NET
5; the deflate bytes differ because .NET uses zlib-ng). Chunks are
compressed in parallel through `pc_par` (batches of up to 32 MiB of input);
the output does not depend on the thread count. The header carries a PNG
thumbnail (longest side 256, other side floored, smaller images kept 1:1, as
in Paint.NET): the composite (`pc_comp_rect`) area-averaged in
premultiplied space, encoded RGB or RGBA with sRGB, gAMA and pHYs chunks
(pdn_png.c, own encoder over pc_zlib).

`pdn_save_opts` (tests and tools): version string, Software, chunk size
(4 .. 64 MiB), level (-1 raw format 1, 0..9 gzip), thumbnail on/off, name
interning, descending chunk order, explicit ArrayList capacity.

## Verification

Commands run on 2026-10-04 (research corpus outside the repository at
/ai/work/paintc-research/pdn-samples: 489 .pdn files from GitHub saved by
Paint.NET 3.0 (3.0.2580) to 5.1.12 (5.112.9563.32325), 1x1 up to 6764 x 168
and 5210 x 6962, 1 to 43 layers, all 14 blend modes, hidden layers,
opacities, gzip chunks out of order):

- Reader vs pypdn 1.0.6 (independent MIT reader):
  `pdn_pypdn_check.py build/tests/codec/test_pdn_dump <489 files>` gives
  `OK 489`: identical size, layer count, names, visibility, opacity, blend
  mode and every pixel.
- Writer read back by pypdn: every file re-saved with
  `test_pdn_dump --resave` and checked again: `OK 489`.
- Round trip read, write, read: identical pixels and layer properties for
  all 489 files (only the stored resolution of 3 files without resolution
  metadata becomes 96 dpi, and centimeter resolutions are written as the
  equal inch value).
- Structure: `test_pdn_dump --compare <489 files>` re-saves each file with
  its own version string and ArrayList capacity and compares structure
  dumps (record kinds, class and library names, member names and types,
  array lengths, object ids, deferred block layout):
  `MATCH 278 LAYOUT 141 SHARED 30 ORDER 2 V3 38 DIFF 0 ERROR 0`.
  MATCH = identical including every object id; LAYOUT = identical except the
  number of metadata items (the original kept EXIF/XMP entries pc_image_meta
  cannot carry); SHARED = the original shares a blend op or name object
  between layers through its edit history; ORDER = the original stores
  chunks out of order; V3 = Paint.NET 3.x documents, which use the older
  layout by design.
- Synthetic files: `test_pdn_dump --emit-synthetic DIR 60` (random sizes,
  1 to 14 layers, every blend mode, UTF-8 and shared names, hidden layers,
  opacities) read by pypdn: `OK 60`.
- Paint.NET itself (non-authoritative per ADR-009): Paint.NET 5.2 beta
  (5.200.9772.9330, the official experimental Wine build) under
  Wine 11.19 + DXVK in Xvfb opened files written by paint.c without error.
  A 3-layer probe (opaque background, hidden opaque red layer, opaque blocks
  with stray color in transparent pixels) displayed at 100% matched our
  `pc_comp_rect` composite exactly in every canvas pixel except the 25
  under the mouse cursor. A 15-layer probe with one strip per blend mode,
  alpha 255/128/40/alternating and opacities 255/200 matched within 1 LSB
  (2 for Reflect) in every strip, so Paint.NET reads the blend modes,
  opacities and visibility we write. Paint.NET 5.1.12 portable did not
  start under Wine 10.0 (DispatcherQueue not implemented), so 5.2 (same
  file format per its release notes) was used.
- CTest (quick mode): test_pdn, test_pdn_nrbf, test_pdn_dump in Release,
  Debug with ASan/UBSan and clang. Full mode (no --quick) runs 240000
  mutated loads in test_pdn.

## Tools

`build/tests/codec/test_pdn_dump` is both a test and the dump tool:

```
test_pdn_dump --dump [--struct] [--keep-ids] FILE...   record dump
test_pdn_dump --resave IN OUT                           load and save with paint.c
test_pdn_dump --compare FILE...                         structure comparison (above)
test_pdn_dump --export FILE PREFIX                      PREFIX.txt + PREFIX.bin for
                                                        tests/codec/pdn_pypdn_check.py
```

## Known gaps

- Document and layer metadata other than resolution and the ICC profile
  (other EXIF tags, XMP packets, per-layer userMetadataItems) is not carried
  through load and save: `pc_image_meta` has no generic metadata field.
- isBackground is not stored in `pc_layer`; the writer sets it on the bottom
  layer only.
- 24-bit surfaces, parent memory blocks, chunk sizes below 4 bytes, the
  Paint.NET 2.x gzip container and Paint.NET 6 documents report
  PC_ERR_UNSUPPORTED.
- Opening our files in Paint.NET 5.1.12 on Windows has not been verified
  (ADR-009, no Windows machine); the Paint.NET 5.2 beta check under Wine
  above is the closest available evidence.
