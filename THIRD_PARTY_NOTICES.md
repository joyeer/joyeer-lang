# Third-Party Notices

The Windows `joyeer-native-backend.dll` statically includes portions of LLVM,
LLD, and LibXml2. These dependencies are build-time inputs and are not exposed
through Joyeer's public ABI.

## LLVM and LLD 22.1.8

LLVM and LLD are licensed under the Apache License 2.0 with LLVM Exceptions.
The Apache License 2.0 text is distributed in `LICENSE`.

LLVM Exceptions to the Apache 2.0 License:

> As an exception, if, as a result of your compiling your source code,
> portions of this Software are embedded into an Object form of such source
> code, you may redistribute such embedded portions in such Object form
> without complying with the conditions of Sections 4(a), 4(b) and 4(d) of
> the License.
>
> In addition, if you combine or link compiled forms of this Software with
> software that is licensed under the GPLv2 ("Combined Software") and if a
> court of competent jurisdiction determines that the patent provision
> (Section 3), the indemnity provision (Section 9) or other Section of the
> License conflicts with the conditions of the GPLv2, you may retroactively
> and prospectively choose to deem waived or otherwise exclude such
> Section(s) of the License, but only in their entirety and only with respect
> to the Combined Software.

Project and license information:

- https://llvm.org/
- https://github.com/llvm/llvm-project/blob/llvmorg-22.1.8/llvm/LICENSE.TXT

## rpmalloc

LLVM Support statically includes rpmalloc.

MIT License

Copyright (c) 2017 Mattias Jansson

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

## xxHash

LLVM Support statically includes portions of xxHash.

Copyright (C) 2012-2023, Yann Collet

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

- Redistributions of source code must retain the above copyright notice,
  this list of conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

## LibXml2 2.14.5

Except where otherwise noted in the source code, LibXml2 is distributed under
the following terms:

Copyright (C) 1998-2012 Daniel Veillard. All Rights Reserved.
Copyright (C) The Libxml2 Contributors.

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

Project and license information:

- https://gitlab.gnome.org/GNOME/libxml2
- https://gitlab.gnome.org/GNOME/libxml2/-/blob/v2.14.5/Copyright