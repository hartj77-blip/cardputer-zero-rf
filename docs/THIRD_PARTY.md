# Third-party code

ZeroOS RF builds on other people's work. This file lists code that is compiled into our programs,
with the notices their licences ask for.

## ft8_lib

FT8 decoding and encoding, by Karlis Goba: https://github.com/kgoba/ft8_lib

It is downloaded at build time (pinned to one commit in `rf/CMakeLists.txt`) and linked into the
radio daemon `zrfd`. It includes KISS FFT by Mark Borgerding (BSD-3-Clause).

```
MIT License

Copyright (c) 2018 Kārlis Goba

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

## Used but not built in

- LVGL (MIT) and the base OS, through the `base/` submodule and its own build.
- librtlsdr (GPL-2.0-or-later), linked dynamically by `zrfd`.
- Programs run as separate processes when installed: rtl_433, rtl_fm, rtl_adsb, rtl_tcp, dump1090,
  multimon-ng, Hamlib's rigctld, sox.
