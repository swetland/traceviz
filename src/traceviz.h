// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <assert.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int traceviz_main(int argc, char** argv);
int traceviz_render(void);

int ktrace_main(int argc, char** argv);

// these need to match the Magenta Kernel
#define TS_SUSPENDED 0
#define TS_READY 1
#define TS_RUNNING 2
#define TS_BLOCKED 3
#define TS_SLEEPING 4
#define TS_DEAD 5

#define TS_NONE 6
#define TS_LAST TS_NONE

typedef struct group group_t;
typedef struct track track_t;
typedef struct event event_t;
typedef struct taskstate taskstate_t;

struct taskstate {
    int64_t ts;
    uint8_t state;
    uint8_t cpu;
};

enum {
    EVT_NONE,
    EVT_THREAD_START,
    EVT_THREAD_STOP,
    EVT_MSGPIPE_CREATE,
    EVT_MSGPIPE_WRITE,
    EVT_MSGPIPE_READ,
    EVT_PORT_WAIT,
    EVT_PORT_WAITED,
    EVT_HANDLE_WAIT,
    EVT_HANDLE_WAITED,
};

struct event {
    int64_t ts;
    uint16_t tag;
    uint16_t reftrack;
    uint32_t refevent;
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
};

static_assert(sizeof(event_t) == 32, "sizeof(event_t) != 32");

struct group {
    group_t* next;
    track_t* first;
    track_t* last;
    const char* name;
    uint32_t flags;
};

#define GRP_FOLDED 1

struct track {
    track_t* next;
    taskstate_t* task;
    event_t* event;
    const char* name;
    unsigned taskcount;
    unsigned tasksize;
    unsigned eventcount;
    unsigned eventsize;
};

void add_groups(group_t* list);

extern uint8_t font_droid_sans[];
extern int size_droid_sans;
extern uint8_t font_symbols[];
extern int size_symbols;

#ifdef __cplusplus
}
#endif
