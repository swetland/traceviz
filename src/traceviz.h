// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <assert.h>
#include <stdint.h>

int traceviz_main(int argc, char** argv);
int traceviz_render(void);

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


namespace tv {

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

#define KPROC    1 // extra = 0
#define KTHREAD  2 // extra = pid
#define KPIPE    3 // extra = other-pipe-id
#define KPORT    4 // extra = 0

struct Thread;
struct Process;
struct MsgPipe;

struct Object {
    Object* next;
    uint32_t id;
    uint8_t kind;
    uint16_t flags;
    uint32_t creator;

    Object(uint32_t _id, uint32_t _kind);
    virtual Thread* as_thread() { return nullptr; }
    virtual Process* as_process() { return nullptr; }
    virtual MsgPipe* as_msgpipe() { return nullptr; }
    virtual void finish(uint64_t ts) {}
};

enum {
    OBJ_RESOLVED = 1,
};

struct Thread : public Object {
    track_t* track;

    Thread(uint32_t id);
    virtual Thread* as_thread() { return this; }
    virtual void finish(uint64_t ts);
};

struct Process : public Object {
    group_t* group;

    Process(uint32_t id);
    virtual Process* as_process() { return this; }
};

struct MsgPipe : public Object {
    MsgPipe* other;

    MsgPipe(uint32_t id);
    virtual MsgPipe* as_msgpipe() { return this; }
};

typedef struct evtinfo evt_info_t;
typedef struct ktrace_record ktrace_record_t;

#define HASHBITS 10
#define BUCKETS (1 << HASHBITS)

struct Trace {
    group_t* groups;

    Object *objhash[BUCKETS];

    int import(int argc, char** argv);
    int import(int fd);
    void import_regular(ktrace_record_t& rec, uint64_t ts, uint32_t tag);
    void import_special(ktrace_record_t& rec, uint32_t tag);

    void evt_process_name(uint32_t pid, const char* name, uint32_t index);
    void evt_thread_name(uint32_t tid, const char* name);
    void evt_context_switch(uint64_t ts, uint32_t oldtid, uint32_t newtid,
                            uint32_t state, uint32_t cpu,
                            uint32_t oldthread, uint32_t newthread);
    void evt_process_create(uint64_t ts, Thread* t, uint32_t pid);
    void evt_process_delete(uint64_t ts, Thread* t, uint32_t pid);
    void evt_process_start(uint64_t ts, Thread* t, uint32_t pid, uint32_t tid);
    void evt_thread_create(uint64_t ts, Thread* t, uint32_t tid, uint32_t pid);
    void evt_thread_delete(uint64_t ts, Thread* t, uint32_t tid);
    void evt_thread_start(uint64_t ts, Thread* t, uint32_t tid);
    void evt_msgpipe_create(uint64_t ts, Thread* t, uint32_t id, uint32_t otherid);
    void evt_msgpipe_delete(uint64_t ts, Thread* t, uint32_t id);
    void evt_msgpipe_write(uint64_t ts, Thread* t, uint32_t id, uint32_t bytes, uint32_t handles);
    void evt_msgpipe_read(uint64_t ts, Thread* t, uint32_t id, uint32_t bytes, uint32_t handles);
    void evt_port_create(uint64_t ts, Thread* t, uint32_t id);
    void evt_port_wait(uint64_t ts, Thread* t, uint32_t id);
    void evt_port_wait_done(uint64_t ts, Thread* t, uint32_t id);
    void evt_port_delete(uint64_t ts, Thread* t, uint32_t id);
    void evt_wait_one(uint64_t ts, Thread* t, uint32_t id, uint32_t signals, uint64_t timeout);
    void evt_wait_one_done(uint64_t ts, Thread* t, uint32_t id, uint32_t pending, uint32_t status);

    group_t* get_groups(void) {
        return groups;
    }

    Object* find_object(uint32_t id, uint32_t kind);
    Process* find_process(uint32_t id, bool create = true);
    Thread* find_thread(uint32_t id, bool create = true);
    MsgPipe* find_msgpipe(uint32_t id, bool create = true);

    void add_object(Object* object);
    void finish(uint64_t ts);
};

};

extern uint8_t font_droid_sans[];
extern int size_droid_sans;
extern uint8_t font_symbols[];
extern int size_symbols;

