# OpenCode Session Manager -- build with MinGW-w64 (mingw32-make)
#
#   mingw32-make            build TUI + GUI + tests
#   mingw32-make gui        GUI only
#   mingw32-make tui        TUI only
#   mingw32-make test       build and run the data layer tests
#   mingw32-make clean
#
# The toolchain location is overridable, so this no longer only works on a
# machine that happens to have C:\MinGW:
#   mingw32-make MINGW_ROOT=D:/msys64/ucrt64 MINGW_OPT=D:/msys64/ucrt64/opt
#
# (The .bat scripts do the same thing without needing make.)

# GNU make on Windows otherwise picks up sh.exe from PATH (e.g. a Git Bash
# install under "C:\Program Files\...") and then fails to even spawn it when the
# path contains a space. Every recipe here is plain path-invocation + mkdir, so
# cmd.exe behaves identically.
ifeq ($(OS),Windows_NT)
SHELL := cmd.exe
endif

MINGW_ROOT ?= C:/MinGW
MINGW_OPT  ?= $(MINGW_ROOT)/opt
CXX        ?= $(MINGW_ROOT)/bin/g++
WINDRES    ?= $(MINGW_ROOT)/bin/windres
OUT        ?= dist

# Defined after OUT: a simply-expanded variable would capture an empty value.
ifeq ($(OS),Windows_NT)
CLEANDIR := if exist $(OUT) rmdir /S /Q $(OUT)
else
CLEANDIR := rm -rf $(OUT)
endif

IMGUI     := imgui
CXXFLAGS  := -std=c++17 -O2 -Wall
INCS      := -I"$(IMGUI)" -I"$(IMGUI)/backends" -I"$(MINGW_OPT)/include"
LIBDIR    := -L"$(MINGW_OPT)/lib"
LIBS_WIN  := -luser32 -lshell32 -lole32

# Prerequisite names must be unquoted (make treats quotes as part of the file
# name); the recipes below may quote them, and neither path contains spaces.
IMGUI_SRC := $(IMGUI)/imgui.cpp $(IMGUI)/imgui_draw.cpp \
             $(IMGUI)/imgui_tables.cpp $(IMGUI)/imgui_widgets.cpp \
             $(IMGUI)/backends/imgui_impl_win32.cpp \
             $(IMGUI)/backends/imgui_impl_opengl3.cpp

TUI  := $(OUT)/opencode-session-manager.exe
GUI  := $(OUT)/opencode-session-manager-gui.exe
TEST := $(OUT)/test_data.exe
RES  := $(OUT)/resource.o

.PHONY: all gui tui test clean
all: gui tui test

gui: $(GUI)
tui: $(TUI)

test: $(TEST)
	$(TEST) test-scratch

$(OUT):
	mkdir $(OUT)

$(RES): resource.rc | $(OUT)
	$(WINDRES) resource.rc -O coff -o $(RES)

$(TUI): main.cpp opencode_data.cpp session_view.cpp db_maintenance.cpp *.hpp $(RES) | $(OUT)
	$(CXX) $(CXXFLAGS) -static -DNCURSES_WIDECHAR $(INCS) \
	    main.cpp opencode_data.cpp session_view.cpp $(RES) $(LIBDIR) -lncursesw -lsqlite3 $(LIBS_WIN) -o $(TUI)

$(GUI): main_gui.cpp opencode_data.cpp session_view.cpp db_maintenance.cpp *.hpp $(IMGUI_SRC) $(RES) | $(OUT)
	$(CXX) $(CXXFLAGS) -static $(INCS) \
	    main_gui.cpp opencode_data.cpp session_view.cpp $(IMGUI_SRC) $(RES) $(LIBDIR) \
	    -lsqlite3 -lopengl32 -ldwmapi -lgdi32 $(LIBS_WIN) -mwindows -o $(GUI)

$(TEST): tests/test_data.cpp opencode_data.cpp session_view.cpp db_maintenance.cpp *.hpp | $(OUT)
	$(CXX) -std=c++17 -O2 -Wall -Wextra -I. $(INCS) \
	    tests/test_data.cpp opencode_data.cpp session_view.cpp db_maintenance.cpp $(LIBDIR) -lsqlite3 $(LIBS_WIN) -o $(TEST)

clean:
	-$(CLEANDIR)
