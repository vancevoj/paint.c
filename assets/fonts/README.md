# Embedded UI font

- Family: Inter 4.1 (Regular and SemiBold, static TrueType outlines)
- Upstream: https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip
  (SHA-256 of the zip: 9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e)
- Files are unmodified copies of `extras/ttf/Inter-Regular.ttf` and
  `extras/ttf/Inter-SemiBold.ttf` from that archive:
  - Inter-Regular.ttf  40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82
  - Inter-SemiBold.ttf 78a843fade9d4612a5567302fb595b56976eb5fcebf4fea5a5912d638bafcde3
- License: SIL Open Font License 1.1, no Reserved Font Name (OFL.txt).
- Coverage: Latin (incl. extended), Greek, Cyrillic, punctuation, arrows and
  common symbols. Kerning lives in GPOS (including extension lookups).

cmake/PcEmbed.cmake turns each file into a C byte array at build time
(target pc_ui); nothing is generated into the source tree.
