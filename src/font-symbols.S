// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "asm.h"

.global SYM(font_symbols)
.global SYM(size_symbols)

.align 4
SYM(font_symbols):
.incbin "symbols.ttf"
SYM(font_symbols_end):

.align 4
SYM(size_symbols):
.int SYM(font_symbols_end) - SYM(font_symbols)

