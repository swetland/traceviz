// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#ifdef __APPLE__
#define SYM(x) _##x
#else
#define SYM(x) x
#endif

