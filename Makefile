# Copyright 2016 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

IMGUI := third_party/imgui

SRCS := src/traceviz.cpp src/ktrace.c
SRCS += src/main-opengl3.cpp
SRCS += $(IMGUI)/imgui.cpp $(IMGUI)/imgui_draw.cpp
#SRCS += $(IMGUI)/imgui_demo.cpp
SRCS += $(IMGUI)/examples/opengl3_example/imgui_impl_glfw_gl3.cpp
SRCS += $(IMGUI)/examples/libs/gl3w/GL/gl3w.cpp

OBJS := $(patsubst %,out/%,$(patsubst %.c,%.o,$(patsubst %.cpp,%.o,$(SRCS))))
DEPS := $(patsubst %.o,%.d,$(OBJS))

UNAME_S := $(shell uname -s)

LIBS := -lGL `pkg-config --static --libs glfw3`

FLAGS := -MMD -g -Wall -Wformat
FLAGS += -Isrc -I$(IMGUI)
FLAGS += -I$(IMGUI)/examples/libs/gl3w -I$(IMGUI)/examples/opengl3_example
FLAGS += `pkg-config --cflags glfw3`
CFLAGS := -std=c11 $(FLAGS)
CXXFLAGS := -std=c++11 $(FLAGS)

MKDIR = mkdir -p $(dir $@)

out/%.o: %.cpp
	@$(MKDIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

out/%.o: %.c
	@$(MKDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

out/traceviz: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LIBS)

-include $(DEPS)

clean:
	rm -rf out
