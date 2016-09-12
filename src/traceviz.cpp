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

void DrawRightTriangle(ImDrawList* dl, ImVec2 pos, ImVec2 size, ImU32 col) {
    dl->AddTriangleFilled(pos, pos + ImVec2(size.x, size.y/2.0), pos + ImVec2(0, size.y), col);
}

void DrawDownTriangle(ImDrawList* dl, ImVec2 pos, ImVec2 size, ImU32 col) {
    dl->AddTriangleFilled(pos, pos + ImVec2(size.x, 0), pos + ImVec2(size.x/2.0, size.y), col);
}

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

    if (ImGui::IsMouseDown(0) && ImGui::IsWindowFocused()) {
        if (io.KeyCtrl) {
            ImVec2 pos = origin + ImVec2(W_NAMES, 0);
            ImVec2 mouse = ImGui::GetMousePos();
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
                int64_t x0 = task->ts;
                uint8_t state = task->state;
                uint8_t cpuid = task->cpu;

                if (++task == end) {
                    break;
                }
                int64_t x1 = task->ts;
                if (x0 < tsedge) {
                    x0 = tsedge;
                }
                if (x1 > tsend) {
                    x1 = tsend;
                }

                // convert to local coords
                x0 = (x0 - tsedge) / tscale;
                x1 = (x1 - tsedge) / tscale;

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

    //const ImFont::Glyph* gUP = symbols->FindGlyph('A');
    //const ImFont::Glyph* gDOWN = symbols->FindGlyph('B');
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

    pos = origin + ImVec2(W_NAMES, H_RULER);
    size = content - ImVec2(W_NAMES, 0);
    if (show_evts) for (Group* g = groups; g != NULL; g = g->next) {
        pos += ImVec2(0, H_GROUP);
        if (g->flags & GRP_FOLDED) {
            continue;
        }
        for (Track* t = g->first; t != NULL; t = t->next) {
            auto e = t->event.begin();
            auto end = t->event.end();

            e = std::lower_bound(e, end, tsedge);
            int64_t tsend = tsedge + ((int64_t)size.x) * tscale;
            int64_t last_x = 0xFFFFFFFFFFFFFFFFUL;

            for (; (e != end) && (e->ts < tsend); e++) {
                int64_t x = e->ts;
                // convert to local coords
                x = (x - tsedge) / tscale;
                if (x < last_x) {
                    continue;
                }

                if (show_flow && (e->tag == EVT_MSGPIPE_READ) && (e->eventidx)) {
                    Track* wrtrack = trace.get_track(e->trackidx);
                    auto wrevent = wrtrack->event[e->eventidx];
                    auto wrpos = ImVec2((wrevent.ts - tsedge) / (float)tscale, wrtrack->y);

                    auto p0 = wrpos + ImVec2(pos.x + 8.0, 0);
                    auto p1 = pos + ImVec2(x + 8.0, 0);
                    if (p0.y < p1.y) {
                        p0 += ImVec2(0, 16);
                    } else {
                        p1 += ImVec2(0, 16);
                    }

                    float n = (p1.x - p0.x) / 2.0;
                    dl->AddBezierCurve(p0, p0 + ImVec2(n,0), p1 + ImVec2(-n,0), p1, fg, 2.0);

                }
                const ImFont::Glyph* glyph;
                switch (e->tag) {
                case EVT_PORT_WAIT:
                case EVT_HANDLE_WAIT:
                    glyph = gSQUARE;
                    break;
                case EVT_PORT_WAITED:
                case EVT_HANDLE_WAITED:
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
                default:
                    glyph = gDIAMOND;
                    break;
                }
                //if ((e->tag == EVT_MSGPIPE_WRITE) || (e->tag == EVT_MSGPIPE_READ))
                symbols->RenderGlyph(dl, pos + ImVec2(x, -1.0), ImColor(0, 0, 220), glyph);
                last_x = x;
                e->x = x;
            }
            pos += ImVec2(0, H_TRACE);
        }
    }

    if ((mark0_pos != mark1_pos) || is_marking) {
        pos = origin + ImVec2(W_NAMES, H_RULER);
        size = content - ImVec2(W_NAMES, 0);
        float x0 = (mark0_pos - tsedge) / tscale;
        float x1 = (mark1_pos - tsedge) / tscale;
        dl->AddLine(ImVec2(pos.x + x0, pos.y), ImVec2(pos.x + x0, pos.y + size.y), red);
        dl->AddLine(ImVec2(pos.x + x1, pos.y), ImVec2(pos.x + x1, pos.y + size.y), red);
    }
    ImGui::PopClipRect();
}

static bool show_color_editor = false;
static bool show_metrics_window = false;

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
            ImGui::MenuItem("Quit", "CTRL+Q");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Window")) {
            if (ImGui::MenuItem("Color Editor")) { show_color_editor = true; }
            if (ImGui::MenuItem("Metrics")) { show_metrics_window = true; }
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

    // Render Metrics Window
    if (show_metrics_window) {
        ImGui::ShowMetricsWindow(&show_metrics_window);
    }

    return 0;
}
