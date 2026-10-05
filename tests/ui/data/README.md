# tests/ui font fixtures

Small subsets used by `tests/ui/test_ui_text.c` to exercise the glyf and CFF
loaders, kerning, fallback chains and the validator. They were produced with
fontTools `pyftsubset` (glyph names and layout tables kept, hinting dropped).

| File | Source | Characters | License |
|---|---|---|---|
| `inter-ttf-subset.ttf` | Inter 4.1 `extras/ttf/Inter-Regular.ttf` (same file as `assets/fonts/Inter-Regular.ttf`) | printable ASCII, `ÅéñΩЖ…` | SIL OFL 1.1, Copyright 2016 The Inter Project Authors |
| `inter-cff-subset.otf` | Inter 4.1 `extras/otf/Inter-Regular.otf` from the same release zip (see `assets/fonts/README.md`) | printable ASCII, `ΩАБ` | SIL OFL 1.1, Copyright 2016 The Inter Project Authors |
| `noto-cjk-subset.otf` | Noto Sans CJK JP Regular 2.004 (Debian package `fonts-noto-cjk`, face 0 of `NotoSansCJK-Regular.ttc`) | `Aaあア日木本水語` | SIL OFL 1.1, Copyright 2014-2021 Adobe (http://www.adobe.com/), with Reserved Font Name "Source" |

The subsets are Modified Versions under the OFL. They keep their original
copyright and license strings in the `name` table, are not renamed to a
Reserved Font Name and are distributed only as test fixtures. The license
text is `assets/fonts/OFL.txt` (identical for both families apart from the
copyright line quoted above).
