// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>
#include <stdint.h>

#define IMGUI_DEFINE_MATH_OPERATORS

#include <imgui.h>
#include <imgui_internal.h>

#include "traceviz.h"

static int keymap[ImGuiKey_COUNT];
#define KEY(x) (keymap[ImGuiKey_##x])

using tv::Trace;
using tv::Group;
using tv::Track;
using tv::Event;
using tv::TaskState;

Trace TheTrace;

#define KTRACE_DEF(num,type,name,group) case num: return #name;

const char* evtname(uint32_t evt) {
    switch (evt) {
#include "ktrace-def.h"
    default: return "???";
    }
};

const char* irqname(uint32_t irqn) {
    switch (irqn) {
    case 0x00: return "DIVIDE_0";
    case 0x01: return "DEBUG";
    case 0x02: return "NMI";
    case 0x03: return "BREAKPOINT";
    case 0x04: return "OVERFLOW";
    case 0x05: return "BOUND_RANGE";
    case 0x06: return "INVALID_OP";
    case 0x07: return "DEVICE_NA";
    case 0x08: return "DOUBLE_FAULT";
    case 0x0A: return "INVALID_TSS";
    case 0x0B: return "SEGMENT_NOT_PRESENT";
    case 0x0C: return "STACK_FAULT";
    case 0x0D: return "GP_FAULT";
    case 0x0E: return "PAGE_FAULT";
    case 0x10: return "RESERVED";
    case 0x11: return "FPU_FP_ERROR";
    case 0x12: return "ALIGNMENT_CHECK";
    case 0x13: return "MACHINE_CHECK";
    case 0x14: return "SIMD_FP_ERROR";
    case 0x15: return "VIRT";
    case 0xF0: return "APIC_SPURIOUS";
    case 0xF1: return "APIC_TIMER";
    case 0xF2: return "APIC_ERROR";
    case 0xF3: return "IPI_GENERIC";
    case 0xF4: return "IPI_RESCHEDULE";
    case 0xF5: return "IPI_HALT";
    default: return NULL;
    }
}

void EventTooltip(Trace& trace, Event* evt) {
    switch (evt->tag) {
    case EVT_MSGPIPE_READ:
    case EVT_MSGPIPE_WRITE:
        ImGui::SetTooltip("%s\nbytes = %u\nhandles = %u",
                          evtname(evt->tag), evt->a, evt->b);
        break;
    case EVT_IRQ_ENTER:
    case EVT_IRQ_EXIT: {
        const char* name = irqname(evt->b);
        const char* str = (evt->tag == EVT_IRQ_ENTER) ? "IRQ ENTER" : "IRQ EXIT";
        if (name) {
            ImGui::SetTooltip("%s %d %s", str, evt->b, name);
        } else {
            ImGui::SetTooltip("%s %d", str, evt->b);
        }
        break;
    }
    case EVT_PAGE_FAULT:
        ImGui::SetTooltip("PAGE FAULT 0x%lx 0x%x", ((uint64_t)evt->a << 32) | evt->b, evt->c);
        break;
    case EVT_SYSCALL_ENTER: {
        const char* name = trace.syscall_name(evt->a);
        if (name) {
            ImGui::SetTooltip("SYSCALL %s()", name);
        } else {
            ImGui::SetTooltip("SYSCALL sys_%u()", evt->a);
        }
        break;
    }
    case EVT_SYSCALL_EXIT: {
        const char* name = trace.syscall_name(evt->a);
        if (name) {
            ImGui::SetTooltip("SYSRETN %s()", name);
        } else {
            ImGui::SetTooltip("SYSRETN sys_%u()", evt->a);
        }
        break;
    }
    default:
        if (evt->tag >= EVT_PROBE) {
            const char* name = trace.probe_name(evt->tag);
            if (name) {
                ImGui::SetTooltip("PROBE %s\n%u\n%u", name, evt->a, evt->b);
            } else {
                ImGui::SetTooltip("PROBE %03x\n%u\n%u", evt->tag, evt->a, evt->b);
            }
            break;
        }
        ImGui::SetTooltip("%s", evtname(evt->tag));
        break;
    }
}

