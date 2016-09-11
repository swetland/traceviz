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

#define FNV32_PRIME (16777619)
#define FNV32_OFFSET_BASIS (2166136261)

// for bits 0..15
static inline uint32_t fnv1a_tiny(uint32_t n, uint32_t bits) {
    uint32_t hash = FNV32_OFFSET_BASIS;
    hash = (hash ^ (n & 0xFF)) * FNV32_PRIME; n >>= 8;
    hash = (hash ^ (n & 0xFF)) * FNV32_PRIME; n >>= 8;
    hash = (hash ^ (n & 0xFF)) * FNV32_PRIME; n >>= 8;
    hash = (hash ^ n) * FNV32_PRIME;
    return ((hash >> bits) ^ hash) & ((1 << bits) - 1);
}

group_t* group_list;
group_t* group_last;
group_t* group_kernel;

group_t* group_create(void) {
    group_t* g = (group_t*)calloc(1, sizeof(group_t));
    if (group_last) {
        group_last->next = g;
    } else {
        group_list = g;
    }
    group_last = g;
    return g;
}

track_t* track_create(group_t* group) {
    track_t* t = (track_t*)calloc(1, sizeof(track_t));
    t->tasksize = 128;
    t->taskcount = 1;
    t->task = (taskstate_t*)malloc(sizeof(taskstate_t) * t->tasksize);
    t->task[0].ts = 0;
    t->task[0].state = TS_NONE;
    t->eventsize = 128;
    t->eventcount = 0;
    t->event = (event_t*)malloc(sizeof(event_t) * t->eventsize);
    if (group->last) {
        group->last->next = t;
    } else {
        group->first = t;
    }
    group->last = t;
    return t;
}

void track_append(track_t* t, uint64_t ts, uint8_t state, uint8_t cpu) {
    if (t->taskcount == t->tasksize) {
        t->tasksize *= 2;
        t->task = (taskstate_t*)realloc(t->task, sizeof(taskstate_t) * t->tasksize);
    }
    t->task[t->taskcount].ts = ts;
    t->task[t->taskcount].state = state;
    t->task[t->taskcount].cpu = cpu;
    t->taskcount++;
}

void track_add_event(track_t* t, uint64_t ts, uint32_t tag) {
    if (t->eventcount == t->eventsize) {
        t->eventsize *= 2;
        t->event = (event_t*)realloc(t->event, sizeof(event_t) * t->eventsize);
    }
    t->event[t->eventcount].ts = ts;
    t->event[t->eventcount++].tag = tag;
}

typedef struct objinfo objinfo_t;
struct objinfo {
    objinfo_t* next;
    union {
        track_t* track;
        group_t* group;
    };
    uint32_t id;
    uint32_t kind;
    uint32_t flags;
    uint32_t creator;
    uint32_t extra;
    uint32_t seq_src;
    uint32_t seq_dst;
};

#define F_DEAD 1

#define HASHBITS 10
#define BUCKETS (1 << HASHBITS)

#define OBJBUCKET(id) fnv1a_tiny(id, HASHBITS)
objinfo_t *objhash[BUCKETS];

#define KPROC    1 // extra = 0
#define KTHREAD  2 // extra = pid
#define KPIPE    3 // extra = other-pipe-id
#define KPORT    4 // extra = 0

const char* kind_string(uint32_t kind) {
    switch (kind) {
    case KPROC:   return "PROC";
    case KTHREAD: return "THRD";
    case KPIPE:   return "MPIP";
    case KPORT:   return "PORT";
    default:      return "NVLD";
    }
}

objinfo_t* find_object(uint32_t id, uint32_t kind) {
    for (objinfo_t* oi = objhash[OBJBUCKET(id)]; oi != NULL; oi = oi->next) {
        if (oi->id == id) {
            if (kind && (oi->kind != kind)) {
                fprintf(stderr, "error: object(%08x) kind %d != %d\n", id, kind, oi->kind);
            }
            return oi;
        }
    }
    return NULL;
}

objinfo_t* new_object(uint32_t id, uint32_t kind, uint32_t creator, uint32_t extra) {
    if (find_object(id, 0) != NULL) {
        fprintf(stderr, "error: object(%08x) already exists!\n", id);
    }
    objinfo_t* oi = (objinfo_t*)calloc(1, sizeof(objinfo_t));
    oi->id = id;
    oi->kind = kind;
    oi->creator = creator;
    oi->extra = extra;
    unsigned n = OBJBUCKET(id);
    oi->next = objhash[n];
    objhash[n] = oi;
    return oi;
}

