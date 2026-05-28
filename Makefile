# Makefile for axctl -- Universal IPC daemon for Wayland compositors
#
# Dependencies:
#   - libjson-c-dev  (JSON parsing)
#   - libwayland-dev (Wayland client for Mango backend + idle manager)
#
# Usage:
#   make          -- Build axctl binary
#   make clean    -- Remove build artifacts
#   make install  -- Install to /usr/local/bin

CC       ?= gcc
CFLAGS   := -Wall -Wextra -Wno-unused-parameter -std=c11 -D_GNU_SOURCE \
	    -Iinclude \
	    $(shell pkg-config --cflags json-c wayland-client 2>/dev/null)

LDFLAGS  := $(shell pkg-config --libs json-c wayland-client 2>/dev/null) \
	    -lpthread

# Source files
SRCS := \
    src/main.c \
    src/utils/log.c \
    src/utils/strutil.c \
    src/utils/json_helpers.c \
    src/ipc/errors.c \
    src/ipc/types.c \
    src/ipc/colors.c \
    src/ipc/cache.c \
    src/ipc/config_types.c \
    src/ipc/compositor.c \
    src/ipc/hyprland/client.c \
    src/ipc/hyprland/generator.c \
    src/ipc/niri/client.c \
    src/ipc/niri/generator.c \
    src/ipc/mango/client.c \
    src/ipc/mango/generator.c \
    src/ipc/wayland/wayland_client.c \
    src/protocols/ext-idle-notify-v1-protocol.c \
    src/protocols/idle-inhibit-unstable-v1-protocol.c \
    src/server/server.c \
    src/server/idle.c \
    src/server/config_handler.c \
    src/config/config.c

OBJDIR := build
OBJS   := $(patsubst src/%.c,$(OBJDIR)/%.o,$(SRCS))
TARGET := axctl

.PHONY: all clean install test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo "Build complete: $(TARGET)"

$(OBJDIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

TEST_SRCS := test/test_axctl.c
TEST_OBJS := \
    src/utils/strutil.o \
    src/utils/json_helpers.o \
    src/ipc/types.o \
    src/ipc/errors.o \
    src/ipc/cache.o

$(OBJDIR)/test_axctl: $(TEST_SRCS) $(TEST_OBJS)
	@mkdir -p $(OBJDIR)
	$(CC) -o $@ $(TEST_SRCS) $(TEST_OBJS) $(LDFLAGS) -Iinclude

test: $(OBJDIR)/test_axctl
	@echo "Running tests..."
	@./$(OBJDIR)/test_axctl

clean:
	rm -rf $(OBJDIR) $(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) /usr/local/bin/$(TARGET)
	@echo "Installed to /usr/local/bin/$(TARGET)"
