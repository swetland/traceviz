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
    return t;
}

void Trace::track_append(Track* t, uint64_t ts, uint8_t state, uint8_t cpu) {
    TaskState task;
    task.ts = ts;
    task.state = state;
    task.cpu = cpu;
    t->task.push_back(task);
}

void Trace::track_add_event(Track* t, uint64_t ts, uint32_t tag) {
    Event event;
    event.ts = ts;
    event.tag = tag;
    t->event.push_back(event);
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

MsgPipe::MsgPipe(uint32_t _id) : Object(_id, KPIPE) {
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
}

#if KTHREADS
// kthread ids are their kvaddrs and may collide with koids
// but there are usually only a handful of these, so just use
// a simple linked list
typedef struct kthread kthread_t;
struct kthread {
    kthread_t* next;
    uint64_t last_ts;
    uint32_t id;
};

kthread_t* kthread_list;

kthread_t* find_kthread(uint32_t id) {
    kthread_t* t;
    for (t = kthread_list; t != NULL; t = t->next) {
        if (t->id == id) {
            return t;
        }
    }
    t = (kthread_t*)malloc(sizeof(kthread_t));
    t->id = id;
    t->last_ts = 0;
    t->next = kthread_list;
    kthread_list = t;
    evt_thread_name(0, id, (id & 0x80000000) ? "idle" : "kernel");
    return t;
}
#endif

static uint64_t ticks_per_ms;

uint64_t ticks_to_ts(uint64_t ts) {
    //TODO: handle overflow for large times
    if (ticks_per_ms) {
        return (ts * 1000000UL) / ticks_per_ms;
    } else {
        return 0;
    }
}

const char* recname(const ktrace_record_t* rec) {
    static char name[25];
    memcpy(name, &rec->ts, 24);
    name[24] = 0;
    return name;
}

int verbose = 0;
int text = 0;

void Trace::evt_context_switch(uint64_t ts, uint32_t oldtid, uint32_t newtid,
                               uint32_t state, uint32_t cpu,
                               uint32_t oldthread, uint32_t newthread) {
#if KTHREADS
    char name[32];
    sprintf(name, "cpu%u", cpu);
    if (ei->tid == 0) {
        kthread_t* t = find_kthread(oldthread);
    }
    if (newtid == 0) {
        kthread_t* t = find_kthread(newthread);
    }
#endif
    if (oldtid) {
        Thread* t = find_thread(oldtid);
        track_append(t->track, ts, state, cpu);
    }
    if (newtid) {
        Thread* t = find_thread(newtid);
        track_append(t->track, ts, TS_RUNNING, cpu);
    }
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
    }
    Process* p = find_process(pid);
    group_add_track(p->group, t->track);
}
void Trace::evt_thread_delete(uint64_t ts, Thread* t, uint32_t tid) {
}
void Trace::evt_thread_start(uint64_t ts, Thread* t, uint32_t tid) {
}
void Trace::evt_thread_name(uint32_t tid, const char* name) {
    char tmp[128];
    sprintf(tmp, "%s (%u)", name, tid);
    Thread* t = find_thread(tid);
    t->track->name = strdup(tmp);
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
    track_add_event(t->track, ts, EVT_MSGPIPE_WRITE);

#if 0
    // if we can find the other half, start a flow event from
    // here to there
    Object* oi = find_object(id, KPIPE);
    if (oi == NULL) return;
    char xid[128];
    sprintf(xid, "%x:%x:%x", id, otherid, oi->seq_src++);
    json_rec(ei->ts, "s", "write", "msgpipe",
             "id", xid,
             "#pid", ei->pid,
             "tid", tidstr,
             NULL);
#endif
}

void Trace::evt_msgpipe_read(uint64_t ts, Thread* t, uint32_t id, uint32_t bytes, uint32_t handles) {
    track_add_event(t->track, ts, EVT_MSGPIPE_READ);

#if 0
    // if we can find the other half, finish a flow event
    // from there to here
    Object* oi = find_object(otherid, KPIPE);
    if (oi == NULL) return;
    char xid[128];
    sprintf(xid, "%x:%x:%x", otherid, id, oi->seq_dst++);
    json_rec(ei->ts, "f", "read", "msgpipe",
             "bp", "e",
             "id", xid,
             "#pid", ei->pid,
             "tid", tidstr,
             NULL);
#endif
}

