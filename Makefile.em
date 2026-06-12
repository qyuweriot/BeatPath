# ------------------------------------------------------------
# BeatPath DX - Web ビルド (Emscripten)
#
# 使い方:
#   source ~/emsdk/emsdk_env.sh   # 毎ターミナル起動時
#   make -f Makefile.em            # docs/ に出力
#
# ローカル確認:
#   npx serve docs                 # http://localhost:3000 で確認
# ------------------------------------------------------------
CXX = em++

CXXFLAGS = -std=c++17 -O2

LDFLAGS  = -s USE_SDL=2 \
           -s ALLOW_MEMORY_GROWTH=1 \
           -s INITIAL_MEMORY=33554432 \
           -s EXPORTED_RUNTIME_METHODS='["stringToUTF8","UTF8ToString"]' \
           --shell-file shell.html

OUT = docs/index.html

$(OUT): main.cpp stages.h audio_params.h shell.html
	mkdir -p docs
	$(CXX) $(CXXFLAGS) -o $(OUT) main.cpp $(LDFLAGS)

clean_web:
	rm -rf docs/

.PHONY: clean_web
