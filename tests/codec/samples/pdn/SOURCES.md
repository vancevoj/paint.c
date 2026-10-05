# .pdn test samples

Real Paint.NET documents used by `tests/codec/test_pdn.c` and
`tests/codec/test_pdn_dump.c`. Only files from repositories whose license
clearly permits redistribution are kept here. The tests skip these files
when they are missing and also run on fully synthetic documents written by
our own writer.

| File | Saved with | Size, layers | Source | License |
|---|---|---|---|---|
| bevy_bar_border.pdn | 5.0.3 (5.3.8488.42200) | 742 x 94, 3 (duplicated layer) | janhohenheim/bevy-jam-3 `resources/bar_border.pdn` | MIT OR Apache-2.0 |
| bevy_health_bar_fill.pdn | 5.0.3 | 742 x 94, 2 | janhohenheim/bevy-jam-3 `resources/health_bar_fill.pdn` | MIT OR Apache-2.0 |
| bevy_posture_bar_fill.pdn | 5.0.3 | 742 x 94, 2 | janhohenheim/bevy-jam-3 `resources/posture_bar_fill.pdn` | MIT OR Apache-2.0 |
| bevy_posture_bar_top.pdn | 5.0.3 | 742 x 94, 2 | janhohenheim/bevy-jam-3 `resources/posture_bar_top.pdn` | MIT OR Apache-2.0 |
| pypdn_Untitled.pdn | 4.0.21 (4.21.6589.7045) | 800 x 600, 1 | addisonElliott/pypdn `tests/data/Untitled.pdn` | MIT |
| pypdn_Untitled2.pdn | 4.0.21 | 800 x 600, 2 | addisonElliott/pypdn `tests/data/Untitled2.pdn` | MIT |
| pypdn_Untitled3.pdn | 4.0.21 | 800 x 600, 2 (Additive, opacity 161) | addisonElliott/pypdn `tests/data/Untitled3.pdn` | MIT |
| pypdn_oldPDN3510.pdn | 3.5.10 (3.510.4297.28964) | 800 x 600, 2 (3.x layout) | addisonElliott/pypdn `tests/data/oldPDN3510.pdn` | MIT |
| pypdn_FlattenBlendTest.pdn | 4.0.21 | 800 x 600, 14 (every blend mode) | addisonElliott/pypdn `tests/data/FlattenBlendTest.pdn` | MIT |

Revisions: pypdn commit 3481b08b3579daf8ab50ecbd03f14619b9c76c8c
(https://github.com/addisonElliott/pypdn), bevy-jam-3 commit
f23d4a333f7dbaec1f8fc9a3b471912d521cbda8
(https://github.com/janhohenheim/bevy-jam-3, assets made by the repository
author per its credits.md; the files were fetched from Git LFS).

SHA-256:

```
88c0eec36e7d16f1ebeefc8493dfdc18b2b7cf33fed626fd6893c02e625f5387  bevy_bar_border.pdn
2266425cd244f20bf9a1582f3dd62567cfb7f9091963fec42c978c4a9d83492b  bevy_health_bar_fill.pdn
0cb7cc8c62495e808f28f92ed4b39a47ffd27b1f1283b4dbe82489183ac6e0b4  bevy_posture_bar_fill.pdn
11dc8c5fee67d7723a1bfac9132bba27ae58c3efe1246ace4c19e871bdb80ee9  bevy_posture_bar_top.pdn
c5589d0bfe4706b783b92d827c66148c22935d67056e7d99e8e5c3081b1cee75  pypdn_FlattenBlendTest.pdn
113e70a0c2e3ce83b4f6533ff7243a5a54b80f31047720e4967087322adf73fd  pypdn_oldPDN3510.pdn
6071e461aaca036fcab27eea96d83355f509780a0631bb55799cf131e4c5c10b  pypdn_Untitled2.pdn
a4cb06a92bba14c56123651530a442bba42ed81c37dfe52bca5f82b7891c92f2  pypdn_Untitled3.pdn
fa84536e53555cfac5306385f95bafe988cedaf701edc5b019043b7480721cfe  pypdn_Untitled.pdn
```

The larger research corpus (489 files from GitHub, many GPL or unlicensed)
is NOT redistributed; it lives outside the repository and is described in
docs/codecs/pdn.md.

## License texts

### pypdn (files `pypdn_*.pdn`)

```
MIT License

Copyright (c) 2018 Addison Elliott

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### bevy-jam-3 (files `bevy_*.pdn`), used under the MIT option

```
MIT License

Copyright (c) Jan Hohenheim <jan@hohenheim.ch>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

(The upstream license-mit.txt carries no copyright line; the holder is the
author named in the repository's Cargo.toml, `authors = ["Jan Hohenheim"]`.)
