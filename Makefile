CXX      := g++
CC       := gcc
CXXFLAGS := -std=c++17 -O2 -fPIC -Wall -Wextra -Iinclude
LDFLAGS  := -ldl -lpthread

BUILD    := build
BIN      := $(BUILD)/sober-exec
PAYLOAD  := $(BUILD)/libsoberpay.so
CLIENT   := $(BUILD)/sober-cli
GUI      := $(BUILD)/sober-exec-gui

SRC      := src/finder.cpp src/scanner.cpp src/injector.cpp src/main.cpp
PAYSRC   := payload/entry.cpp payload/luau_api.cpp payload/hooks.cpp payload/ipc_server.cpp payload/scheduler.cpp
CLISRC   := client/cli.cpp

IMGUI_DIR := vendor/imgui
IMGUI_SRC := $(IMGUI_DIR)/imgui.cpp \
             $(IMGUI_DIR)/imgui_draw.cpp \
             $(IMGUI_DIR)/imgui_tables.cpp \
             $(IMGUI_DIR)/imgui_widgets.cpp \
             $(IMGUI_DIR)/imgui_demo.cpp \
             $(IMGUI_DIR)/backends/imgui_impl_glfw.cpp \
             $(IMGUI_DIR)/backends/imgui_impl_opengl3.cpp

TE_DIR    := vendor/texteditor
TE_SRC    := $(TE_DIR)/TextEditor.cpp

GUI_SRC   := gui/main.cpp gui/app.cpp gui/ipc.cpp gui/theme.cpp
GUI_INC   := -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends -I$(TE_DIR) -Igui
GUI_LIBS  := $(shell pkg-config --libs glfw3 gl)
GUI_CF    := $(shell pkg-config --cflags glfw3 gl)

all: $(BIN) $(PAYLOAD) $(CLIENT) $(GUI)

$(BUILD):
	@mkdir -p $(BUILD)

$(BIN): $(SRC) | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC) $(LDFLAGS)

$(PAYLOAD): $(PAYSRC) | $(BUILD)
	$(CXX) $(CXXFLAGS) -shared -fvisibility=hidden -o $@ $(PAYSRC) $(LDFLAGS)

$(CLIENT): $(CLISRC) | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $(CLISRC)

$(GUI): $(GUI_SRC) $(IMGUI_SRC) $(TE_SRC) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(GUI_CF) $(GUI_INC) -o $@ $(GUI_SRC) $(IMGUI_SRC) $(TE_SRC) $(GUI_LIBS) $(LDFLAGS)

clean:
	rm -rf $(BUILD)

.PHONY: all clean
