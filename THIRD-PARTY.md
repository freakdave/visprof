# Third-party notices

libvisprof itself is MIT licensed. See `LICENSE`.

## KallistiOS

Programs linking KOS must include its copyright notice, conditions and
disclaimer with their distribution. Use the notice from the KOS version
you build against.

Quoted verbatim from `doc/license/LICENSE.KOS` in KallistiOS:

> All of the documentation and software included in the KallistiOS Releases
> is copyrighted (C) 1997-2024 by Megan Potter, Lawrence Sebald, and others (as
> noted in each file).
>
> Copyright (C) 1997-2024 KallistiOS Contributors. All rights reserved.
>
> Redistribution and use in source and binary forms, with or without
> modification, are permitted provided that the following conditions
> are met:
> 1. Redistributions of source code must retain the above copyright
>    notice, this list of conditions and the following disclaimer.
> 2. Redistributions in binary form must reproduce the above copyright
>    notice, this list of conditions and the following disclaimer in the
>    documentation and/or other materials provided with the distribution.
> 3. Neither the name of Cryptic Allusion nor the names of its contributors
>    may be used to endorse or promote products derived from this software
>    without specific prior written permission.
>
> THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS ``AS IS'' AND
> ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
> IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
> ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
> FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
> DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
> OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
> HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
> LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
> OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
> SUCH DAMAGE.

The copyright years and the contributor list change between KOS
releases. Check `doc/license/LICENSE.KOS` and `AUTHORS` in the version
you build against, and copy from there.

## The Dreamcast BIOS font

libvisprof reads the font from the console firmware through KOS
`bfont_draw_ex()` during `visprof_init()`. The repository stores no glyph data.
The atlas is built in memory at run time and is not an input asset.
