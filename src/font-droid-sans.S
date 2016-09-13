// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "asm.h"

.global SYM(font_droid_sans)
.global SYM(size_droid_sans)

.data

.align 4
SYM(font_droid_sans):
.incbin "../third_party/imgui/extra_fonts/DroidSans.ttf"
SYM(font_droid_sans_end):

.align 4
SYM(size_droid_sans):
.int SYM(font_droid_sans_end) - SYM(font_droid_sans)