int is_object(uint32_t id, uint32_t flags) {
    objinfo_t* oi = find_object(id, 0);
    if (oi && (oi->flags & flags)) {
        return 1;
    } else {
        return 0;
    }
}

void for_each_object(void (*func)(objinfo_t* oi, uint64_t ts), uint64_t ts) {
    for (unsigned n = 0; n < BUCKETS; n++) {
        for (objinfo_t* oi = objhash[n]; oi != NULL; oi = oi->next) {
            func(oi, ts);
        }
    }
}

void evt_thread_name(uint32_t pid, uint32_t tid, const char* name);

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

uint32_t other_pipe(uint32_t id) {
    objinfo_t* oi = find_object(id, KPIPE);
    if (oi) {
        return oi->extra;
    } else {
        fprintf(stderr, "error: pipe object(%08x) missing\n", id);
        return 0;
    }
}

uint32_t thread_to_process(uint32_t id) {
    objinfo_t* oi = find_object(id, KTHREAD);
    if (oi) {
        return oi->extra;
    } else {
        return 0;
    }
}

int verbose = 0;
int text = 0;

typedef struct evtinfo {
    uint64_t ts;
    uint32_t pid;
    uint32_t tid;
} evt_info_t;

#define trace(fmt...) do { if(text) printf(fmt); } while (0)

void trace_hdr(evt_info_t* ei, uint32_t tag) {
    if (!text) {
        return;
    }
    switch (tag) {
    case TAG_TICKS_PER_MS:
    case TAG_PROC_NAME:
    case TAG_THREAD_NAME:
        printf("                          ");
        return;
    }
    printf("%04lu.%09lu [%08x] ",
           ei->ts/(1000000000UL), ei->ts%(1000000000UL), ei->tid);
}

void evt_context_switch(evt_info_t* ei, uint32_t newpid, uint32_t newtid,
                        uint32_t state, uint32_t cpu, uint32_t oldthread, uint32_t newthread) {
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
    if (ei->pid && ei->tid) {
        objinfo_t* oi = find_object(ei->tid, KTHREAD);
        if (oi) {
            track_append(oi->track, ei->ts, state, 0);
        }
    }
    if (newpid && newtid) {
        objinfo_t* oi = find_object(newtid, KTHREAD);
        if (oi) {
            track_append(oi->track, ei->ts, TS_RUNNING, cpu);
        }
    }
}

void end_of_trace(objinfo_t* oi, uint64_t ts) {
    if (oi->kind == KTHREAD) {
        // final mark at the final timestamp
        track_append(oi->track, ts, TS_NONE, 0);
    }
}


void evt_process_create(evt_info_t* ei, uint32_t pid) {
}
void evt_process_delete(evt_info_t* ei, uint32_t pid) {
}
void evt_process_start(evt_info_t* ei, uint32_t pid, uint32_t tid) {
}
void evt_process_name(uint32_t pid, const char* name, uint32_t index) {
    objinfo_t* oi = find_object(pid, KPROC);
    if (oi) {
        oi->group->name = strdup(name);
    }
}

void evt_thread_create(evt_info_t* ei, uint32_t tid, uint32_t pid) {
}
void evt_thread_delete(evt_info_t* ei, uint32_t tid) {
}
void evt_thread_start(evt_info_t* ei, uint32_t tid) {
}
void evt_thread_name(uint32_t pid, uint32_t tid, const char* name) {
    char tmp[128];
    sprintf(tmp, "%s (%u)", name, tid);
    objinfo_t* oi = find_object(tid, KTHREAD);
    if (oi) {
        oi->track->name = strdup(tmp);
    }
}

void evt_msgpipe_create(evt_info_t* ei, uint32_t id, uint32_t otherid) {
    objinfo_t* oi = find_object(ei->tid, KTHREAD);
    if (oi) {
        track_add_event(oi->track, ei->ts, EVT_MSGPIPE_CREATE);
    }
}

