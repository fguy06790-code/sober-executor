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

# ---------------------------------------------------------------------------
# install targets
# ---------------------------------------------------------------------------
PREFIX    ?= /usr
DESTDIR   ?=

BINDIR    := $(DESTDIR)$(PREFIX)/bin
LIBDIR    := $(DESTDIR)$(PREFIX)/lib/sober-exec
DESKTOPDIR:= $(DESTDIR)$(PREFIX)/share/applications
ICONDIR   := $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps
DOCDIR    := $(DESTDIR)$(PREFIX)/share/doc/sober-exec
LICENSEDIR:= $(DESTDIR)$(PREFIX)/share/licenses/sober-exec

install: all
	install -Dm755 $(GUI)     $(BINDIR)/sober-exec-gui
	install -Dm755 $(CLIENT)  $(BINDIR)/sober-cli
	install -Dm755 $(BIN)     $(BINDIR)/sober-exec
	install -Dm755 packaging/sober-exec-launch.sh $(BINDIR)/sober-exec-launch
	install -Dm755 $(PAYLOAD) $(LIBDIR)/libsoberpay.so
	install -Dm644 packaging/sober-exec.desktop $(DESKTOPDIR)/sober-exec.desktop
	install -Dm644 packaging/sober-exec.svg     $(ICONDIR)/sober-exec.svg
	install -Dm644 README.md                    $(DOCDIR)/README.md

uninstall:
	rm -f $(BINDIR)/sober-exec-gui
	rm -f $(BINDIR)/sober-cli
	rm -f $(BINDIR)/sober-exec
	rm -f $(BINDIR)/sober-exec-launch
	rm -rf $(LIBDIR)
	rm -f $(DESKTOPDIR)/sober-exec.desktop
	rm -f $(ICONDIR)/sober-exec.svg
	rm -rf $(DOCDIR)

.PHONY: all clean install uninstall
