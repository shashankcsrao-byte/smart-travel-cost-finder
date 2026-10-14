# Smart Travel Cost Finder - C backend
#
#   make            build build/smart-travel
#   make run        build and start the server (from the project folder)
#   make clean
#
# Needs a C compiler and three libraries: libcurl (HTTPS to Ola Maps),
# SQLite 3 (search history) and zlib (reading the metro GTFS zip, gzip).

CC      ?= cc
CFLAGS  ?= -O2 -g
override CFLAGS += -std=c11 -Wall -Wextra -Wno-unused-parameter -pthread -D_DEFAULT_SOURCE -D_GNU_SOURCE
override LDLIBS += -lcurl -lsqlite3 -lz -lm -pthread

BUILD   := build
CORE    := util strmap heap json graph fares routing csv zipread metro cache ratelimit olamaps planner history http
CORE_OBJ := $(CORE:%=$(BUILD)/%.o)

all: $(BUILD)/smart-travel

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c src/*.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/smart-travel: $(CORE_OBJ) $(BUILD)/server.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

run: $(BUILD)/smart-travel
	cd .. && backend-c/$(BUILD)/smart-travel

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