void evt_msgpipe_delete(evt_info_t* ei, uint32_t id) {
}
void evt_msgpipe_write(evt_info_t* ei, uint32_t id, uint32_t otherid,
                       uint32_t bytes, uint32_t handles) {
    if (ei->pid == 0) {
        // ignore writes from unknown threads
        return;
    }

    objinfo_t* oi = find_object(ei->tid, KTHREAD);
    if (oi) {
        track_add_event(oi->track, ei->ts, EVT_MSGPIPE_WRITE);
    }

#if 0
    // if we can find the other half, start a flow event from
    // here to there
    objinfo_t* oi = find_object(id, KPIPE);
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

void evt_msgpipe_read(evt_info_t* ei, uint32_t id, uint32_t otherid,
                      uint32_t bytes, uint32_t handles) {
    if (ei->pid == 0) {
        // ignore reads from unknown threads
        return;
    }

    objinfo_t* oi = find_object(ei->tid, KTHREAD);
    if (oi) {
        track_add_event(oi->track, ei->ts, EVT_MSGPIPE_READ);
    }
#if 0
    // if we can find the other half, finish a flow event
    // from there to here
    objinfo_t* oi = find_object(otherid, KPIPE);
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

void evt_port_create(evt_info_t* ei, uint32_t id) {
}
void evt_port_wait(evt_info_t* ei, uint32_t id) {
    objinfo_t* oi = find_object(ei->tid, KTHREAD);
    if (oi) {
        track_add_event(oi->track, ei->ts, EVT_PORT_WAIT);
    }
}
void evt_port_wait_done(evt_info_t* ei, uint32_t id) {
    objinfo_t* oi = find_object(ei->tid, KTHREAD);
    if (oi) {
        track_add_event(oi->track, ei->ts, EVT_PORT_WAITED);
    }
}
void evt_port_delete(evt_info_t* ei, uint32_t id) {
}

void evt_wait_one(evt_info_t* ei, uint32_t id, uint32_t signals, uint64_t timeout) {
    objinfo_t* oi = find_object(ei->tid, KTHREAD);
    if (oi) {
        track_add_event(oi->track, ei->ts, EVT_HANDLE_WAIT);
    }
}
void evt_wait_one_done(evt_info_t* ei, uint32_t id, uint32_t pending, uint32_t status) {
    objinfo_t* oi = find_object(ei->tid, KTHREAD);
    if (oi) {
        track_add_event(oi->track, ei->ts, EVT_HANDLE_WAITED);
    }
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

int ktrace_main(int argc, char** argv) {
    int show_stats = 0;
    stats_t s;
    ktrace_record_t rec;
    objinfo_t* oi;
    objinfo_t* oi2;
    unsigned offset = 0;
    unsigned limit = 0xFFFFFFFF;
    uint64_t t;
    uint32_t n;

    memset(&s, 0, sizeof(s));

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

    evt_process_name(0, "Magenta Kernel", 0);

    evt_info_t ei;
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
        ei.pid = thread_to_process(rec.id);
        ei.tid = rec.id;
        if ((tag != TAG_PROC_NAME) && (tag != TAG_THREAD_NAME)) {
            ei.ts = ticks_to_ts(rec.ts);
            if (s.ts_first == 0) {
                s.ts_first = ei.ts;
            }
        } else {
            ei.ts = 0;
        }
        s.events++;
        trace_hdr(&ei, tag);
        switch (tag) {
        case TAG_TICKS_PER_MS:
            ticks_per_ms = ((uint64_t)rec.a) | (((uint64_t)rec.b) << 32);
            trace("TICKS_PER_MS n=%lu\n", ticks_per_ms);
            break;
        case TAG_CONTEXT_SWITCH:
            s.context_switch++;
            trace("CTXT_SWITCH to=%08x st=%d cpu=%d old=%08x new=%08x\n",
                  rec.a, rec.b >> 16, rec.b & 0xFFFF, rec.c, rec.d);
            evt_context_switch(&ei, thread_to_process(rec.a), rec.a,
                               rec.b >> 16, rec.b & 0xFFFF, rec.c, rec.d);
            break;
        case TAG_OBJECT_DELETE:
            if ((oi = find_object(rec.a, 0)) == 0) {
                trace("OBJT_DELETE id=%08x\n", rec.a);
            } else {
                trace("%s_DELETE id=%08x\n", kind_string(oi->kind), rec.a);
                switch (oi->kind) {
                case KPIPE:
                    s.msgpipe_del++;
                    evt_msgpipe_delete(&ei, rec.a);
                    break;
                case KTHREAD:
                    s.thread_del++;
                    evt_thread_delete(&ei, rec.a);
                    break;
                case KPROC:
                    s.process_del++;
                    evt_process_delete(&ei, rec.a);
                    break;
                case KPORT:
                    evt_port_delete(&ei, rec.a);
                    break;
                }
            }
            break;
        case TAG_PROC_CREATE:
            s.process_new++;
            trace("PROC_CREATE id=%08x\n", rec.a);
            oi = new_object(rec.a, KPROC, rec.id, 0);
            oi->group = group_create();
            evt_process_create(&ei, rec.a);
            break;
        case TAG_PROC_NAME:
            trace("PROC_NAME   id=%08x '%s'\n", rec.id, recname(&rec));
            evt_process_name(rec.id, recname(&rec), 10);
            break;
        case TAG_PROC_START:
            trace("PROC_START  id=%08x tid=%08x\n", rec.b, rec.a);
            evt_process_start(&ei, rec.b, rec.a);
            break;
        case TAG_THREAD_CREATE:
            s.thread_new++;
            trace("THRD_CREATE id=%08x pid=%08x\n", rec.a, rec.b);
            oi = new_object(rec.a, KTHREAD, rec.id, rec.b);
            oi2 = find_object(rec.b, KPROC);
            if (oi2 == NULL) {
                oi2 = new_object(rec.b, KPROC, 0, 0);
                oi2->group = group_create();
            }
            oi->track = track_create(oi2->group);
            evt_thread_create(&ei, rec.a, rec.b);
            break;
        case TAG_THREAD_NAME:
            trace("THRD_NAME   id=%08x '%s'\n", rec.id, recname(&rec));
            evt_thread_name(ei.pid, rec.id, recname(&rec));
            break;
        case TAG_THREAD_START:
            trace("THRD_START  id=%08x\n", rec.a);
            evt_thread_start(&ei, rec.a);
            break;
        case TAG_MSGPIPE_CREATE:
            s.msgpipe_new += 2;
            trace("MPIP_CREATE id=%08x other=%08x flags=%x\n", rec.a, rec.b, rec.c);
            new_object(rec.a, KPIPE, rec.id, rec.b);
            new_object(rec.b, KPIPE, rec.id, rec.a);
            evt_msgpipe_create(&ei, rec.a, rec.b);
            evt_msgpipe_create(&ei, rec.b, rec.a);
            break;
        case TAG_MSGPIPE_WRITE:
            s.msgpipe_write++;
            n = other_pipe(rec.a);
            trace("MPIP_WRITE  id=%08x to=%08x bytes=%d handles=%d\n", rec.a, n, rec.b, rec.c);
            evt_msgpipe_write(&ei, rec.a, n, rec.b, rec.c);
            break;
        case TAG_MSGPIPE_READ:
            s.msgpipe_read++;
            n = other_pipe(rec.a);
            trace("MPIP_READ   id=%08x fr=%08x bytes=%d handles=%d\n", rec.a, n, rec.b, rec.c);
            evt_msgpipe_read(&ei, rec.a, n, rec.b, rec.c);
            break;
        case TAG_PORT_CREATE:
            trace("PORT_CREATE id=%08x\n", rec.a);
            new_object(rec.a, KPORT, 0, 0);
            evt_port_create(&ei, rec.a);
            break;
        case TAG_PORT_QUEUE:
            trace("PORT_QUEUE  id=%08x\n", rec.a);
            break;
        case TAG_PORT_WAIT:
            trace("PORT_WAIT   id=%08x\n", rec.a);
            evt_port_wait(&ei, rec.a);
            break;
        case TAG_PORT_WAIT_DONE:
            trace("PORT_WDONE  id=%08x\n", rec.a);
            evt_port_wait_done(&ei, rec.a);
            break;
        case TAG_WAIT_ONE:
            t = ((uint64_t)rec.c) | (((uint64_t)rec.d) << 32);
            trace("WAIT_ONE    id=%08x signals=%08x timeout=%lu\n", rec.a, rec.b, t);
            evt_wait_one(&ei, rec.a, rec.b, t);
            break;
        case TAG_WAIT_ONE_DONE:
            trace("WAIT_DONE   id=%08x pending=%08x result=%08x\n", rec.a, rec.b, rec.c);
            evt_wait_one_done(&ei, rec.a, rec.b, rec.c);
            break;
        default:
            trace("UNKNOWN_TAG id=%08x tag=%08x\n", rec.id, tag);
            break;
        }
    }
    if (s.events) {
        s.ts_last = ei.ts;
        for_each_object(end_of_trace, ei.ts);
    }
    add_groups(group_list);

    if (show_stats) {
        dump_stats(&s);
    }
    return 0;
}