static ImU32 task_state_color[TS_LAST + 1];

static const char *task_state_name[TS_LAST + 1] = {
    "Suspended",
    "Ready",
    "Running",
    "Blocked",
    "Sleeping",
    "Dead",
    "None",
};
static float task_float_color[3 * (TS_LAST + 1)];

struct {
    int64_t scale;
    int64_t div;
    const char* unit;
} zoom[] = {
    { 1,           1,          "ns" },
    { 2,           1,          "ns" },
    { 5,           1,          "ns" },
    { 10,          1000,       "us" },
    { 20,          1000,       "us" },
    { 50,          1000,       "us" },
    { 100,         1000,       "us" },
    { 200,         1000,       "us" },
    { 500,         1000,       "us" },
    { 1000,        1000,       "us" },
    { 2000,        1000,       "us" },
    { 5000,        1000,       "us" },
    { 10000,       1000000,    "ms" },
    { 20000,       1000000,    "ms" },
    { 50000,       1000000,    "ms" },
    { 100000,      1000000,    "ms" },
    { 200000,      1000000,    "ms" },
    { 500000,      1000000,    "ms" },
    { 1000000,     1000000,    "ms" },
    { 2000000,     1000000,    "ms" },
    { 5000000,     1000000,    "ms" },
    { 10000000,    1000000000, "s" },
    { 20000000,    1000000000, "s" },
    { 50000000,    1000000000, "s" },
#if 0
    { 100000000,   1000000000, "s" },
    { 200000000,   1000000000, "s" },
    { 500000000,   1000000000, "s" },
    { 1000000000,  1000000000, "s" },
#endif
};

#define ZOOMMAX ((sizeof(zoom)/sizeof(zoom[0])) - 1)
#define ZOOMDEF 19
static unsigned zoomno = ZOOMDEF;

static int64_t drag_offset = 0;
static int64_t tpos = 0; // time at left edge

static ImFont* symbols;

static inline float distish(const ImVec2& a, const ImVec2& b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    return dx * dx + dy * dy;
}

static ImColor colorify(uint32_t tag) {
    uint32_t bits = tag * 157;
    bits ^= bits >> 8;
    return ImColor::HSV((bits & 255) / 256.0, 0.7, 1.0);
}

void DrawRightTriangle(ImDrawList* dl, ImVec2 pos, ImVec2 size, ImU32 col) {
    dl->AddTriangleFilled(pos, pos + ImVec2(size.x, size.y/2.0), pos + ImVec2(0, size.y), col);
}

void DrawDownTriangle(ImDrawList* dl, ImVec2 pos, ImVec2 size, ImU32 col) {
    dl->AddTriangleFilled(pos, pos + ImVec2(size.x, 0), pos + ImVec2(size.x/2.0, size.y), col);
}

static bool show_color_editor = false;
static bool show_metrics_window = false;
static bool show_syscalls = true;
static bool show_interrupts = true;
static bool show_probes = true;
static bool show_help_window = false;
static bool show_flow = true;
static bool show_evts = true;

static bool is_marking = false;
static int64_t mark0_pos;
static int64_t mark1_pos;

