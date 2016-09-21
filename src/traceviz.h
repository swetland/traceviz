// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <assert.h>
#include <stdint.h>

#include <vector>
#include <deque>
#include <map>

int traceviz_main(int argc, char** argv);
int traceviz_render(void);

#define KTRACE_DEF(num,type,name,group) EVT_##name = num,
enum {
#include "ktrace-def.h"
};

#define EVT_PROBE 0x800

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

struct Group;
struct Track;
struct Event;
struct TaskState;

struct Group {
    Group* next;
    Track* first;
    Track* last;
    const char* name;
    uint32_t flags;
};

#define GRP_FOLDED 1

struct TaskState {
    int64_t ts;
    uint8_t state;
    uint8_t cpu;
};

static inline bool operator<(const TaskState& task, int64_t ts) {
    return task.ts < ts;
}

struct Event {
    int64_t ts;
    uint16_t tag;
    uint16_t trackidx;
    uint32_t eventidx;
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
};

static inline bool operator<(const Event& event, int64_t ts) {
    return event.ts < ts;
}

struct Track {
    Track* next;
    std::vector<TaskState> task;
    std::vector<Event> event;
    const char* name;
    uint16_t idx;
    float y;
};


static_assert(sizeof(Event) == 32, "sizeof(Event) != 32");

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
    Track* track;

    Thread(uint32_t id);
    virtual Thread* as_thread() { return this; }
    virtual void finish(uint64_t ts);
};

struct Process : public Object {
    Group* group;

    Process(uint32_t id);
    virtual Process* as_process() { return this; }
};

struct Msg {
    uint16_t trackidx;
    uint32_t eventidx;
};

struct MsgPipe : public Object {
    MsgPipe* other;
    std::deque<Msg> msgs;
    MsgPipe(uint32_t id);
    virtual MsgPipe* as_msgpipe() { return this; }
};

typedef struct evtinfo evt_info_t;
typedef union ktrace_record ktrace_record_t;

#define HASHBITS 10
#define BUCKETS (1 << HASHBITS)

#define MAXCPU 32

struct Trace {
    std::vector<Track*> tracks;
    std::map<uint32_t,const char*> syscall_names;
    std::map<uint32_t,const char*> probe_names;
    Group* group_list;
    Group* group_last;

    Object *objhash[BUCKETS];

    Thread* kthread_list;

    Track* get_track(unsigned n) {
        return tracks[n];
    }
    void add_track(Track* track) {
        track->idx = tracks.size();
        tracks.push_back(track);
    }

    Thread* active[MAXCPU];

    int import(int argc, char** argv);
    int import(int fd);
    void import_event(ktrace_record_t& rec, uint32_t evt);

    void evt_syscall_name(uint32_t num, const char* name);
    void evt_probe_name(uint32_t num, const char* name);
    void evt_process_name(uint32_t pid, const char* name, uint32_t index);
    void evt_thread_name(uint32_t tid, uint32_t pid, const char* name);
    void evt_kthread_name(uint32_t tid, const char* name);
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
    void evt_irq_enter(uint64_t ts, uint32_t cpu, uint32_t irqn);
    void evt_syscall_enter(uint64_t ts, uint32_t cpu, uint32_t num);
    void evt_syscall_exit(uint64_t ts, uint32_t cpu, uint32_t num);
    void evt_probe(uint64_t ts, Thread* t, uint32_t evt, uint32_t arg0, uint32_t arg1);
    Group* get_groups(void) {
        return group_list;
    }

    Object* find_object(uint32_t id, uint32_t kind);
    Process* find_process(uint32_t id, bool create = true);
    Thread* find_thread(uint32_t id, bool create = true);
    MsgPipe* find_msgpipe(uint32_t id, bool create = true);

    Thread* find_kthread(uint32_t id, bool create = true);

    Group* group_create(void);
    void group_add_track(Group* group, Track* track);
    Track* track_create(void);
    static void track_append(Track* t, uint64_t ts, uint8_t state, uint8_t cpu);
    static Event* track_add_event(Track* t, uint64_t ts, uint32_t tag);

    const char* syscall_name(uint32_t num) {
        return syscall_names[num];
    }
    const char* probe_name(uint32_t evt) {
        return probe_names[evt];
    }

    void add_object(Object* object);
    void finish(uint64_t ts);
};

};

extern uint8_t font_droid_sans[];
extern int size_droid_sans;
extern uint8_t font_symbols[];
extern int size_symbols;

