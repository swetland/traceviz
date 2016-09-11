# Copyright 2016 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

IMGUI := third_party/imgui

FLAGS := -MMD -g -Wall -Wformat
FLAGS += -Isrc -I$(IMGUI)
FLAGS += -DImDrawIdx=unsigned

all: out/traceviz

SRCS := src/traceviz.cpp src/ktrace.c
SRCS += src/font-droid-sans.S src/font-symbols.S
SRCS += $(IMGUI)/imgui.cpp $(IMGUI)/imgui_draw.cpp

#SRCS += src/main-opengl3.cpp
#SRCS += $(IMGUI)/examples/opengl3_example/imgui_impl_glfw_gl3.cpp
#SRCS += $(IMGUI)/examples/libs/gl3w/GL/gl3w.cpp
#LIBS := -lGL `pkg-config --static --libs glfw3`
#FLAGS += -I$(IMGUI)/examples/libs/gl3w -I$(IMGUI)/examples/opengl3_example
#FLAGS += `pkg-config --cflags glfw3`

SRCS += src/main-opengl3-sdl.cpp
SRCS += $(IMGUI)/examples/sdl_opengl3_example/imgui_impl_sdl_gl3.cpp
SRCS += $(IMGUI)/examples/libs/gl3w/GL/gl3w.cpp
LIBS := `sdl2-config --libs` -lGL -ldl
FLAGS += -I$(IMGUI)/examples/libs/gl3w -I$(IMGUI)/examples/sdl_opengl3_example
FLAGS += `sdl2-config --cflags`

OBJS := $(patsubst %.S,%.o,$(patsubst %.c,%.o,$(patsubst %.cpp,%.o,$(SRCS))))
OBJS := $(patsubst %,out/%,$(OBJS))
DEPS := $(patsubst %.o,%.d,$(OBJS))

UNAME_S := $(shell uname -s)

CFLAGS := -std=c11 $(FLAGS)
CXXFLAGS := -std=c++11 $(FLAGS)

MKDIR = mkdir -p $(dir $@)

out/src/font-symbols.o: src/symbols.ttf

out/%.o: %.cpp Makefile
	@$(MKDIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

out/%.o: %.c Makefile
	@$(MKDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

out/%.o: %.S Makefile
	@$(MKDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

out/traceviz: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LIBS)

-include $(DEPS)

clean:
	rm -rf out