void TraceView(tv::Trace &trace, ImVec2 origin, ImVec2 content) {
    Group* groups = trace.get_groups();
    auto red = ImColor(255,0,0);
    auto fg = ImColor(0,0,0);
    auto grid = ImColor(100,100,100);
    auto dl = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();
    float tick = 20.0;

    bool zoomed = false;
    int64_t oldscale = zoom[zoomno].scale;
    if (ImGui::IsKeyPressed(KEY(W), false)) {
        if (zoomno > 0) {
            zoomno--;
        }
        zoomed = true;
    }
    if (ImGui::IsKeyPressed(KEY(S), false)) {
        if (zoomno < ZOOMMAX) {
            zoomno++;
        }
        zoomed = true;
    }

    if (ImGui::IsKeyPressed(KEY(0), false)) {
        zoomno = ZOOMDEF;
        tpos = 0;
    }

    // tscale: nanoseconds per horizontal pixel
    // tdiv: divisor for nanoseconds to units
    int64_t tscale = zoom[zoomno].scale;
    int64_t tdiv = zoom[zoomno].div;
    const char* tunit = zoom[zoomno].unit;

    if (zoomed) {
        auto mpos = ImGui::GetMousePos();
        mpos -= origin + ImVec2(200, 0);
        if ((mpos.x >= 0) && (mpos.x < content.x)) {
            // if cursor is over window, compensate for zoom
            tpos = tpos + (oldscale * mpos.x) - (tscale * mpos.x);
        }
    }

    if (ImGui::IsKeyPressed(KEY(F), false)) {
        show_flow = !show_flow;
        if (show_flow) {
            show_evts = true;
        }
    }
    if (ImGui::IsKeyPressed(KEY(E), false)) {
        show_evts = !show_evts;
    }
    if (ImGui::IsKeyPressed(KEY(I), false)) {
        show_interrupts = !show_interrupts;
    }
    if (ImGui::IsKeyPressed(KEY(C), false)) {
        show_syscalls = !show_syscalls;
    }
    if (ImGui::IsKeyPressed(KEY(P), false)) {
        show_probes= !show_probes;
    }
    if (ImGui::IsKeyPressed(KEY(H), false)) {
        show_help_window = !show_help_window;
    }
    if (ImGui::IsKeyPressed(KEY(M), false)) {
        if (!is_marking && (mark0_pos != mark1_pos)) {
            tpos = mark0_pos;
        }
    }
    if (ImGui::IsKeyDown(KEY(A))) {
        tpos -= tscale * 5;
    }
    if (ImGui::IsKeyDown(KEY(D))) {
        tpos += tscale * 5;
    }

    if (ImGui::IsMouseReleased(0) && drag_offset) {
        tpos += drag_offset;
        drag_offset = 0;
    }

    int64_t tsegment = 100 * tscale;
    int64_t tsedge = tpos;

    // Drawing Constants
#define W_NAMES 200
#define H_TICK  20
#define Y_TICK  15
#define H_RULER 22
#define H_GROUP 20
#define H_TRACE 18

    ImVec2 mouse = ImGui::GetMousePos();
    if (ImGui::IsMouseDown(0) && ImGui::IsWindowFocused()) {
        if (io.KeyCtrl) {
            ImVec2 pos = origin + ImVec2(W_NAMES, 0);
            if (!is_marking) {
                is_marking = true;
                mark0_pos = tsedge + (int64_t) ((mouse.x - pos.x) * tscale);
            }
            mark1_pos = tsedge + (int64_t) ((mouse.x - pos.x) * tscale);
        } else if (io.KeyShift) {
        } else if (io.KeyAlt) {
        } else {
            auto delta = ImGui::GetMouseDragDelta();
            drag_offset = -delta.x * tscale;
            tsedge += drag_offset;
        }
    } else {
        if (is_marking) {
            is_marking = false;
        }
    }

    // round down to prev segment
    int64_t ts = (tsedge / tsegment) * tsegment;

    // figure the adjustment to start of drawing in pixels
    float adj = (tsedge - ts) / tscale;

    // Draw Ruler and Grid
    ImVec2 pos = origin + ImVec2(W_NAMES, 0);
    ImVec2 size = content - ImVec2(W_NAMES, 0);

    if (mark0_pos != mark1_pos) {
        int64_t n = mark0_pos - mark1_pos;
        char tmp[64];
        if (n < 0) {
            n = -n;
        }
        if (n > 1000000000L) {
            sprintf(tmp, "[mark] %ld.%06ld s", n / 1000000000L, (n % 1000000000L) / 1000L);
        } else if (n > 1000000) {
            sprintf(tmp, "[mark] %ld.%06ld ms", n / 1000000L, n % 1000000L);
        } else if (n > 1000) {
            sprintf(tmp, "[mark] %ld.%03ld us", n / 1000L, n % 1000L);
        } else {
            sprintf(tmp, "[mark] %ld ns", n);
        }
        dl->AddText(ImVec2(10, origin.y + 3), red, tmp);
    }
    if (size.x < 0) {
        return;
    }
    ImGui::PushClipRect(pos, pos + size, false);
    dl->AddRect(pos, pos + size, fg);
    dl->AddLine(pos + ImVec2(0, H_RULER), pos + ImVec2(size.x, H_RULER), fg);
    for (float x = 0 - adj; x < size.x; ) {
        char tmp[64];
        sprintf(tmp, "%ld%s", ts / tdiv, tunit);
        dl->AddText(pos + ImVec2(x + 3, 0), fg, tmp);
        ts += tsegment;
        dl->AddLine(pos + ImVec2(x, H_TICK), pos + ImVec2(x, size.y), grid);
        dl->AddLine(pos + ImVec2(x, 0),      pos + ImVec2(x, H_TICK), fg); x += tick;
        dl->AddLine(pos + ImVec2(x, Y_TICK), pos + ImVec2(x, H_TICK), fg); x += tick;
        dl->AddLine(pos + ImVec2(x, Y_TICK), pos + ImVec2(x, H_TICK), fg); x += tick;
        dl->AddLine(pos + ImVec2(x, Y_TICK), pos + ImVec2(x, H_TICK), fg); x += tick;
        dl->AddLine(pos + ImVec2(x, Y_TICK), pos + ImVec2(x, H_TICK), fg); x += tick;
    }
    ImGui::PopClipRect();

    // Draw Group Names and Bars
    // record y position of tracks
    pos = origin + ImVec2(0, H_RULER);
    size = content;
    for (Group* g = groups; g != NULL; g = g->next) {
        dl->AddLine(pos, pos + ImVec2(size.x - 1, 1), ImColor(220,220,220));
        dl->AddRectFilled(pos + ImVec2(0, 1),  pos + ImVec2(size.x - 1, H_GROUP - 2),
                          ImColor(180,180,180));
        dl->AddRectFilled(pos + ImVec2(0, H_GROUP - 2), pos + ImVec2(size.x - 1, H_GROUP - 2),
                          ImColor(150,150,150));
        dl->AddText(pos + ImVec2(H_GROUP, 0), fg, g->name, NULL);
        if (g->flags & GRP_FOLDED) {
            DrawRightTriangle(dl, pos + ImVec2(5, 4), ImVec2(11, 11), fg);
        } else {
            DrawDownTriangle(dl, pos + ImVec2(5, 4), ImVec2(11, 11), fg);
        }
        if (ImGui::IsMouseHoveringRect(pos, pos + ImVec2(W_NAMES, H_GROUP))) {
            if (ImGui::IsMouseClicked(0)) {
                g->flags ^= GRP_FOLDED;
            }
        }
        if (g->flags & GRP_FOLDED) {
            for (Track* t = g->first; t != NULL; t = t->next) {
                t->y = pos.y;
            }
            pos += ImVec2(0, H_GROUP);
        } else {
            pos += ImVec2(0, H_GROUP);
            for (Track* t = g->first; t != NULL; t = t->next) {
                t->y = pos.y;
                pos += ImVec2(0, H_TRACE);
            }
        }
    }

    // Draw Track Names
    pos = origin + ImVec2(5, H_RULER);
    size = content;
    ImGui::PushClipRect(pos, pos + ImVec2(W_NAMES, size.y), false);
    for (Group* g = groups; g != NULL; g = g->next) {
        pos += ImVec2(0, H_GROUP);
        if (g->flags & GRP_FOLDED) {
            continue;
        }
        for (Track* t = g->first; t != NULL; t = t->next) {
            dl->AddText(pos, fg, t->name, NULL);
            pos += ImVec2(0, H_TRACE);
        }
    }
    ImGui::PopClipRect();

    int64_t snap_ts = 0;
    float snap_dist = 10000000.0;

    char cpu[5];
    cpu[0] = 'c';
    cpu[1] = 'p';
    cpu[2] = 'u';
    cpu[3] = '0';
    cpu[4] = 0;
    pos = origin + ImVec2(W_NAMES, H_RULER);
    size = content - ImVec2(W_NAMES, 0);
    ImGui::PushClipRect(pos + ImVec2(1,0), pos + size - ImVec2(1,0), false);
    for (Group* g = groups; g != NULL; g = g->next) {
        pos += ImVec2(0, H_GROUP);
        if (g->flags & GRP_FOLDED) {
            continue;
        }
        for (Track* t = g->first; t != NULL; t = t->next) {
            auto task = t->task.begin();
            auto end = t->task.end();

            // find the event before the left edge
            ++task;
            task = std::lower_bound(task, end, tsedge);
            --task;

            ts = tsedge;
            int64_t tsend = tsedge + ((int64_t)size.x) * tscale;
            int64_t last_x = 0xFFFFFFFFFFFFFFFFUL;

            while ((task != end) && (task->ts < tsend)) {
                int64_t ts0 = task->ts;
                uint8_t state = task->state;
                uint8_t cpuid = task->cpu;

                if (++task == end) {
                    break;
                }
                int64_t ts1 = task->ts;
                if (ts0 < tsedge) {
                    ts0 = tsedge;
                }
                if (ts1 > tsend) {
                    ts1 = tsend;
                }

                // convert to local coords
                float x0 = (ts0 - tsedge) / tscale;
                float x1 = (ts1 - tsedge) / tscale;

                if (is_marking) {
                    float dist = fabs((mouse.x - pos.x) - x0);
                    if (dist < snap_dist) {
                        snap_dist = dist;
                        snap_ts = ts0;
                    }
                }

                if (x1 > last_x) {
                    auto color = task_state_color[state];
                    dl->AddRectFilled(pos + ImVec2(x0, 0), pos + ImVec2(x1, H_TRACE - 2), color);
                    if ((state == TS_RUNNING) && ((x1 - x0) > 50)) {
                        cpu[3] = '0' + cpuid;
                        dl->AddText(pos + ImVec2(x0 + 10, -2.0), fg, cpu);
                    }
                    last_x = x1;
                }
            }
            pos += ImVec2(0, H_TRACE);
        }
    }

    const ImFont::Glyph* gUP = symbols->FindGlyph('A');
    const ImFont::Glyph* gDOWN = symbols->FindGlyph('B');
    const ImFont::Glyph* gRIGHT = symbols->FindGlyph('C');
    //const ImFont::Glyph* gLEFT = symbols->FindGlyph('D');
    const ImFont::Glyph* gSQUARE = symbols->FindGlyph('E');
    const ImFont::Glyph* gDIAMOND = symbols->FindGlyph('F');
    const ImFont::Glyph* gCIRCLE = symbols->FindGlyph('G');
#if 1
    const ImFont::Glyph* gSEND = symbols->FindGlyph('H');
    const ImFont::Glyph* gRECV = symbols->FindGlyph('I');
#else
    const ImFont::Glyph* gSEND = symbols->FindGlyph('K');
    const ImFont::Glyph* gRECV = symbols->FindGlyph('J');
#endif

    Event* tt_evt;
    float tt_dist = 1000000000.0;
    pos = origin + ImVec2(W_NAMES, H_RULER);
    size = content - ImVec2(W_NAMES, 0);
    for (Group* g = groups; g != NULL; g = g->next) {
        pos += ImVec2(0, H_GROUP);
        if (g->flags & GRP_FOLDED) {
            continue;
        }
        for (Track* t = g->first; t != NULL; t = t->next) {
            auto end = t->event.end();
            auto start = std::lower_bound(t->event.begin(), end, tsedge);
            int64_t tsend = tsedge + ((int64_t)size.x) * tscale;

            // Draw system events first.
            if (show_evts || show_interrupts || show_syscalls) {
                for (auto e = start; (e != end) && (e->ts < tsend); ++e) {
                    bool show = show_evts;
                    const ImFont::Glyph* glyph;
                    switch (e->tag) {
                    case EVT_PORT_WAIT:
                    case EVT_WAIT_ONE:
                        glyph = gSQUARE;
                        break;
                    case EVT_PORT_WAIT_DONE:
                    case EVT_WAIT_ONE_DONE:
                        glyph = gRIGHT;
                        break;
                    case EVT_MSGPIPE_CREATE:
                        glyph = gCIRCLE;
                        break;
                    case EVT_MSGPIPE_WRITE:
                        glyph = gSEND;
                        break;
                    case EVT_MSGPIPE_READ:
                        glyph = gRECV;
                        break;
                    case EVT_SYSCALL_ENTER:
                        glyph = gUP;
                        show = show_syscalls;
                        break;
                    case EVT_SYSCALL_EXIT:
                        glyph = gDOWN;
                        show = show_syscalls;
                        break;
                    case EVT_IRQ_ENTER:
                        glyph = gDIAMOND;
                        show = show_interrupts;
                        break;
                    case EVT_IRQ_EXIT:
                        glyph = gDIAMOND;
                        show = show_interrupts;
                        break;
                    case EVT_PAGE_FAULT:
                        glyph = gDIAMOND;
                        show = show_interrupts;
                        break;
                    default:
                        show = false;
                        break;
                    }
                    if (!show) continue;

                    auto gpos = pos + ImVec2((e->ts - tsedge) / (float)tscale, -1.0);
                    float d = distish(gpos + ImVec2(8.0, 8.0), mouse);
                    if (d < tt_dist) {
                        tt_dist = d;
                        tt_evt = &(*e);
                    }
                    symbols->RenderGlyph(dl, gpos, ImColor(0, 0, 220), glyph);
                }
            }

            // Draw probes on top so they are more visible.
            if (show_probes) {
                for (auto e = start; (e != end) && (e->ts < tsend); ++e) {
                    if (e->tag < EVT_PROBE) continue;

                    auto gpos = pos + ImVec2((e->ts - tsedge) / (float)tscale, -1.0);
                    float d = distish(gpos + ImVec2(8.0, 8.0), mouse);
                    if (d < tt_dist) {
                        tt_dist = d;
                        tt_evt = &(*e);
                    }
                    symbols->RenderGlyph(dl, gpos, colorify(e->tag), gDIAMOND);
                }
            }

            // Draw flow events last since they cover other things.
            if (show_flow) {
                for (auto e = start; (e != end) && (e->ts < tsend); ++e) {
                    if (e->tag != EVT_MSGPIPE_READ || !e->eventidx) continue;

                    Track* wrtrack = trace.get_track(e->trackidx);
                    auto wrevent = wrtrack->event[e->eventidx];
                    auto wrpos = ImVec2((wrevent.ts - tsedge) / (float)tscale, wrtrack->y);

                    auto p0 = wrpos + ImVec2(pos.x + 8.0, 0);
                    auto p1 = pos + ImVec2((e->ts - tsedge) / (float)tscale + 8.0, 0);
                    if (p0.y < p1.y) {
                        p0 += ImVec2(0, 16);
                    } else {
                        p1 += ImVec2(0, 16);
                    }

                    float n = (p1.x - p0.x) / 2.0;
                    dl->AddBezierCurve(p0, p0 + ImVec2(n,0), p1 + ImVec2(-n,0), p1, fg, 2.0);
                }
            }
            pos += ImVec2(0, H_TRACE);
        }
    }

    if (sqrtf(tt_dist) < 12.0) {
        EventTooltip(trace, tt_evt);
    }

    if ((mark0_pos != mark1_pos) || is_marking) {
        if (snap_dist < 8.0) {
            mark1_pos = snap_ts;
        }
        pos = origin + ImVec2(W_NAMES, H_RULER);
        size = content - ImVec2(W_NAMES, 0);
        float x0 = (mark0_pos - tsedge) / tscale;
        float x1 = (mark1_pos - tsedge) / tscale;
        dl->AddLine(ImVec2(pos.x + x0, pos.y), ImVec2(pos.x + x0, pos.y + size.y), red);
        dl->AddLine(ImVec2(pos.x + x1, pos.y), ImVec2(pos.x + x1, pos.y + size.y), red);
    }
    ImGui::PopClipRect();
}

