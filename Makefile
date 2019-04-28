CC = g++
CFLAGS = -std=c++11 -Werror -Wall -Wextra -Wpedantic
CFLAGS_DEBUG = -g -O0
CFLAGS_RELEASE = -O3
LDFLAGS = -lavformat -lavcodec -lavutil -lz -lm -lpthread -lbz2 -llzma -lswresample -lswscale
EXE = analyze-keyframes
EXE_DEBUG = analyze-keyframes_debug

.PHONY: clean debug release

all: release

debug: $(EXE_DEBUG)

release: $(EXE)

$(EXE): analyze-keyframes.cpp
	$(CC) $(CFLAGS) $(CFLAGS_RELEASE) -o $@ $< $(LDFLAGS)

$(EXE_DEBUG): analyze-keyframes.cpp
	$(CC) $(CFLAGS) $(CFLAGS_DEBUG) -o $@ $< $(LDFLAGS)

clean:
	-rm -f $(EXE) $(EXE_DEBUG)
