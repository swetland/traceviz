// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

.global font_symbols
.global size_symbols

.align 4
font_symbols:
.incbin "symbols.ttf"
font_symbols_end:

.align 4
size_symbols:
.int font_symbols_end - font_symbols

