// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

.global font_droid_sans
.global size_droid_sans

.align 4
font_droid_sans:
.incbin "../third_party/imgui/extra_fonts/DroidSans.ttf"
font_droid_sans_end:

.align 4
size_droid_sans:
.int font_droid_sans_end - font_droid_sans