void Trace::evt_port_create(uint64_t ts, Thread* t, uint32_t id) {
}
void Trace::evt_port_wait(uint64_t ts, Thread* t, uint32_t id) {
    track_add_event(t->track, ts, EVT_PORT_WAIT);
}
void Trace::evt_port_wait_done(uint64_t ts, Thread* t, uint32_t id) {
    track_add_event(t->track, ts, EVT_PORT_WAITED);
}
void Trace::evt_port_delete(uint64_t ts, Thread* t, uint32_t id) {
}

void Trace::evt_wait_one(uint64_t ts, Thread* t, uint32_t id, uint32_t signals, uint64_t timeout) {
    track_add_event(t->track, ts, EVT_HANDLE_WAIT);
}
void Trace::evt_wait_one_done(uint64_t ts, Thread* t, uint32_t id, uint32_t pending, uint32_t status) {
    track_add_event(t->track, ts, EVT_HANDLE_WAITED);
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
bool is_regular_event(uint32_t tag) {
    switch (tag) {
    case TAG_OBJECT_DELETE:
    case TAG_PROC_CREATE:
    case TAG_PROC_START:
    case TAG_THREAD_CREATE:
    case TAG_THREAD_START:
    case TAG_MSGPIPE_CREATE:
    case TAG_MSGPIPE_WRITE:
    case TAG_MSGPIPE_READ:
    case TAG_PORT_CREATE:
    case TAG_PORT_QUEUE:
    case TAG_PORT_WAIT:
    case TAG_PORT_WAIT_DONE:
    case TAG_WAIT_ONE:
    case TAG_WAIT_ONE_DONE:
        return true;
    default:
        return false;
    }
}

#define trace(fmt...) do { if(text) printf(fmt); } while (0)

void Trace::import_special(ktrace_record_t& rec, uint32_t tag) {
    trace("                          ");

    switch (tag) {
    case TAG_TICKS_PER_MS:
        ticks_per_ms = ((uint64_t)rec.a) | (((uint64_t)rec.b) << 32);
        trace("TICKS_PER_MS n=%lu\n", ticks_per_ms);
        break;
    case TAG_CONTEXT_SWITCH:
        s.context_switch++;
        trace("CTXT_SWITCH to=%08x st=%d cpu=%d old=%08x new=%08x\n",
              rec.a, rec.b >> 16, rec.b & 0xFFFF, rec.c, rec.d);
        evt_context_switch(ticks_to_ts(rec.ts), rec.id, rec.a, rec.b >> 16, rec.b & 0xFFFF, rec.c, rec.d);
        break;
    case TAG_PROC_NAME:
        trace("PROC_NAME   id=%08x '%s'\n", rec.id, recname(&rec));
        evt_process_name(rec.id, recname(&rec), 10);
        break;
    case TAG_THREAD_NAME:
        trace("THRD_NAME   id=%08x '%s'\n", rec.id, recname(&rec));
        evt_thread_name(rec.id, recname(&rec));
        break;
    default:
        trace("UNKNOWN_TAG id=%08x tag=%08x\n", rec.id, tag);
        break;
    }
}

void Trace::import_regular(ktrace_record_t& rec, uint64_t ts, uint32_t tag) {
    if (rec.id == 0) {
        // ignore kernel threads
        return;
    }
    Thread* t = find_thread(rec.id);

    trace("%04lu.%09lu [%08x] ", ts/(1000000000UL), ts%(1000000000UL), rec.id);

    switch (tag) {
    case TAG_OBJECT_DELETE:
        Object* oi;
        if ((oi = find_object(rec.a, 0)) == 0) {
            trace("OBJT_DELETE id=%08x\n", rec.a);
        } else {
            trace("%s_DELETE id=%08x\n", kind_string(oi->kind), rec.a);
            switch (oi->kind) {
            case KPIPE:
                s.msgpipe_del++;
                evt_msgpipe_delete(ts, t, rec.a);
                break;
            case KTHREAD:
                s.thread_del++;
                evt_thread_delete(ts, t, rec.a);
                break;
            case KPROC:
                s.process_del++;
                evt_process_delete(ts, t, rec.a);
                break;
            case KPORT:
                evt_port_delete(ts, t, rec.a);
                break;
            }
        }
        break;
    case TAG_PROC_CREATE:
        s.process_new++;
        trace("PROC_CREATE id=%08x\n", rec.a);
        evt_process_create(ts, t, rec.a);
        break;
    case TAG_PROC_START:
        trace("PROC_START  id=%08x tid=%08x\n", rec.b, rec.a);
        evt_process_start(ts, t, rec.b, rec.a);
        break;
    case TAG_THREAD_CREATE:
        s.thread_new++;
        trace("THRD_CREATE id=%08x pid=%08x\n", rec.a, rec.b);
        evt_thread_create(ts, t, rec.a, rec.b);
        break;
    case TAG_THREAD_START:
        trace("THRD_START  id=%08x\n", rec.a);
        evt_thread_start(ts, t, rec.a);
        break;
    case TAG_MSGPIPE_CREATE:
        s.msgpipe_new += 2;
        trace("MPIP_CREATE id=%08x other=%08x flags=%x\n", rec.a, rec.b, rec.c);
        evt_msgpipe_create(ts, t, rec.a, rec.b);
        break;
    case TAG_MSGPIPE_WRITE:
        s.msgpipe_write++;
        trace("MPIP_WRITE  id=%08x bytes=%d handles=%d\n", rec.a, rec.b, rec.c);
        evt_msgpipe_write(ts, t, rec.a, rec.b, rec.c);
        break;
    case TAG_MSGPIPE_READ:
        s.msgpipe_read++;
        trace("MPIP_READ   id=%08x bytes=%d handles=%d\n", rec.a, rec.b, rec.c);
        evt_msgpipe_read(ts, t, rec.a, rec.b, rec.c);
        break;
    case TAG_PORT_CREATE:
        trace("PORT_CREATE id=%08x\n", rec.a);
        evt_port_create(ts, t, rec.a);
        break;
    case TAG_PORT_QUEUE:
        trace("PORT_QUEUE  id=%08x\n", rec.a);
        break;
    case TAG_PORT_WAIT:
        trace("PORT_WAIT   id=%08x\n", rec.a);
        evt_port_wait(ts, t, rec.a);
        break;
    case TAG_PORT_WAIT_DONE:
        trace("PORT_WDONE  id=%08x\n", rec.a);
        evt_port_wait_done(ts, t, rec.a);
        break;
    case TAG_WAIT_ONE: {
        uint64_t timeout = ((uint64_t)rec.c) | (((uint64_t)rec.d) << 32);
        trace("WAIT_ONE    id=%08x signals=%08x timeout=%lu\n", rec.a, rec.b, timeout);
        evt_wait_one(ts, t, rec.a, rec.b, timeout);
        break;
    }
    case TAG_WAIT_ONE_DONE:
        trace("WAIT_DONE   id=%08x pending=%08x result=%08x\n", rec.a, rec.b, rec.c);
        evt_wait_one_done(ts, t, rec.a, rec.b, rec.c);
        break;
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
    uint64_t ts = 0;
    memset(&s, 0, sizeof(s));

    evt_process_name(0, "Magenta Kernel", 0);

    while (read(fd, &rec, sizeof(rec)) == sizeof(rec)) {
        uint32_t tag = rec.tag & 0xFFFFFF00;
        offset += 32;
        if (tag == 0) {
            fprintf(stderr, "eof: zero tag at offset %08x\n", offset);
            break;
        }
        if (offset > limit) {
            break;
        }

        s.events++;
        if (is_regular_event(tag)) {
            ts = ticks_to_ts(rec.ts);
            import_regular(rec, ts, tag);
        } else {
            import_special(rec, tag);
        }
    }
    if (s.events) {
        s.ts_last = ts;
        finish(ts);
        adjust_tracks(group_list);
    }

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
    text = 0;
    int r = import(fd);
    close(fd);
    return r;
}

};
