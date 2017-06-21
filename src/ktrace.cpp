// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// for strdup
#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ktrace.h"

#include "traceviz.h"

namespace tv {

#define FNV32_PRIME (16777619)
#define FNV32_OFFSET_BASIS (2166136261)

#define exit(n) ( *((int*) 0) = (n) )
// for bits 0..15
static inline uint32_t fnv1a_tiny(uint32_t n, uint32_t bits) {
    uint32_t hash = FNV32_OFFSET_BASIS;
    hash = (hash ^ (n & 0xFF)) * FNV32_PRIME; n >>= 8;
    hash = (hash ^ (n & 0xFF)) * FNV32_PRIME; n >>= 8;
    hash = (hash ^ (n & 0xFF)) * FNV32_PRIME; n >>= 8;
    hash = (hash ^ n) * FNV32_PRIME;
    return ((hash >> bits) ^ hash) & ((1 << bits) - 1);
}

Group* group_kernel;

Group* Trace::group_create(void) {
    Group* g = (Group*)calloc(1, sizeof(Group));
    g->name = "unknown";
    if (group_last) {
        group_last->next = g;
    } else {
        group_list = g;
    }
    group_last = g;
    return g;
}

void Trace::group_add_track(Group* group, Track* track) {
    track->next = nullptr;
    if (group->last) {
        group->last->next = track;
    } else {
        group->first = track;
    }
    group->last = track;
}

Track* Trace::track_create(void) {
    Track* t = (Track*)calloc(1, sizeof(Track));
    t->name = "unknown";
    add_track(t);
    return t;
}

void Trace::track_append(Track* t, uint64_t ts, uint8_t state, uint8_t cpu) {
    TaskState task;
    task.ts = ts;
    task.state = state;
    task.cpu = cpu;
    t->task.push_back(task);
}

Event* Trace::track_add_event(Track* t, uint64_t ts, uint32_t tag) {
    Event event;
    memset(&event, 0, sizeof(event));
    event.ts = ts;
    event.tag = tag;
    t->event.push_back(event);
    return &t->event.back();
}

#define OBJBUCKET(id) fnv1a_tiny(id, HASHBITS)

const char* kind_string(uint32_t kind) {
    switch (kind) {
    case KPROC:   return "PROC";
    case KTHREAD: return "THRD";
    case KPIPE:   return "MPIP";
    case KPORT:   return "PORT";
    default:      return "NVLD";
    }
}

Object* Trace::find_object(uint32_t id, uint32_t kind) {
    for (Object* oi = objhash[OBJBUCKET(id)]; oi != NULL; oi = oi->next) {
        if (oi->id == id) {
            if (kind && (oi->kind != kind)) {
                fprintf(stderr, "error: object(%08x) is %s not %s\n",
                        id, kind_string(oi->kind), kind_string(kind));
                exit(1);
            }
            return oi;
        }
    }
    return NULL;
}

Object::Object(uint32_t _id, uint32_t _kind) : id(_id), kind(_kind), flags(0) {
};

Thread::Thread(uint32_t _id) : Object(_id, KTHREAD) {
}

void Thread::finish(uint64_t ts) {
    Trace::track_append(track, ts, TS_NONE, 0);
}

Process::Process(uint32_t _id) : Object(_id, KPROC) {
}

MsgPipe::MsgPipe(uint32_t _id) : Object(_id, KPIPE), other(nullptr) {
}

void Trace::add_object(Object* object) {
    unsigned n = OBJBUCKET(object->id);
    object->next = objhash[n];
    objhash[n] = object;
}

Process* Trace::find_process(uint32_t id, bool create) {
    Object* o = find_object(id, KPROC);
    if (o != nullptr) {
        return o->as_process();
    }
    if (create) {
        Process* p = new Process(id);
        p->group = group_create();
        add_object(p);
        return p;
    }
    return nullptr;
}

Thread* Trace::find_thread(uint32_t id, bool create) {
    Object* o = find_object(id, KTHREAD);
    if (o != nullptr) {
        return o->as_thread();
    }
    if (create) {
        Thread* t = new Thread(id);
        t->track = track_create();
        add_object(t);
        return t;
    }
    return nullptr;
}

MsgPipe* Trace::find_msgpipe(uint32_t id, bool create) {
    Object* o = find_object(id, KPIPE);
    if (o != nullptr) {
        return o->as_msgpipe();
    }
    if (create) {
        MsgPipe* p = new MsgPipe(id);
        add_object(p);
        return p;
    }
    return nullptr;
}

void Trace::finish(uint64_t ts) {
    for (unsigned n = 0; n < BUCKETS; n++) {
        for (Object* obj = objhash[n]; obj != NULL; obj = obj->next) {
            obj->finish(ts);
        }
    }
    for (Object* obj = kthread_list; obj != nullptr; obj = obj->next) {
        obj->finish(ts);
    }
}

// kthread ids are their kvaddrs and may collide with koids
// but there are usually only a handful of these, so just use
// a simple linked list
Thread* Trace::find_kthread(uint32_t id, bool create) {
    Object* o;
    for (o = kthread_list; o != nullptr; o = o->next) {
        if (o->id == id) {
            return o->as_thread();
        }
    }

    if (!create) {
        return nullptr;
    }

    // create new kernel thread
    Thread* t = new Thread(id);
    t->track = track_create();
    t->next = kthread_list;
    kthread_list = t;

    // add it to the kernel "process"
    Process* p = find_process(0);
    group_add_track(p->group, t->track);
    t->flags |= OBJ_RESOLVED;
    return t;
}

static uint64_t ticks_per_ms;

uint64_t ticks_to_ts(uint64_t ts) {
    //TODO: handle overflow for large times
    if (ticks_per_ms) {
        return (ts * 1000000ULL) / ticks_per_ms;
    } else {
        return 0;
    }
}

const char* recname(ktrace_rec_name_t& rec) {
    uint32_t len = KTRACE_LEN(rec.tag);
    if (len < (KTRACE_NAMESIZE + 1)) {
        return "ERROR";
    }
    len -= (KTRACE_NAMESIZE - 1);
    rec.name[len] = 0;
    return rec.name;
}

int verbose = 0;
int text = 0;

void Trace::evt_context_switch(uint64_t ts, uint32_t oldtid, uint32_t newtid,
                               uint32_t state, uint32_t cpu,
                               uint32_t oldthread, uint32_t newthread) {
    Thread* t;
    if (oldtid) {
        t = find_thread(oldtid);
    } else {
        t = find_kthread(oldthread);
    }
    track_append(t->track, ts, state, cpu);

    if (newtid) {
        t = find_thread(newtid);
    } else {
        t = find_kthread(newthread);
    }
    track_append(t->track, ts, TS_RUNNING, cpu);

    if (cpu >= MAXCPU) {
        return;
    }
    active[cpu] = t;
}

void Trace::evt_syscall_name(uint32_t num, const char* name) {
    syscall_names[num] = strdup(name);
}

void Trace::evt_probe_name(uint32_t num, const char* name) {
    probe_names[num + EVT_PROBE] = strdup(name);
}

void Trace::evt_process_create(uint64_t ts, Thread* t, uint32_t pid) {
    Process* p = find_process(pid);
    if (p->flags & OBJ_RESOLVED) {
        fprintf(stderr, "error: process %08x already created\n", pid);
        exit(1);
    }
    p->flags |= OBJ_RESOLVED;
    p->creator = t->id;
}
void Trace::evt_process_delete(uint64_t ts, Thread* t, uint32_t pid) {
}
void Trace::evt_process_start(uint64_t ts, Thread* t, uint32_t pid, uint32_t tid) {
}
void Trace::evt_process_name(uint32_t pid, const char* name, uint32_t index) {
    Process* p = find_process(pid);
    p->group->name = strdup(name);
}

void Trace::evt_thread_create(uint64_t ts, Thread* ct, uint32_t tid, uint32_t pid) {
    Thread* t = find_thread(tid);
    if (t->flags & OBJ_RESOLVED) {
        fprintf(stderr, "error: thread %08x already created\n", tid);
        return;
    }
    Process* p = find_process(pid);
    group_add_track(p->group, t->track);
    t->flags |= OBJ_RESOLVED;
}
void Trace::evt_thread_delete(uint64_t ts, Thread* t, uint32_t tid) {
}
void Trace::evt_thread_start(uint64_t ts, Thread* t, uint32_t tid) {
}
void Trace::evt_thread_name(uint32_t tid, uint32_t pid, const char* name) {
    char tmp[128];
    sprintf(tmp, "%s (%u)", name, tid);
    Thread* t = find_thread(tid);
    t->track->name = strdup(tmp);

    // if thread is not created, it must be already running
    // so we'll create it retroactively
    if (!(t->flags & OBJ_RESOLVED)) {
        Process* p = find_process(pid);
        group_add_track(p->group, t->track);
        t->flags |= OBJ_RESOLVED;
    }
}

void Trace::evt_kthread_name(uint32_t tid, const char* name) {
    Thread* t = find_kthread(tid);
    t->track->name = strdup(name);
}

void Trace::evt_msgpipe_create(uint64_t ts, Thread* t, uint32_t id, uint32_t otherid) {
    MsgPipe* p0 = find_msgpipe(id);
    MsgPipe* p1 = find_msgpipe(otherid);
    if (p0->flags & OBJ_RESOLVED) {
        fprintf(stderr, "error: msgpipe %08x already created\n", id);
        exit(1);
    }
    if (p1->flags & OBJ_RESOLVED) {
        fprintf(stderr, "error: msgpipe %08x already created\n", otherid);
        exit(1);
    }
    p0->flags |= OBJ_RESOLVED;
    p0->creator = t->id;
    p0->other = p1;
    p1->flags |= OBJ_RESOLVED;
    p1->creator = t->id;
    p1->other = p0;
    track_add_event(t->track, ts, EVT_MSGPIPE_CREATE);
}

void Trace::evt_msgpipe_delete(uint64_t ts, Thread* t, uint32_t id) {
}
void Trace::evt_msgpipe_write(uint64_t ts, Thread* t, uint32_t id, uint32_t bytes, uint32_t handles) {
    MsgPipe* pipe = find_msgpipe(id);

    Msg m;
    m.trackidx = t->track->idx;
    m.eventidx = t->track->event.size();

    Event* evt = track_add_event(t->track, ts, EVT_MSGPIPE_WRITE);
    evt->a = bytes;
    evt->b = handles;

    MsgPipe* other;
    if ((other = pipe->other) != nullptr) {
        if (other->msgs.size()) return;
        //fprintf(stderr, "WR %p(%u,%u) -> %p %d\n", pipe, m.trackidx, m.eventidx, other, other->msgs.size());
        other->msgs.push_back(m);
    }
}

void Trace::evt_msgpipe_read(uint64_t ts, Thread* t, uint32_t id, uint32_t bytes, uint32_t handles) {
    MsgPipe* pipe = find_msgpipe(id);
    Event* evt = track_add_event(t->track, ts, EVT_MSGPIPE_READ);
    evt->a = bytes;
    evt->b = handles;

    if (pipe->msgs.size()) {
        auto msg = pipe->msgs.front();
        //MsgPipe* other = pipe->other;
        //fprintf(stderr, "RD %p <- %p(%u,%u) %d\n", pipe, other, msg.trackidx, msg.eventidx, pipe->msgs.size());
        evt->trackidx = msg.trackidx;
        evt->eventidx = msg.eventidx;
        pipe->msgs.pop_front();
    }
}

void Trace::evt_port_create(uint64_t ts, Thread* t, uint32_t id) {
}
void Trace::evt_port_wait(uint64_t ts, Thread* t, uint32_t id) {
    track_add_event(t->track, ts, EVT_PORT_WAIT);
}
void Trace::evt_port_wait_done(uint64_t ts, Thread* t, uint32_t id) {
    track_add_event(t->track, ts, EVT_PORT_WAIT_DONE);
}
void Trace::evt_port_delete(uint64_t ts, Thread* t, uint32_t id) {
}

void Trace::evt_wait_one(uint64_t ts, Thread* t, uint32_t id, uint32_t signals, uint64_t timeout) {
    track_add_event(t->track, ts, EVT_WAIT_ONE);
}
void Trace::evt_wait_one_done(uint64_t ts, Thread* t, uint32_t id, uint32_t pending, uint32_t status) {
    track_add_event(t->track, ts, EVT_WAIT_ONE_DONE);
}

void Trace::evt_irq_enter(uint64_t ts, uint32_t cpu, uint32_t irqn) {
    if (cpu >= MAXCPU) {
        return;
    }
    Thread* t = active[cpu];
    if (t != nullptr) {
        Event* evt = track_add_event(t->track, ts, EVT_IRQ_ENTER);
        evt->a = cpu;
        evt->b = irqn;
    }
}

void Trace::evt_irq_exit(uint64_t ts, uint32_t cpu, uint32_t irqn) {
    if (cpu >= MAXCPU) {
        return;
    }
    Thread* t = active[cpu];
    if (t != nullptr) {
        Event* evt = track_add_event(t->track, ts, EVT_IRQ_EXIT);
        evt->a = cpu;
        evt->b = irqn;
    }
}

void Trace::evt_page_fault(uint64_t ts, uint64_t address, uint32_t flags, uint32_t cpu) {
    Thread* t = active[cpu];
    if (t != nullptr) {
        Event* evt = track_add_event(t->track, ts, EVT_PAGE_FAULT);
        evt->a = (address >> 32) & 0xffffffff;
        evt->b = address & 0xffffffff;
        evt->c = flags;
    }
}

void Trace::evt_syscall_enter(uint64_t ts, uint32_t cpu, uint32_t num) {
    if (cpu >= MAXCPU) {
        return;
    }
    Thread* t = active[cpu];
    if (t != nullptr) {
        Event* evt = track_add_event(t->track, ts, EVT_SYSCALL_ENTER);
        evt->a = num;
    }
}

void Trace::evt_syscall_exit(uint64_t ts, uint32_t cpu, uint32_t num) {
    if (cpu >= MAXCPU) {
        return;
    }
    Thread* t = active[cpu];
    if (t != nullptr) {
        Event* evt = track_add_event(t->track, ts, EVT_SYSCALL_EXIT);
        evt->a = num;
    }
}

void Trace::evt_probe(uint64_t ts, Thread* t, uint32_t evt, uint32_t arg0, uint32_t arg1) {
    Event* e = track_add_event(t->track, ts, evt);
    e->a = arg0;
    e->b = arg1;
}

typedef struct {
    uint64_t ts_first;
    uint64_t ts_last;
    uint32_t events;
    uint32_t context_switch;
    uint32_t msgpipe_new;
    uint32_t msgpipe_del;
    uint32_t msgpipe_write;
    uint32_t msgpipe_read;
    uint32_t thread_new;
    uint32_t thread_del;
    uint32_t process_new;
    uint32_t process_del;
} stats_t;

void dump_stats(stats_t* s) {
    fprintf(stderr, "-----------------------------------------\n");
    uint64_t duration = s->ts_last - s->ts_first;
    fprintf(stderr, "elapsed time:     %lu.%06lu s\n",
            duration / 1000000UL, duration % 1000000UL);
    fprintf(stderr, "total events:     %u\n", s->events);
    fprintf(stderr, "context switches: %u\n", s->context_switch);
    fprintf(stderr, "msgpipe created:  %u\n", s->msgpipe_new);
    fprintf(stderr, "msgpipe deleted:  %u\n", s->msgpipe_del);
    fprintf(stderr, "msgpipe writes:   %u\n", s->msgpipe_write);
    fprintf(stderr, "msgpipe reads:    %u\n", s->msgpipe_read);
    fprintf(stderr, "thread created:   %u\n", s->thread_new);
    fprintf(stderr, "process created:  %u\n", s->process_new);
}

static stats_t s;

#define trace(fmt...) do { if(text) fprintf(stderr, fmt); } while (0)

typedef union ktrace_record {
    ktrace_header_t hdr;
    ktrace_rec_32b_t x4;
    ktrace_rec_name_t name;
    uint8_t raw[256];
} ktrace_record_t;

static inline void tracehdr(uint64_t ts, uint32_t id) {
   trace("%04lu.%09lu [%08x] ", ts/(1000000000UL), ts%(1000000000UL), id);
}

void Trace::import_event(ktrace_record_t& rec, uint32_t evt) {
    // only valid if the sub-header actually uses this field
    uint64_t ts = ticks_to_ts(rec.hdr.ts);
    bool ts_valid = true;

    switch (evt) {
    case EVT_VERSION:
        tracehdr(0, 0);
        ts_valid = false;
        trace("VERSION      n=%08x\n", rec.x4.a);
        return;
    case EVT_TICKS_PER_MS:
        ticks_per_ms = ((uint64_t)rec.x4.a) | (((uint64_t)rec.x4.b) << 32);
        tracehdr(0, 0);
        ts_valid = false;
        trace("TICKS_PER_MS n=%lu\n", ticks_per_ms);
        return;
    case EVT_CONTEXT_SWITCH:
        s.context_switch++;
        tracehdr(ts, rec.hdr.tid);
        trace("CTXT_SWITCH to=%08x st=%d cpu=%d old=%08x new=%08x\n",
              rec.x4.a, rec.x4.b >> 16, rec.x4.b & 0xFFFF, rec.x4.c, rec.x4.d);
        evt_context_switch(ts, rec.x4.tid, rec.x4.a, rec.x4.b >> 16, rec.x4.b & 0xFFFF, rec.x4.c, rec.x4.d);
        s.ts_last = ts;    s.ts_last = ts;
        return;
    case EVT_PROC_NAME:
        tracehdr(0, 0);
        ts_valid = false;
        trace("PROC_NAME   id=%08x '%s'\n", rec.name.id, recname(rec.name));
        evt_process_name(rec.name.id, recname(rec.name), 10);
        return;
    case EVT_THREAD_NAME:
        tracehdr(0, 0);
        ts_valid = false;
        trace("THRD_NAME   id=%08x '%s'\n", rec.name.id, recname(rec.name));
        evt_thread_name(rec.name.id, rec.name.arg, recname(rec.name));
        return;
    case EVT_KTHREAD_NAME:
        tracehdr(0, 0);
        ts_valid = false;
        trace("THRD_NAME   id=%08x '%s'\n", rec.name.id, recname(rec.name));
        evt_kthread_name(rec.name.id, recname(rec.name));
        return;
    case EVT_SYSCALL_NAME:
        tracehdr(0, 0);
        ts_valid = false;
        trace("SYSCALLNAME id=%08x '%s'\n", rec.name.id, recname(rec.name));
        evt_syscall_name(rec.name.id, recname(rec.name));
        return;
    case EVT_PROBE_NAME:
        tracehdr(0, 0);
        ts_valid = false;
        trace("PROBE_NAME id=%08x '%s'\n", rec.name.id, recname(rec.name));
        evt_probe_name(rec.name.id, recname(rec.name));
        return;
    case EVT_IRQ_ENTER:
        tracehdr(ts, 0);
        trace("IRQ_ENTER   cpu=%03d irqn=%05d\n", rec.hdr.tid & 0xFF, rec.hdr.tid >> 8);
        evt_irq_enter(ts, rec.hdr.tid & 0xFF, rec.hdr.tid >> 8);
        return;
    case EVT_IRQ_EXIT:
        tracehdr(ts, 0);
        trace("IRQ_EXIT   cpu=%03d irqn=%05d\n", rec.hdr.tid & 0xFF, rec.hdr.tid >> 8);
        evt_irq_exit(ts, rec.hdr.tid & 0xFF, rec.hdr.tid >> 8);
        return;
    case EVT_SYSCALL_ENTER:
        tracehdr(ts, 0);
        trace("SYSCALL     cpu=%03d n=%05d\n", rec.hdr.tid & 0xFF, rec.hdr.tid >> 8);
        evt_syscall_enter(ts, rec.hdr.tid & 0xFF, rec.hdr.tid >> 8);
        return;
    case EVT_SYSCALL_EXIT:
        tracehdr(ts, 0);
        trace("SYSCALL_RET cpu=%03d n=%05d\n", rec.hdr.tid & 0xFF, rec.hdr.tid >> 8);
        evt_syscall_exit(ts, rec.hdr.tid & 0xFF, rec.hdr.tid >> 8);
        return;
    case EVT_PAGE_FAULT: {
        tracehdr(ts, 0);
        uint64_t address = ((uint64_t)rec.x4.a << 32) | rec.x4.b;
        trace("PAGE_FAULT address %016lx flags %08x cpu=%03d\n", address, rec.x4.c, rec.x4.d);
        evt_page_fault(ts, address, rec.x4.c, rec.x4.d);
        return;
    }
    default:
        // events before 0x100 do not have the common tag/tid/ts header
        // so bail here instead of in the later switch
        if (evt < 0x100) {
            tracehdr(0, 0);
            trace("UNKNOWN_EVT tag=%08x evt=%03x\n", rec.hdr.tag, evt);
            return;
        }
        break;
    }

    if (rec.hdr.tid == 0) {
        // ignore kernel threads except for context switches
        return;
    }
    Thread* t = find_thread(rec.hdr.tid);

    tracehdr(ts, rec.hdr.tid);
    switch (evt) {
    case EVT_OBJECT_DELETE:
        Object* oi;
        if ((oi = find_object(rec.x4.a, 0)) == 0) {
            trace("OBJT_DELETE id=%08x\n", rec.x4.a);
        } else {
            trace("%s_DELETE id=%08x\n", kind_string(oi->kind), rec.x4.a);
            switch (oi->kind) {
            case KPIPE:
                s.msgpipe_del++;
                evt_msgpipe_delete(ts, t, rec.x4.a);
                break;
            case KTHREAD:
                s.thread_del++;
                evt_thread_delete(ts, t, rec.x4.a);
                break;
            case KPROC:
                s.process_del++;
                evt_process_delete(ts, t, rec.x4.a);
                break;
            case KPORT:
                evt_port_delete(ts, t, rec.x4.a);
                break;
            }
        }
        break;
    case EVT_PROC_CREATE:
        s.process_new++;
        trace("PROC_CREATE id=%08x\n", rec.x4.a);
        evt_process_create(ts, t, rec.x4.a);
        break;
    case EVT_PROC_START:
        trace("PROC_START  id=%08x tid=%08x\n", rec.x4.b, rec.x4.a);
        evt_process_start(ts, t, rec.x4.b, rec.x4.a);
        break;
    case EVT_THREAD_CREATE:
        s.thread_new++;
        trace("THRD_CREATE id=%08x pid=%08x\n", rec.x4.a, rec.x4.b);
        evt_thread_create(ts, t, rec.x4.a, rec.x4.b);
        break;
    case EVT_THREAD_START:
        trace("THRD_START  id=%08x\n", rec.x4.a);
        evt_thread_start(ts, t, rec.x4.a);
        break;
    case EVT_MSGPIPE_CREATE:
        s.msgpipe_new += 2;
        trace("MPIP_CREATE id=%08x other=%08x flags=%x\n", rec.x4.a, rec.x4.b, rec.x4.c);
        evt_msgpipe_create(ts, t, rec.x4.a, rec.x4.b);
        break;
    case EVT_MSGPIPE_WRITE:
        s.msgpipe_write++;
        trace("MPIP_WRITE  id=%08x bytes=%d handles=%d\n", rec.x4.a, rec.x4.b, rec.x4.c);
        evt_msgpipe_write(ts, t, rec.x4.a, rec.x4.b, rec.x4.c);
        break;
    case EVT_MSGPIPE_READ:
        s.msgpipe_read++;
        trace("MPIP_READ   id=%08x bytes=%d handles=%d\n", rec.x4.a, rec.x4.b, rec.x4.c);
        evt_msgpipe_read(ts, t, rec.x4.a, rec.x4.b, rec.x4.c);
        break;
    case EVT_PORT_CREATE:
        trace("PORT_CREATE id=%08x\n", rec.x4.a);
        evt_port_create(ts, t, rec.x4.a);
        break;
    case EVT_PORT_QUEUE:
        trace("PORT_QUEUE  id=%08x\n", rec.x4.a);
        break;
    case EVT_PORT_WAIT:
        trace("PORT_WAIT   id=%08x\n", rec.x4.a);
        evt_port_wait(ts, t, rec.x4.a);
        break;
    case EVT_PORT_WAIT_DONE:
        trace("PORT_WDONE  id=%08x\n", rec.x4.a);
        evt_port_wait_done(ts, t, rec.x4.a);
        break;
    case EVT_WAIT_ONE: {
        uint64_t timeout = ((uint64_t)rec.x4.c) | (((uint64_t)rec.x4.d) << 32);
        trace("WAIT_ONE    id=%08x signals=%08x timeout=%lu\n", rec.x4.a, rec.x4.b, timeout);
        evt_wait_one(ts, t, rec.x4.a, rec.x4.b, timeout);
        break;
    }
    case EVT_WAIT_ONE_DONE:
        trace("WAIT_DONE   id=%08x pending=%08x result=%08x\n", rec.x4.a, rec.x4.b, rec.x4.c);
        evt_wait_one_done(ts, t, rec.x4.a, rec.x4.b, rec.x4.c);
        break;
    default:
        if (evt >= EVT_PROBE) {
            if (KTRACE_LEN(rec.hdr.tag) == 16) {
                evt_probe(ts, t, evt, 0, 0);
                break;
            } else if (KTRACE_LEN(rec.hdr.tag) == 24) {
                evt_probe(ts, t, evt, rec.x4.a, rec.x4.b);
                break;
            }
        }
        trace("UNKNOWN_EVT id=%08x tag=%08x evt=%03x\n", rec.hdr.tid, rec.hdr.tag, evt);
        break;
    }

    // save the first time stamp in the system so we can set the starting point
    if (ts_valid && first_timestamp == 0) {
        first_timestamp = ts;
    }
}

static int show_stats = 0;
static unsigned limit = 0xFFFFFFFF;

void adjust_tracks(Group* groups) {
    int64_t tszero = 0x7FFFFFFFFFFFFFFFUL;
    for (Group* g = groups; g != NULL; g = g->next) {
        for (Track* t = g->first; t != NULL; t = t->next) {
            if (t->task[1].ts < tszero) {
                tszero = t->task[1].ts;
            }
        }
    }
    for (Group* g = groups; g != NULL; g = g->next) {
        for (Track* t = g->first; t != NULL; t = t->next) {
            for (auto& task : t->task) {
                task.ts -= tszero;
            }
            for (auto& event : t->event) {
                event.ts -= tszero;
            }
        }
    }
}

int Trace::import(int fd) {
    ktrace_record_t rec;
    unsigned offset = 0;
    memset(&s, 0, sizeof(s));

    evt_process_name(0, "Magenta Kernel", 0);

    while (read(fd, rec.raw, sizeof(ktrace_header_t)) == sizeof(ktrace_header_t)) {
        uint32_t tag = rec.hdr.tag;
        uint32_t len = KTRACE_LEN(tag);
        if (tag == 0) {
            fprintf(stderr, "eof: zero tag at offset %08x\n", offset);
            break;
        }
        if (len < sizeof(ktrace_header_t)) {
            fprintf(stderr, "eof: short packet at offset %08x\n", offset);
            break;
        }
        offset += (sizeof(ktrace_header_t) + len);
        len -= sizeof(ktrace_header_t);
        if (read(fd, rec.raw + sizeof(ktrace_header_t), len) != len) {
            fprintf(stderr, "eof: incomplete packet at offset %08x\n", offset);
            break;
        }
        if (offset > limit) {
            break;
        }

        s.events++;
        import_event(rec, KTRACE_EVENT(tag));
    }
    if (s.events) {
        finish(s.ts_last);
        adjust_tracks(group_list);
    }

    // shuffle the idle threads to the front of the kernel thread list
    Group* k = find_process(0)->group;
    Track* first = nullptr;
    Track* last = nullptr;
    Track* next;
    for (Track* t = k->first; t != nullptr; t = next) {
        next = t->next;
        if (!strncmp(t->name, "idle", 4)) {
            if (last == nullptr) {
                t->next = nullptr;
                first = last = t;
            } else {
                t->next = first;
                first = t;
            }
        } else {
            if (last == nullptr) {
                first = t;
            } else {
                last->next = t;
            }
            last = t;
            t->next = nullptr;
        }
    }
    k->first = first;
    k->last = last;

    if (show_stats) {
        dump_stats(&s);
    }
    return 0;
}

int Trace::import(int argc, char** argv) {
    while (argc > 1) {
        if (!strcmp(argv[1], "-v")) {
            verbose++;
        } else if (!strcmp(argv[1], "-text")) {
            text = 1;
        } else if (!strncmp(argv[1], "-limit=", 7)) {
            limit = 32 * atoi(argv[1] + 7);
        } else if (!strcmp(argv[1], "-stats")) {
            show_stats = 1;
        } else if (argv[1][0] == '-') {
            fprintf(stderr, "error: unknown option '%s'\n\n", argv[0]);
            return -1;
        } else {
            break;
        }
        argc--;
        argv++;
    }

    if (argc != 2) {
        return -1;
    }

    int fd;
    if ((fd = open(argv[1], O_RDONLY)) < 0) {
        fprintf(stderr, "error: cannot open '%s'\n", argv[0]);
        return -1;
    }
    int r = import(fd);
    close(fd);
    return r;
}

};
