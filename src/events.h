// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

DEF_EVENT(NONE, 0, "none")
DEF_EVENT(THREAD_START, 1, "thread_start()")
DEF_EVENT(MSGPIPE_CREATE, 2, "msgpipe_create()")
DEF_EVENT(MSGPIPE_WRITE, 3, "msgpipe_write()")
DEF_EVENT(MSGPIPE_READ, 4, "msgpipe_read()")
DEF_EVENT(PORT_WAIT, 5, "port_wait()")
DEF_EVENT(PORT_WAITED, 6, "port_wait() done")
DEF_EVENT(HANDLE_WAIT, 7, "handle_wait()")
DEF_EVENT(HANDLE_WAITED, 8, "handle_wait() done")

#undef DEF_EVENT
