CC = gcc
CFLAGS = -std=c99 -Werror -Wall -Wextra -Wpedantic
CFLAGS_DEBUG = -g -O0
CFLAGS_RELEASE = -O3
LDFLAGS = -lavformat -lavcodec -lavutil -lz -lm -lpthread -lbz2 -llzma -lswresample
EXE = analyze-keyframes
EXE_DEBUG = analyze-keyframes_debug

.PHONY: clean debug release

all: release

debug: $(EXE_DEBUG)

release: $(EXE)

$(EXE): analyze-keyframes.c
	$(CC) $(CFLAGS) $(CFLAGS_RELEASE) -o $@ $< $(LDFLAGS)

$(EXE_DEBUG): analyze-keyframes.c
	$(CC) $(CFLAGS) $(CFLAGS_DEBUG) -o $@ $< $(LDFLAGS)

clean:
	-rm -f $(EXE) $(EXE_DEBUG)