int traceviz_main(int argc, char** argv) {
    TheTrace.import(argc, argv);

    for (unsigned n = 0; n < ImGuiKey_COUNT; n++) {
        keymap[n] = ImGui::GetKeyIndex(n);
    }

    task_state_color[TS_NONE] = ImColor(200,200,200);
    task_state_color[TS_SUSPENDED] = ImColor(100,100,100);
    task_state_color[TS_READY] = ImColor(208,104,63);
    task_state_color[TS_RUNNING] = ImColor(27,144,0);
    task_state_color[TS_BLOCKED] = ImColor(164,153,100);
    task_state_color[TS_SLEEPING] = ImColor(85,172,182);
    task_state_color[TS_DEAD] = ImColor(200,200,200);

    for (unsigned n = 0; n <= TS_LAST; n++) {
        ImVec4 c = ImColor(task_state_color[n]);
        task_float_color[n*3+0] = c.x;
        task_float_color[n*3+1] = c.y;
        task_float_color[n*3+2] = c.z;
    }

    ImGuiIO& io = ImGui::GetIO();

    {
        ImFontConfig fc;
        fc.FontData = font_droid_sans;
        fc.FontDataSize = size_droid_sans;
        fc.FontDataOwnedByAtlas = false;
        fc.SizePixels = 16.0;
        io.Fonts->AddFont(&fc);
    }

    {
        ImFontConfig fc;
        fc.FontData = font_symbols;
        fc.FontDataSize = size_symbols;
        fc.FontDataOwnedByAtlas = false;
        fc.SizePixels = 16.0;
        fc.OversampleH = 1;
        fc.OversampleV = 1;
        fc.PixelSnapH = true;
        symbols = io.Fonts->AddFont(&fc);
    }

    return 0;
}

