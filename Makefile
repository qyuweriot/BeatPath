# ------------------------------------------------------------
# BeatPath DX Makefile
#  - Apple Silicon: /opt/homebrew の sdl2-config を優先
#  - arm64 Mac で arm64版SDL2 が無い場合は自動で -arch x86_64 (Rosetta) ビルド
#  - 手動指定: make SDL2_CONFIG=/path/to/sdl2-config
# ------------------------------------------------------------
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

SDL2_CONFIG ?= $(shell command -v /opt/homebrew/bin/sdl2-config 2>/dev/null || command -v sdl2-config)

ARCHFLAG :=
ifeq ($(UNAME_S),Darwin)
ifeq ($(UNAME_M),arm64)
ifeq ($(findstring /opt/homebrew,$(SDL2_CONFIG)),)
  # arm64 Mac だが Homebrew(arm64) の SDL2 が無い -> x86_64 でビルドして Rosetta 実行
  ARCHFLAG := -arch x86_64
endif
endif
endif

CXX      ?= g++
# sdl2-config --cflags returns -I.../include/SDL2, but main.cpp uses <SDL2/SDL.h>
# so we strip the trailing /SDL2 to get the parent include dir
SDL2_CFLAGS := $(shell $(SDL2_CONFIG) --cflags | sed 's|-I\([^ ]*/include\)/SDL2|-I\1|g')
CXXFLAGS  = -O2 -std=c++17 -Wall $(ARCHFLAG) $(SDL2_CFLAGS)
LIBS      = $(ARCHFLAG) $(shell $(SDL2_CONFIG) --libs) -lm

beatpath: main.cpp
	$(CXX) $(CXXFLAGS) -o $@ main.cpp $(LIBS)

run: beatpath
	./beatpath

clean:
	rm -f beatpath

.PHONY: run clean