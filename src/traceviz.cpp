// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>
#include <stdint.h>

#define IMGUI_DEFINE_MATH_OPERATORS

#include <imgui.h>
#include <imgui_internal.h>

#include "traceviz.h"

static int key_esc;
static int key_w;
static int key_a;
static int key_s;
static int key_d;
static int key_q;
static int key_e;

group_t* groups;

void add_groups(group_t* list) {
    groups = list;
}

void adjust_tracks(void) {
    int64_t tszero = 0x7FFFFFFFFFFFFFFFUL;
    for (group_t* group = groups; group != NULL; group = group->next) {
        for (track_t* track = group->first; track != NULL; track = track->next) {
            if (track->task[1].ts < tszero) {
                tszero = track->task[1].ts;
            }
        }
    }
    for (group_t* group = groups; group != NULL; group = group->next) {
        for (track_t* track = group->first; track != NULL; track = track->next) {
            for (unsigned n = 1; n < track->taskcount; n++) {
                track->task[n].ts -= tszero;
            }
        }
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
    { 10000,       1000,       "us" },
    { 20000,       1000000,    "ms" },
    { 50000,       1000000,    "ms" },
    { 100000,      1000000,    "ms" },
    { 2000000,     1000000,    "ms" },
    { 5000000,     1000000,    "ms" },
    { 10000000,    1000000,    "ms" },
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
unsigned zoomno = 19;

static int busy = 0;

static int64_t drag_offset = 0;
static int64_t tpos = 0; // time at left edge

void TraceView(ImVec2 pos, ImVec2 size) {
    auto fg = ImColor(0,0,0);
    auto grid = ImColor(100,100,100);
    auto dl = ImGui::GetWindowDrawList();
    float tick = 20.0;

    bool zoomed = false;
    int64_t oldscale = zoom[zoomno].scale;
    if (busy) {
        busy--;
    } else {
        if (ImGui::IsKeyDown(key_w)) {
            if (zoomno > 0) {
                zoomno--;
            }
            busy = 10;
            zoomed = true;
        }
        if (ImGui::IsKeyDown(key_s)) {
            if (zoomno < ZOOMMAX) {
                zoomno++;
            }
            busy = 10;
            zoomed = true;
        }
    }

    // tscale: nanoseconds per horizontal pixel
    // tdiv: divisor for nanoseconds to units
    int64_t tscale = zoom[zoomno].scale;
    int64_t tdiv = zoom[zoomno].div;
    const char* tunit = zoom[zoomno].unit;

    if (zoomed) {
        auto mpos = ImGui::GetMousePos();
        mpos -= pos + ImVec2(200, 0);
        if ((mpos.x >= 0) && (mpos.x < size.x)) {
            // if cursor is over window, compensate for zoom
            tpos = tpos + (oldscale * mpos.x) - (tscale * mpos.x);
        }
    }

    if (ImGui::IsKeyDown(key_a)) {
        tpos -= tscale * 5;
    }
    if (ImGui::IsKeyDown(key_d)) {
        tpos += tscale * 5;
    }

    if (ImGui::IsMouseReleased(0) && drag_offset) {
        tpos += drag_offset;
        drag_offset = 0;
    }

    int64_t tsegment = 100 * tscale;
    int64_t tsedge = tpos;

    if (ImGui::IsMouseDown(0) && ImGui::IsWindowFocused()) {
        auto delta = ImGui::GetMouseDragDelta();
        drag_offset = -delta.x * tscale;
        tsedge += drag_offset;
    }

    // round down to prev segment
    int64_t ts = (tsedge / tsegment) * tsegment;

    // figure the adjustment to start of drawing in pixels
    float adj = (tsedge - ts) / tscale;

    ImVec2 pos0 = pos;
    ImVec2 size0 = size;

    // Draw Ruler and Grid
    pos += ImVec2(200, 0);
    size -= ImVec2(200, 0);
    if (size.x < 0) {
        return;
    }
    ImGui::PushClipRect(pos, pos + size, false);
    dl->AddRect(pos, pos + size, fg);
    dl->AddLine(pos + ImVec2(0, 20), pos + ImVec2(size.x, 20), fg);
    for (float x = 0 - adj; x < size.x; ) {
        char tmp[64];
        sprintf(tmp, "%ld%s", ts / tdiv, tunit);
        dl->AddText(pos + ImVec2(x + 3, 0), fg, tmp);
        ts += tsegment;
        dl->AddLine(pos + ImVec2(x, 20), pos + ImVec2(x, size.y), grid);
        dl->AddLine(pos + ImVec2(x, 0), pos + ImVec2(x, 20), fg); x += tick;
        dl->AddLine(pos + ImVec2(x, 15), pos + ImVec2(x, 20), fg); x += tick;
        dl->AddLine(pos + ImVec2(x, 15), pos + ImVec2(x, 20), fg); x += tick;
        dl->AddLine(pos + ImVec2(x, 15), pos + ImVec2(x, 20), fg); x += tick;
        dl->AddLine(pos + ImVec2(x, 15), pos + ImVec2(x, 20), fg); x += tick;
    }
    ImGui::PopClipRect();

    // Draw Group Names and Bars
    ImVec2 textpos = pos0 + ImVec2(0, 22);
    for (group_t* g = groups; g != NULL; g = g->next) {
        dl->AddLine(textpos, textpos + ImVec2(size0.x - 1, 1), ImColor(220,220,220));
        dl->AddRectFilled(textpos + ImVec2(0, 1), textpos + ImVec2(size0.x - 1, 18), ImColor(180,180,180));
        dl->AddRectFilled(textpos + ImVec2(0, 18), textpos + ImVec2(size0.x - 1, 19), ImColor(150,150,150));
        dl->AddText(textpos + ImVec2(5, 0), fg, g->name, NULL);
        textpos += ImVec2(0, 20);
        for (track_t* t = g->first; t != NULL; t = t->next) {
            textpos += ImVec2(0, 18);
        }
    }

    // Draw Track Names
    ImGui::PushClipRect(pos0, pos0 + ImVec2(200, size.y), false);
    textpos = pos0 + ImVec2(5, 22);
    for (group_t* g = groups; g != NULL; g = g->next) {
        textpos += ImVec2(0, 20);
        for (track_t* t = g->first; t != NULL; t = t->next) {
            dl->AddText(textpos, fg, t->name, NULL);
            textpos += ImVec2(0, 18);
        }
    }
    ImGui::PopClipRect();

    ImGui::PushClipRect(pos + ImVec2(1,0), pos + size - ImVec2(1,0), false);
    pos += ImVec2(0, 22);
    for (group_t* g = groups; g != NULL; g = g->next) {
        pos += ImVec2(0, 20);
        for (track_t* t = g->first; t != NULL; t = t->next) {
            taskstate_t* task = t->task + 1;
            taskstate_t* end = t->task + t->taskcount - 1;

            while (task < end) {
                if (task->ts >= tsedge) {
                    break;
                }
                task++;
            }
            task--;

            ts = tsedge;
            int64_t tsend = tsedge + ((int64_t)size.x) * tscale;
            int64_t last_x = 0xFFFFFFFFFFFFFFFFUL;

            while ((task < end) && (task->ts < tsend)) {
                task++;
                int64_t x0 = task[-1].ts;
                int64_t x1 = task[0].ts;
                auto color = task_state_color[task[-1].state];
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
                    dl->AddRectFilled(pos + ImVec2(x0, 0), pos + ImVec2(x1, 16), color);
                }
                last_x = x1;
            }
            pos += ImVec2(0, 18);
        }
    }
    ImGui::PopClipRect();
}

static bool show_color_editor = false;
static bool show_metrics_window = false;

int traceviz_main(int argc, char** argv) {
    ktrace_main(argc, argv);

    key_esc = ImGui::GetKeyIndex(ImGuiKey_Escape);
    key_w = ImGui::GetKeyIndex(ImGuiKey_W);
    key_a = ImGui::GetKeyIndex(ImGuiKey_A);
    key_s = ImGui::GetKeyIndex(ImGuiKey_S);
    key_d = ImGui::GetKeyIndex(ImGuiKey_D);
    key_q = ImGui::GetKeyIndex(ImGuiKey_Q);
    key_e = ImGui::GetKeyIndex(ImGuiKey_A);

    task_state_color[TS_NONE] = ImColor(200,200,200);
    task_state_color[TS_SUSPENDED] = ImColor(100,100,100);
    task_state_color[TS_READY] = ImColor(208,104,63);
    task_state_color[TS_RUNNING] = ImColor(27,144,0);
    task_state_color[TS_BLOCKED] = ImColor(164,153,100);
    task_state_color[TS_SLEEPING] = ImColor(85,172,182);
    task_state_color[TS_DEAD] = ImColor(200,200,200);

    adjust_tracks();

    for (unsigned n = 0; n <= TS_LAST; n++) {
        ImVec4 c = ImColor(task_state_color[n]);
        task_float_color[n*3+0] = c.x;
        task_float_color[n*3+1] = c.y;
        task_float_color[n*3+2] = c.z;
    }

    return 0;
}

int traceviz_render(void) {
    if (ImGui::IsKeyDown(key_esc)) {
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
    TraceView(pos, size);
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
