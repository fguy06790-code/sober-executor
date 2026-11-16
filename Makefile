CXX      := g++
CC       := gcc
CXXFLAGS := -std=c++17 -O2 -fPIC -Wall -Wextra -Iinclude
LDFLAGS  := -ldl -lpthread

BUILD    := build
BIN      := $(BUILD)/sober-exec
PAYLOAD  := $(BUILD)/libsoberpay.so
CLIENT   := $(BUILD)/sober-cli

SRC      := src/finder.cpp src/scanner.cpp src/injector.cpp src/main.cpp
PAYSRC   := payload/entry.cpp payload/luau_api.cpp payload/hooks.cpp payload/ipc_server.cpp payload/scheduler.cpp
CLISRC   := client/cli.cpp

all: $(BIN) $(PAYLOAD) $(CLIENT)

$(BUILD):
	@mkdir -p $(BUILD)

$(BIN): $(SRC) | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC) $(LDFLAGS)

$(PAYLOAD): $(PAYSRC) | $(BUILD)
	$(CXX) $(CXXFLAGS) -shared -fvisibility=hidden -o $@ $(PAYSRC) $(LDFLAGS)

$(CLIENT): $(CLISRC) | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $(CLISRC)

clean:
	rm -rf $(BUILD)

.PHONY: all clean