int traceviz_render(void) {
    if (ImGui::IsKeyDown(KEY(Escape))) {
        return -1;
    }

    auto io = ImGui::GetIO();

    // Render Trace Window
    auto bg = ImColor(255,255,255);
    ImGui::SetNextWindowSize(io.DisplaySize, ImGuiSetCond_Always);
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiSetCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0,0));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 255.0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, bg);
    ImGui::Begin("Trace", NULL,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar);

    auto origin = ImGui::GetWindowPos();
    auto size = ImGui::GetContentRegionAvail();
    auto pos = ImGui::GetCursorPos() + origin;
    TraceView(TheTrace, pos, size);
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::PopStyleVar();
    ImGui::PopStyleVar();

    // Render Trace Window Menu Bar
    ImGui::Begin("Trace");
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            ImGui::MenuItem("Quit", "ESC");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Window")) {
            if (ImGui::MenuItem("Color Editor")) { show_color_editor = true; }
            if (ImGui::MenuItem("Metrics")) { show_metrics_window = true; }
            if (ImGui::MenuItem("Help")) { show_help_window = true; }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
    ImGui::End();

    // Render Color Editor Window
    if (show_color_editor) {
        ImGui::Begin("Color Editor", &show_color_editor);
        for (unsigned n = 0; n <= TS_LAST; n++) {
            if (ImGui::ColorEdit3(task_state_name[n], task_float_color + n * 3)) {
                task_state_color[n] = ImColor(task_float_color[n*3+0],
                                              task_float_color[n*3+1],
                                              task_float_color[n*3+2]);
            }
        }
        ImGui::End();
    }

    // Render Help Window
    if (show_help_window) {
        ImGui::Begin("Help", &show_help_window);
        ImGui::Text("A/D - Pan Left / Pan Right");
        ImGui::Text("W/S - Zoom In / Zoom Out");
        ImGui::Text(" ");
        ImGui::Text("E - Toggle Show Events");
        ImGui::Text("F - Toggle Show IPC Flow");
        ImGui::Text("I - Toggle Show Interrupts");
        ImGui::Text("C - Toggle Show Syscalls");
        ImGui::Text("P - Toggle Show Probes");
        ImGui::Text("H - Toggle Show Help");
        ImGui::Text("0 - Go To Origin");
        ImGui::Text("M - Go To Mark");
        ImGui::Text(" ");
        ImGui::Text("Ctrl-Drag - Mark / Measure");
        ImGui::Text("Click-Drag - Pan Left / Pan Right");
        ImGui::End();
    }

    // Render Metrics Window
    if (show_metrics_window) {
        ImGui::ShowMetricsWindow(&show_metrics_window);
    }

    return 0;
}
