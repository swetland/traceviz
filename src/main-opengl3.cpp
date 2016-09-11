// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>
#include <stdint.h>

#include <imgui.h>
#include <imgui_internal.h>

#include "imgui_impl_glfw_gl3.h"

#include <GL/gl3w.h>
#include <GLFW/glfw3.h>

#include "traceviz.h"

static void glfw_error(int error, const char* msg) {
    fprintf(stderr, "error: %d: %s\n", error, msg);
}

int main(int argc, char** argv) {
    glfwSetErrorCallback(glfw_error);

    if (!glfwInit()) {
        return -1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    GLFWwindow* window = glfwCreateWindow(1280, 720, "TraceView", NULL, NULL);
    glfwMakeContextCurrent(window);
    gl3wInit();

    ImGui_ImplGlfwGL3_Init(window, true);

    if (traceviz_main(argc, argv)) {
        return -1;
    }

    ImVec4 clear = ImColor(144, 144, 154);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplGlfwGL3_NewFrame();

    	if (traceviz_render()) {
            break;
        }

        int width, height;
        glfwGetFramebufferSize(window, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(clear.x, clear.y, clear.z, clear.w);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui::Render();
        glfwSwapBuffers(window);
    }

    ImGui_ImplGlfwGL3_Shutdown();
    glfwTerminate();
    return 0;
}
