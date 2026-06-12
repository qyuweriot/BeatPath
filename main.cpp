// ============================================================
//  BeatPath DX - インタラクティブミュージック × タイル配置パズル
//  C++17 / SDL2 (描画・入力・リアルタイム音声合成 / 外部アセット不要)
//
//  コンセプト:
//   - 常に一定のリズムが鳴り続け、毎小節の頭にスタートから球が発射
//   - 球は1拍ごとに1マス直進し、ギミックタイルで効果+音が鳴る
//   - 試行錯誤の過程そのものが曲になり、完成に近づくほど豪華になる
//
//  ギミック: 右折 / 左折 / 二分裂 / 三分裂 / 加速(2倍) / 減速(1/2)
//            / 1拍停止 / ペイント(球の色変更=音色変更)
//  ステージ要素: ワープ / 色付きゴール / 複数スタート
//
//  音楽: キック+8分ハット+ベース+パッド+メロディの5レイヤー
//        Am -> F -> C -> G の4小節コード進行 / 球の色で音色が変化
//        球が3個以上で16分シェイカー追加
//
//  操作: マウスドラッグ&ドロップ / R リセット / N 次へ / M セレクト
//        - + 音量 / ESC 戻る
// ============================================================

#include <SDL2/SDL.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "stages.h"
#include "audio_params.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ------------------------------------------------------------
// 設定 (beatpath.cfg から起動時に読み込み)
// ------------------------------------------------------------
struct Config {
    int gridW    = 7;
    int gridH    = 5;
    int maxCell  = 120;  // CELL キーから読む。セルサイズの上限
    int cell     = 80;   // computeDerived() が計算する実際のセルサイズ
    int volume   = 80;
    int menuBpm  = 112;
    int maxBalls = 64;

    // 派生レイアウト (computeDerived() で計算)
    int gridX  = 40;
    int gridY  = 140;
    int panelX = 0;
    int winW   = 0;
    int winH   = 0;

    void computeDerived() {
        static constexpr int WIN_W    = 1280;
        static constexpr int WIN_H    = 720;
        static constexpr int HEADER_H = 140;
        static constexpr int FOOTER_H = 80;
        static constexpr int PANEL_W  = 220;
        static constexpr int LEFT_M   = 40;
        static constexpr int GRID_GAP = 20;

        winW   = WIN_W;
        winH   = WIN_H;
        panelX = WIN_W - PANEL_W;                  // 1060

        int availW  = panelX - LEFT_M - GRID_GAP;  // 1000
        int availH  = WIN_H - HEADER_H - FOOTER_H; // 500

        int cellFit = std::min(availW / gridW, availH / gridH);
        cell        = std::max(30, std::min(maxCell, cellFit));

        gridX  = LEFT_M   + (availW - gridW * cell) / 2;
        gridY  = HEADER_H + (availH - gridH * cell) / 2;
    }
};
static Config cfg;


static const SDL_Color BALLCOL[3] = {
    {235,  85,  95, 255},   // 0: RED
    { 85, 155, 245, 255},   // 1: BLUE
    {245, 205,  70, 255},   // 2: YELLOW
};

// ------------------------------------------------------------
// オーディオ (リアルタイム合成)
// ------------------------------------------------------------
struct Voice {
    bool   active = false;
    int    type   = 0;
    int    wave   = 0;       // PLUCK音色: 0標準 1スクエア 2サイン 3トライアングル
    double t = 0, phase = 0, phase2 = 0, freq = 0, amp = 0;
    float  hp = 0;
};

struct AudioState {
    static const int MAXV = 96;
    Voice voices[MAXV];
    long  samplePos  = 0;
    int   sampleRate = 48000;
    int   melodyStep = 0;
    unsigned rng = 0x1234567u;
    std::atomic<long> totalBeats{0};
    std::atomic<int>  bpm{112};
    std::atomic<int>  beatsPM{4};
    std::atomic<int>  layer{0};       // bit0:ハット(球生存) bit1:ベース(1枚設置) bit2:パッド(2枚設置) bit3:メロディ(クリア)
    std::atomic<int>  ballLayer{0};   // 球2個以上 -> 16分シェイカー
    std::atomic<int>  vol100{80};     // マスター音量 0-100
};

static AudioState A;
static SDL_AudioDeviceID g_dev = 0;
static bool g_audioOK = false;

static float frnd(unsigned& s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return (int)s / 2147483648.0f;
}

static void addVoiceRaw(int type, double freq, double amp, int wave = 0) {
    for (int i = 0; i < AudioState::MAXV; i++) {
        if (!A.voices[i].active) {
            Voice& v = A.voices[i];
            v.active = true; v.type = type; v.wave = wave;
            v.t = 0; v.phase = 0; v.phase2 = 0;
            v.freq = freq; v.amp = amp; v.hp = 0;
            return;
        }
    }
}

static void addVoice(int type, double freq, double amp, int wave = 0) {
    if (g_dev) SDL_LockAudioDevice(g_dev);
    addVoiceRaw(type, freq, amp, wave);
    if (g_dev) SDL_UnlockAudioDevice(g_dev);
}

static void audioCallback(void*, Uint8* stream, int len) {
    float* out = (float*)stream;
    int n = len / (int)sizeof(float);
    const int sr = A.sampleRate;
    const float vol = A.vol100.load() / 100.0f;

    for (int i = 0; i < n; i++) {
        long beatLen = (long)(sr * 60.0 / A.bpm.load());
        long half    = beatLen / 2;
        long quart   = beatLen / 4;
        int  bpmeas  = A.beatsPM.load();
        int  layer   = A.layer.load();

        // ---- 拍頭/裏拍/16分のスケジューリング (サンプル精度) ----
        if (beatLen > 0 && A.samplePos % beatLen == 0) {
            long beatNum = A.samplePos / beatLen;
            int  idx  = (int)(beatNum % bpmeas);
            int  prog = (int)((beatNum / bpmeas) % 4);          // コード進行位置
            addVoiceRaw(V_KICK, 0, idx == 0 ? KICK_AMP_DOWN : KICK_AMP_WEAK);
            if (idx == 0) addVoiceRaw(V_RIDE, 0, CRASH_AMP);   // 小節頭シンバル
            if (layer & 2) {                                     // ベース (1枚以上設置)
                if (idx == 0) {
                    addVoiceRaw(V_BASS, BASS_ROOT[prog], BASS_AMP_ROOT);
                } else if (idx == bpmeas / 2) {
                    addVoiceRaw(V_BASS, BASS_FIFTH[prog], BASS_AMP_FIFTH);
                }
            }
            if (layer & 4) {                                     // コードパッド (2枚以上設置)
                if (idx == 0) {
                    for (int c = 0; c < 3; c++)
                        addVoiceRaw(V_PAD, PAD_CH[prog][c], PAD_AMP);
                }
            }
            if (layer & 8) {                                     // メロディ (クリア)
                addVoiceRaw(V_PLUCK, MELODY[prog][A.melodyStep % 4], MELODY_AMP);
                A.melodyStep++;
            }
            A.totalBeats.fetch_add(1);
        } else if ((layer & 1) && half > 0 && A.samplePos % beatLen == half) {
            addVoiceRaw(V_HAT, 0, HAT_AMP);                      // 8分ハット
        } else if ((layer & 2) && A.ballLayer.load() && quart > 0 &&
                   (A.samplePos % beatLen == quart || A.samplePos % beatLen == 3 * quart)) {
            addVoiceRaw(V_HAT, 0, SHAKER_AMP);                   // 16分シェイカー
        }

        // ---- ボイスのレンダリング ----
        double mix = 0;
        for (int vi = 0; vi < AudioState::MAXV; vi++) {
            Voice& v = A.voices[vi];
            if (!v.active) continue;
            double s = 0;
            switch (v.type) {
                case V_KICK: {
                    double f = 45.0 + 115.0 * exp(-v.t * 22.0);
                    v.phase += 2.0 * M_PI * f / sr;
                    s = sin(v.phase) * exp(-v.t * 8.0) * 0.9 * v.amp;
                    if (v.t > 0.6) v.active = false;
                } break;
                case V_PLUCK: {
                    v.phase += 2.0 * M_PI * v.freq / sr;
                    double base;
                    switch (v.wave) {
                        case 1:  base = tanh(2.5 * sin(v.phase)) * 0.8; break;          // スクエア風
                        case 2:  base = sin(v.phase); break;                            // ピュアサイン
                        case 3:  base = (2.0 / M_PI) * asin(sin(v.phase)); break;       // トライアングル
                        default: base = sin(v.phase) + 0.35 * sin(2 * v.phase)
                                        + 0.15 * sin(3 * v.phase);                      // 撥弦風
                    }
                    s = base * exp(-v.t * 5.0) * 0.5 * v.amp;
                    if (v.t > 1.4) v.active = false;
                } break;
                case V_HAT: {
                    float nz = frnd(A.rng);
                    s = (nz - v.hp) * 0.5;
                    v.hp = nz;
                    s *= exp(-v.t * 60.0) * 0.30 * v.amp;
                    if (v.t > 0.15) v.active = false;
                } break;
                case V_BASS: {
                    v.phase += 2.0 * M_PI * v.freq / sr;
                    s = (sin(v.phase) * 0.9 + 0.2 * sin(2 * v.phase))
                        * exp(-v.t * 5.0) * 0.5 * v.amp;
                    if (v.t > 1.0) v.active = false;
                } break;
                case V_BELL: {
                    v.phase += 2.0 * M_PI * v.freq / sr;
                    s = (sin(v.phase) * exp(-v.t * 3.5)
                         + 0.3 * sin(2.76 * v.phase) * exp(-v.t * 7.0)) * 0.45 * v.amp;
                    if (v.t > 1.8) v.active = false;
                } break;
                case V_CLICK: {
                    v.phase += 2.0 * M_PI * v.freq / sr;
                    s = sin(v.phase) * exp(-v.t * 60.0) * 0.4 * v.amp;
                    if (v.t > 0.12) v.active = false;
                } break;
                case V_SNARE: { // 壁破壊
                    float nz = frnd(A.rng);
                    double noise = (nz - v.hp) * 0.7; v.hp = nz;
                    v.phase += 2.0 * M_PI * 185.0 / sr;
                    s = (noise * exp(-v.t * 22.0) * 0.6
                         + sin(v.phase) * exp(-v.t * 28.0) * 0.45) * v.amp;
                    if (v.t > 0.4) v.active = false;
                } break;
                case V_SWEEP: { // ワープ (上昇スイープ)
                    double f = v.freq * pow(2.0, v.t * 3.0);
                    if (f > 4000) f = 4000;
                    v.phase += 2.0 * M_PI * f / sr;
                    s = sin(v.phase) * exp(-v.t * 5.0) * 0.35 * v.amp;
                    if (v.t > 0.6) v.active = false;
                } break;
                case V_PAD: { // 柔らかいコードパッド (デチューン2声)
                    v.phase  += 2.0 * M_PI * v.freq / sr;
                    v.phase2 += 2.0 * M_PI * v.freq * 1.006 / sr;
                    double env = std::min(v.t * 6.0, 1.0) * exp(-v.t * 1.2);
                    s = (sin(v.phase) + sin(v.phase2)) * 0.5 * 0.13 * env * v.amp;
                    if (v.t > 3.0) v.active = false;
                } break;
                case V_MARIMBA: { // 木琴・マリンバ
                    v.phase += 2.0 * M_PI * v.freq / sr;
                    s = (sin(v.phase) * exp(-v.t * 7.0)
                         + 0.15 * sin(4.0 * v.phase) * exp(-v.t * 20.0))
                        * 0.5 * v.amp;
                    if (v.t > 0.8) v.active = false;
                } break;
                case V_TOM: { // タム (有音程キック)
                    double f = v.freq * (1.0 + 0.8 * exp(-v.t * 15.0));
                    v.phase += 2.0 * M_PI * f / sr;
                    s = sin(v.phase) * exp(-v.t * 12.0) * 0.7 * v.amp;
                    if (v.t > 0.5) v.active = false;
                } break;
                case V_ORGAN: { // オルガン (ドローバー風、持続音)
                    v.phase += 2.0 * M_PI * v.freq / sr;
                    double env = std::min(v.t * 15.0, 1.0) * exp(-v.t * 0.6);
                    s = (sin(v.phase) * 0.8
                         + sin(2.0 * v.phase) * 0.5
                         + sin(3.0 * v.phase) * 0.2
                         + sin(4.0 * v.phase) * 0.1)
                        * 0.15 * env * v.amp;
                    if (v.t > 2.5) v.active = false;
                } break;
                case V_RIDE: { // ライドシンバル (高域ノイズ変調)
                    float nz = frnd(A.rng);
                    double hipass = (nz - v.hp) * 0.9; v.hp = nz;
                    // ノイズを非整数比の高域キャリアで振幅変調 → ピッチ感のないシマー
                    v.phase  += 2.0 * M_PI * 5200.0 / sr;
                    v.phase2 += 2.0 * M_PI * 8300.0 / sr;
                    double shimmer = hipass * (1.0 + 0.5 * sin(v.phase) + 0.3 * sin(v.phase2));
                    s = shimmer * exp(-v.t * 8.0) * 0.30 * v.amp;
                    if (v.t > 0.7) v.active = false;
                } break;
                case V_MARIMBA2: { // タンタン (16分音符 2打)
                    v.phase += 2.0 * M_PI * v.freq / sr;
                    double dt = 60.0 / (A.bpm.load() * 4.0);
                    double e1 = exp(-v.t * 16.0);
                    double e2 = v.t > dt ? exp(-(v.t - dt) * 16.0) : 0.0;
                    double h1 = exp(-v.t * 50.0);
                    double h2 = v.t > dt ? exp(-(v.t - dt) * 50.0) : 0.0;
                    s = (sin(v.phase) * (e1 + e2)
                         + 0.15 * sin(4.0 * v.phase) * (h1 + h2))
                        * 0.5 * v.amp;
                    if (v.t > 2.0 * dt + 0.25) v.active = false;
                } break;
                case V_MARIMBA3: { // タタタン (拍を4等分して前3打、4打目は休符)
                    v.phase += 2.0 * M_PI * v.freq / sr;
                    double dt = 60.0 / (A.bpm.load() * 4.0);
                    double e1 = exp(-v.t * 16.0);
                    double e2 = v.t > dt     ? exp(-(v.t - dt)     * 16.0) : 0.0;
                    double e3 = v.t > 2.0*dt ? exp(-(v.t - 2.0*dt) * 16.0) : 0.0;
                    double h1 = exp(-v.t * 50.0);
                    double h2 = v.t > dt     ? exp(-(v.t - dt)     * 50.0) : 0.0;
                    double h3 = v.t > 2.0*dt ? exp(-(v.t - 2.0*dt) * 50.0) : 0.0;
                    s = (sin(v.phase) * (e1 * 0.85 + e2 * 0.85 + e3)
                         + 0.15 * sin(4.0 * v.phase) * (h1 * 0.85 + h2 * 0.85 + h3))
                        * 0.5 * v.amp;
                    if (v.t > 3.0 * dt + 0.25) v.active = false;
                } break;
            }
            v.t += 1.0 / sr;
            mix += s;
        }
        out[i] = (float)(tanh(mix * 0.9) * 0.85) * vol;
        A.samplePos++;
    }
}

static void resetAudioTimeline() {
    if (g_dev) SDL_LockAudioDevice(g_dev);
    A.samplePos = 0;
    A.melodyStep = 0;
    A.totalBeats.store(0);
    for (auto& v : A.voices) v.active = false;
    if (g_dev) SDL_UnlockAudioDevice(g_dev);
}

// ------------------------------------------------------------
// 5x7 ビットマップフォント
// ------------------------------------------------------------
static const char* FONT_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!:.-";
static const unsigned char FONT[][7] = {
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E},{0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},{0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11},{0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},{0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},{0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},{0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},{0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    {0x04,0x04,0x04,0x04,0x04,0x00,0x04},{0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C},{0x00,0x00,0x00,0x0E,0x00,0x00,0x00},
};

static void drawText(SDL_Renderer* r, int x, int y, int scale, const char* s) {
    for (; *s; s++) {
        char c = (char)toupper((unsigned char)*s);
        if (c == ' ') { x += 6 * scale; continue; }
        const char* p = strchr(FONT_CHARS, c);
        if (!p) { x += 6 * scale; continue; }
        const unsigned char* g = FONT[p - FONT_CHARS];
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++)
                if (g[row] & (1 << (4 - col))) {
                    SDL_Rect rc{ x + col * scale, y + row * scale, scale, scale };
                    SDL_RenderFillRect(r, &rc);
                }
        x += 6 * scale;
    }
}
static int textWidth(int scale, const char* s) { return 6 * scale * (int)strlen(s); }

// ------------------------------------------------------------
// 描画ヘルパー
// ------------------------------------------------------------
static void fillCircle(SDL_Renderer* r, int cx, int cy, int rad) {
    for (int dy = -rad; dy <= rad; dy++) {
        int dx = (int)std::sqrt((double)(rad * rad - dy * dy));
        SDL_RenderDrawLine(r, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}
static void drawCircle(SDL_Renderer* r, int cx, int cy, int rad) {
    const int N = 48;
    for (int i = 0; i < N; i++) {
        double a0 = 2 * M_PI * i / N, a1 = 2 * M_PI * (i + 1) / N;
        SDL_RenderDrawLine(r, (int)(cx + rad * cos(a0)), (int)(cy + rad * sin(a0)),
                              (int)(cx + rad * cos(a1)), (int)(cy + rad * sin(a1)));
    }
}
static void drawThickLine(SDL_Renderer* r, int x1, int y1, int x2, int y2, int w = 2) {
    double dx = x2 - x1, dy = y2 - y1;
    double len = std::sqrt(dx*dx + dy*dy);
    if (len < 0.5) return;
    double px = -dy/len, py = dx/len;
    for (int i = -(w/2); i <= w/2; i++)
        SDL_RenderDrawLine(r, (int)(x1+i*px+.5), (int)(y1+i*py+.5),
                              (int)(x2+i*px+.5), (int)(y2+i*py+.5));
}
static void drawArrowHead(SDL_Renderer* r, double px, double py, double angle) {
    double l = 9;
    drawThickLine(r, (int)px, (int)py,
                  (int)(px + l * cos(angle + 2.6)), (int)(py + l * sin(angle + 2.6)));
    drawThickLine(r, (int)px, (int)py,
                  (int)(px + l * cos(angle - 2.6)), (int)(py + l * sin(angle - 2.6)));
}
static void drawArc(SDL_Renderer* r, double cx, double cy, double rad, double a0, double a1) {
    const int N = 28;
    for (int i = 0; i < N; i++) {
        double t0 = a0 + (a1 - a0) * i / N;
        double t1 = a0 + (a1 - a0) * (i + 1) / N;
        drawThickLine(r, (int)(cx + rad * cos(t0)), (int)(cy + rad * sin(t0)),
                         (int)(cx + rad * cos(t1)), (int)(cy + rad * sin(t1)));
    }
}
static void drawChevron(SDL_Renderer* r, double cx, double cy, double s) {
    drawThickLine(r, (int)(cx - s), (int)(cy - s), (int)(cx + s * 0.4), (int)cy);
    drawThickLine(r, (int)(cx + s * 0.4), (int)cy, (int)(cx - s), (int)(cy + s));
}

static void tileColor(TileType t, Uint8& cr, Uint8& cg, Uint8& cb) {
    switch (t) {
        case T_TURN_R:  cr = 240; cg = 150; cb =  60; break;
        case T_TURN_L:  cr =  90; cg = 160; cb = 240; break;
        case T_SPLIT:   cr = 170; cg = 110; cb = 230; break;
        case T_SPLIT3:  cr = 230; cg =  90; cb = 160; break;
        case T_SPEED2:  cr =  90; cg = 220; cb = 130; break;
        case T_SLOW:    cr = 140; cg = 200; cb = 215; break;
        case T_STOP:    cr = 235; cg = 110; cb = 100; break;
        case T_PAINT_R: cr = BALLCOL[0].r; cg = BALLCOL[0].g; cb = BALLCOL[0].b; break;
        case T_PAINT_B: cr = BALLCOL[1].r; cg = BALLCOL[1].g; cb = BALLCOL[1].b; break;
        case T_PAINT_Y: cr = BALLCOL[2].r; cg = BALLCOL[2].g; cb = BALLCOL[2].b; break;
        default:        cr = 200; cg = 200; cb = 200; break;
    }
}

// ギミックタイルのアイコン (x,y = 左上, size = 一辺)
static void drawTileIcon(SDL_Renderer* r, TileType t, int x, int y, int size, Uint8 alpha) {
    double cx = x + size / 2.0, cy = y + size / 2.0;
    double s = size * 0.30;
    Uint8 cr, cg, cb;
    tileColor(t, cr, cg, cb);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_Rect bg{ x + (int)(size * 0.08), y + (int)(size * 0.08),
                 (int)(size * 0.84), (int)(size * 0.84) };
    SDL_SetRenderDrawColor(r, cr, cg, cb, (Uint8)(alpha / 5));
    SDL_RenderFillRect(r, &bg);
    SDL_SetRenderDrawColor(r, cr, cg, cb, alpha);
    SDL_RenderDrawRect(r, &bg);

    switch (t) {
        case T_TURN_R: {
            double a0 = -2.0, a1 = 2.4;
            drawArc(r, cx, cy, s, a0, a1);
            drawArrowHead(r, cx + s * cos(a1), cy + s * sin(a1), a1 + M_PI / 2);
        } break;
        case T_TURN_L: {
            double a0 = M_PI + 2.0, a1 = M_PI - 2.4;
            drawArc(r, cx, cy, s, a0, a1);
            drawArrowHead(r, cx + s * cos(a1), cy + s * sin(a1), a1 - M_PI / 2);
        } break;
        case T_SPLIT: {
            drawThickLine(r, (int)(cx - s), (int)cy, (int)cx, (int)cy);
            drawThickLine(r, (int)cx, (int)cy, (int)cx, (int)(cy - s));
            drawThickLine(r, (int)cx, (int)cy, (int)cx, (int)(cy + s));
            drawArrowHead(r, cx, cy - s, -M_PI / 2);
            drawArrowHead(r, cx, cy + s,  M_PI / 2);
        } break;
        case T_SPLIT3: {
            drawThickLine(r, (int)(cx - s), (int)cy, (int)(cx + s), (int)cy);
            drawThickLine(r, (int)cx, (int)cy, (int)cx, (int)(cy - s));
            drawThickLine(r, (int)cx, (int)cy, (int)cx, (int)(cy + s));
            drawArrowHead(r, cx + s, cy, 0);
            drawArrowHead(r, cx, cy - s, -M_PI / 2);
            drawArrowHead(r, cx, cy + s,  M_PI / 2);
        } break;
        case T_SPEED2: { // 二重シェブロン
            drawChevron(r, cx - s * 0.35, cy, s * 0.55);
            drawChevron(r, cx + s * 0.55, cy, s * 0.55);
        } break;
        case T_SLOW: {   // シェブロン + X0.5
            drawChevron(r, cx, cy - s * 0.3, s * 0.5);
            drawText(r, (int)(cx - 12), (int)(cy + s * 0.25), 1, "X0.5");
        } break;
        case T_STOP: {   // ポーズバー
            SDL_Rect b1{ (int)(cx - s * 0.55), (int)(cy - s * 0.7), (int)(s * 0.35), (int)(s * 1.4) };
            SDL_Rect b2{ (int)(cx + s * 0.20), (int)(cy - s * 0.7), (int)(s * 0.35), (int)(s * 1.4) };
            SDL_RenderFillRect(r, &b1);
            SDL_RenderFillRect(r, &b2);
        } break;
        case T_PAINT_R: case T_PAINT_B: case T_PAINT_Y: { // ペンキの雫
            fillCircle(r, (int)cx, (int)(cy + s * 0.15), (int)(s * 0.6));
            SDL_RenderDrawLine(r, (int)cx, (int)(cy - s * 0.9), (int)(cx - s * 0.5), (int)(cy + s * 0.05));
            SDL_RenderDrawLine(r, (int)cx, (int)(cy - s * 0.9), (int)(cx + s * 0.5), (int)(cy + s * 0.05));
        } break;
        default: break;
    }
}

// ------------------------------------------------------------
// ゲームデータ
// ------------------------------------------------------------
struct Ball {
    int x = 0, y = 0, px = 0, py = 0, dx = 1, dy = 0;
    int color = -1;          // -1: 白 / 0-2: BALLCOL
    int speed = 1;           // 2 = 1拍2マス
    bool slow = false;       // 2拍1マス
    bool slowPhase = false;
    bool oneTimeFast = false; // 次の1拍だけ2倍速
    bool oneTimeSlow = false; // 次の1拍だけスキップ(半速)
    int stopBeats = 0;       // 残り停止拍数
    bool alive = true;
    float hx[10], hy[10];    // 残像用の描画位置履歴
    int hn = 0, hi = 0;
};


struct Flash { int x, y; double t; };
struct Particle { float x, y, vx, vy, life, max; Uint8 r, g, b; };

enum Scene { SC_TITLE, SC_SELECT, SC_GAME };

struct Game {
    std::vector<StageDef> stages = makeStages();
    int curStage = 0;
    Scene scene = SC_TITLE;

    std::vector<std::vector<TileType>> grid;  // grid[y][x]
    std::vector<Ball> balls;
    std::vector<int>  goalLit;
    std::vector<bool> goalHitAtHead;
    std::vector<std::pair<TileType, int>> inv;
    std::vector<Flash> flashes;
    std::vector<Particle> parts;

    bool cleared = false;
    bool everTriggered = false;
    int  goalsEverHit = 0;
    long processedBeats = 0;
    int  lastBeatIdx = 0;
    Uint32 lastBeatTick = 0;
    double shake = 0;

    bool dragging = false;
    TileType dragType = T_EMPTY;

    std::vector<bool> clearedStages;
};

static Game G;
static double g_fallbackAccum = 0;
static long   g_fallbackBeats = 0;

static const StageDef& stage() { return G.stages[G.curStage]; }

// ------------------------------------------------------------
// セーブ / ロード
// ------------------------------------------------------------
static const char* SAVE_FILE = "beatpath_save.dat";

static void saveProgress() {
    FILE* f = fopen(SAVE_FILE, "w");
    if (!f) return;
    for (bool c : G.clearedStages) fputc(c ? '1' : '0', f);
    fclose(f);
}
static void loadProgress() {
    G.clearedStages.assign(G.stages.size(), false);
    FILE* f = fopen(SAVE_FILE, "r");
    if (!f) return;
    for (size_t i = 0; i < G.clearedStages.size(); i++) {
        int c = fgetc(f);
        if (c == EOF) break;
        G.clearedStages[i] = (c == '1');
    }
    fclose(f);
}
static bool stageUnlocked(int i) {
    return i == 0 || G.clearedStages[i - 1];
}

// ------------------------------------------------------------
// 盤面ユーティリティ
// ------------------------------------------------------------
static bool isStart(int cx, int cy) {
    for (auto& s : stage().starts) if (s.x == cx && s.y == cy) return true;
    return false;
}
static int goalIndexAt(int cx, int cy) {
    for (size_t i = 0; i < stage().goals.size(); i++) {
        auto& g = stage().goals[i];
        if (g.x >= cfg.gridW || g.y >= cfg.gridH) continue; // OOB
        if (g.x == cx && g.y == cy) return (int)i;
    }
    return -1;
}
static bool warpAt(int cx, int cy, int& ox, int& oy) {
    for (auto& w : stage().warps) {
        if (w.x1 == cx && w.y1 == cy) { ox = w.x2; oy = w.y2; return true; }
        if (w.x2 == cx && w.y2 == cy) { ox = w.x1; oy = w.y1; return true; }
    }
    return false;
}
static bool isWarpCell(int cx, int cy) { int a, b; return warpAt(cx, cy, a, b); }
static bool cellPlaceable(int cx, int cy) {
    return G.grid[cy][cx] == T_EMPTY && !isStart(cx, cy) &&
           goalIndexAt(cx, cy) < 0 && !isWarpCell(cx, cy);
}

static void spawnBurst(double px, double py, int n, Uint8 r, Uint8 g, Uint8 b, double speed) {
    static unsigned prng = 0xBEEFu;
    for (int i = 0; i < n; i++) {
        double a = frnd(prng) * M_PI;
        double v = (0.4 + 0.6 * fabs(frnd(prng))) * speed;
        Particle p;
        p.x = (float)px; p.y = (float)py;
        p.vx = (float)(cos(a * 2) * v);
        p.vy = (float)(sin(a * 2) * v - speed * 0.3);
        p.max = p.life = 0.45f + 0.3f * fabsf(frnd(prng));
        p.r = r; p.g = g; p.b = b;
        G.parts.push_back(p);
    }
}
static void cellBurst(int cx, int cy, Uint8 r, Uint8 g, Uint8 b, int n, double speed) {
    spawnBurst(cfg.gridX + cx * cfg.cell + cfg.cell / 2.0,
               cfg.gridY + cy * cfg.cell + cfg.cell / 2.0,
               n, r, g, b, speed);
}

static int colorWave(int color) { return color < 0 ? 0 : 1 + color; }

// ------------------------------------------------------------
// ステージロード
// ------------------------------------------------------------
static void loadStage(int idx) {
    G.curStage = idx;
    {
        int ew = stage().gridW > 0 ? stage().gridW : 7;
        int eh = stage().gridH > 0 ? stage().gridH : 5;
        cfg.gridW = ew;
        cfg.gridH = eh;
        cfg.computeDerived();
    }
    G.grid.assign(cfg.gridH, std::vector<TileType>(cfg.gridW, T_EMPTY));
    G.balls.clear();
    G.flashes.clear();
    G.parts.clear();
    G.inv = stage().inv;
    G.goalLit.assign(stage().goals.size(), 0);
    G.goalHitAtHead.assign(stage().goals.size(), false);
    G.cleared = false;
    G.everTriggered = false;
    G.goalsEverHit = 0;
    G.processedBeats = 0;
    G.lastBeatIdx = 0;
    G.lastBeatTick = SDL_GetTicks();
    G.dragging = false;
    G.shake = 0;
    A.bpm.store(stage().bpm);
    A.beatsPM.store(stage().beatsPM);
    A.layer.store(0);
    A.ballLayer.store(0);
    resetAudioTimeline();
    g_fallbackAccum = 0;
    g_fallbackBeats = 0;
    G.scene = SC_GAME;
}

static void enterMenuMusic() {
    A.bpm.store(cfg.menuBpm);
    A.beatsPM.store(4);
    A.layer.store(1);
    A.ballLayer.store(0);
    resetAudioTimeline();
    G.processedBeats = 0;
    g_fallbackAccum = 0;
    g_fallbackBeats = 0;
}

// ------------------------------------------------------------
// 球が1マス進入した時の処理 (戻り値: この拍の続きのステップを継続するか)
// ------------------------------------------------------------
static bool processCell(Ball& b, std::vector<Ball>& newBalls) {
    // 場外
    if (b.x < 0 || b.x >= cfg.gridW || b.y < 0 || b.y >= cfg.gridH) {
        b.alive = false;
        addVoice(SND_BALL_OUT.voice, SND_BALL_OUT.freq, SND_BALL_OUT.amp);
        return false;
    }
    // ゴール: 色が一致すれば点灯、不一致は素通り
    int gi = goalIndexAt(b.x, b.y);
    if (gi >= 0) {
        const GoalDef& g = stage().goals[gi];
        if (g.color < 0 || g.color == b.color) {
            G.goalLit[gi] = stage().beatsPM;
            if (G.lastBeatIdx == 0) G.goalHitAtHead[gi] = true;
            G.goalsEverHit++;
            addVoice(V_BELL, GOAL_FREQ[gi % 4], GOAL_AMP);
            Uint8 cr = 255, cg = 220, cb = 90;
            if (g.color >= 0) { cr = BALLCOL[g.color].r; cg = BALLCOL[g.color].g; cb = BALLCOL[g.color].b; }
            cellBurst(b.x, b.y, cr, cg, cb, 16, 200);
            G.flashes.push_back({b.x, b.y, 0});
            b.alive = false;
            return false;
        } else {
            addVoice(SND_GOAL_WRONG.voice, SND_GOAL_WRONG.freq, SND_GOAL_WRONG.amp);
            return true;
        }
    }
    // ワープ: 対になった出口へ (方向維持)
    int ox, oy;
    if (warpAt(b.x, b.y, ox, oy)) {
        addVoice(SND_WARP.voice, SND_WARP.freq, SND_WARP.amp);
        cellBurst(b.x, b.y, 110, 230, 230, 8, 160);
        b.x = ox; b.y = oy;
        b.px = ox; b.py = oy;             // 補間でワープ間を横切らないように
        cellBurst(b.x, b.y, 110, 230, 230, 8, 160);
        G.everTriggered = true;
        return true;
    }
    // 配置されたギミックタイル
    TileType t = G.grid[b.y][b.x];
    if (t == T_EMPTY) return true;
    Uint8 cr, cg, cb;
    tileColor(t, cr, cg, cb);
    int w = colorWave(b.color);
    switch (t) {
        case T_TURN_R: { int nx = -b.dy, ny = b.dx; b.dx = nx; b.dy = ny;
                         addVoice(TILE_SOUNDS[t].voice, TILE_SOUNDS[t].freq, TILE_SOUNDS[t].amp, w); } break;
        case T_TURN_L: { int nx = b.dy, ny = -b.dx; b.dx = nx; b.dy = ny;
                         addVoice(TILE_SOUNDS[t].voice, TILE_SOUNDS[t].freq, TILE_SOUNDS[t].amp, w); } break;
        case T_SPLIT: {
            Ball l = b, rgt = b;
            l.dx = b.dy;    l.dy = -b.dx;  l.hn = 0;  l.hi = 0;
            rgt.dx = -b.dy; rgt.dy = b.dx; rgt.hn = 0; rgt.hi = 0;
            b.alive = false;
            newBalls.push_back(l);
            newBalls.push_back(rgt);
            addVoice(TILE_SOUNDS[t].voice, TILE_SOUNDS[t].freq, TILE_SOUNDS[t].amp, w);
            G.flashes.push_back({b.x, b.y, 0});
            cellBurst(b.x, b.y, cr, cg, cb, 8, 150);
            G.everTriggered = true;
            return false;
        }
        case T_SPLIT3: {
            Ball l = b, rgt = b;
            l.dx = b.dy;    l.dy = -b.dx;  l.hn = 0;  l.hi = 0;
            rgt.dx = -b.dy; rgt.dy = b.dx; rgt.hn = 0; rgt.hi = 0;
            newBalls.push_back(l);
            newBalls.push_back(rgt);
            addVoice(TILE_SOUNDS[t].voice, TILE_SOUNDS[t].freq, TILE_SOUNDS[t].amp, w);
        } break;
        case T_SPEED2: { b.oneTimeFast = true;
                         addVoice(TILE_SOUNDS[t].voice, TILE_SOUNDS[t].freq, TILE_SOUNDS[t].amp, w); } break;
        case T_SLOW:   { b.oneTimeSlow = true;
                         addVoice(TILE_SOUNDS[t].voice, TILE_SOUNDS[t].freq, TILE_SOUNDS[t].amp, w); } break;
        case T_STOP:   { b.stopBeats = 1;
                         addVoice(TILE_SOUNDS[t].voice, TILE_SOUNDS[t].freq, TILE_SOUNDS[t].amp); } break;
        case T_PAINT_R: case T_PAINT_B: case T_PAINT_Y: {
            b.color = (t == T_PAINT_R) ? 0 : (t == T_PAINT_B) ? 1 : 2;
            addVoice(TILE_SOUNDS[t].voice, TILE_SOUNDS[t].freq, TILE_SOUNDS[t].amp);
        } break;
        default: break;
    }
    G.flashes.push_back({b.x, b.y, 0});
    cellBurst(b.x, b.y, cr, cg, cb, 6, 130);
    G.everTriggered = true;
    return true;
}

// ------------------------------------------------------------
// 1拍ぶんのゲームロジック
// ------------------------------------------------------------
static void onBeat() {
    int bpmeas = stage().beatsPM;
    int idx = (int)(G.processedBeats % bpmeas);
    G.lastBeatIdx = idx;
    G.lastBeatTick = SDL_GetTicks();
    G.shake = (idx == 0) ? 5.0 : 3.0;

    // 小節の頭: 前小節のヒット記録をリセット (ボール移動前)
    if (idx == 0) {
        std::fill(G.goalHitAtHead.begin(), G.goalHitAtHead.end(), false);
    }

    // ---- 球の移動 ----
    std::vector<Ball> newBalls;
    for (auto& b : G.balls) {
        if (!b.alive) continue;
        b.px = b.x; b.py = b.y;
        if (b.stopBeats > 0) { b.stopBeats--; continue; }       // 1拍停止
        if (b.oneTimeSlow) { b.oneTimeSlow = false; continue; } // 次の1拍だけスキップ
        if (b.oneTimeFast) {                                    // 1マス先をスキップして2マス先へジャンプ
            b.oneTimeFast = false;
            b.x += b.dx; b.y += b.dy;  // 1マス先: 座標のみ(壁・ギミック無視)
            b.x += b.dx; b.y += b.dy;  // 2マス先: 通常処理
            processCell(b, newBalls);
            continue;
        }
        if (b.slow) {                                           // 0.5倍速
            b.slowPhase = !b.slowPhase;
            if (!b.slowPhase) continue;
        }
        int steps = b.speed;                                    // 2倍速は1拍2マス
        for (int s = 0; s < steps && b.alive; s++) {
            b.x += b.dx; b.y += b.dy;
            if (!processCell(b, newBalls)) break;
        }
    }
    for (auto& nb : newBalls)
        if ((int)G.balls.size() < cfg.maxBalls) G.balls.push_back(nb);
    G.balls.erase(std::remove_if(G.balls.begin(), G.balls.end(),
                                 [](const Ball& b){ return !b.alive; }),
                  G.balls.end());

    // ---- 毎小節の頭に発射 (クリア後も鳴り続ける = 完成した曲がループ) ----
    if (idx == 0) {
        for (auto& s : stage().starts) {
            if (s.x >= cfg.gridW || s.y >= cfg.gridH) continue; // OOB
            if ((int)G.balls.size() < cfg.maxBalls) {
                Ball b;
                b.x = s.x; b.y = s.y; b.px = s.x; b.py = s.y;
                b.dx = s.dx; b.dy = s.dy;
                G.balls.push_back(b);
            }
        }
    }

    // ---- ゴール点灯とクリア判定 (小節の頭に全ゴール同時到達でクリア) ----
    bool allLit = false;
    bool hasInBoundsGoal = false;
    for (size_t i = 0; i < G.goalLit.size(); i++) {
        auto& g = stage().goals[i];
        if (g.x >= cfg.gridW || g.y >= cfg.gridH) continue; // OOB
        hasInBoundsGoal = true;
        if (G.goalLit[i] > 0) G.goalLit[i]--;
    }
    if (idx == 0 && hasInBoundsGoal) {
        allLit = true;
        for (size_t i = 0; i < G.goalLit.size(); i++) {
            auto& g = stage().goals[i];
            if (g.x >= cfg.gridW || g.y >= cfg.gridH) continue;
            if (!G.goalHitAtHead[i]) { allLit = false; break; }
        }
    }
    if (allLit && !G.cleared) {
        G.cleared = true;
        G.clearedStages[G.curStage] = true;
        saveProgress();
        for (int i = 0; i < 4; i++)
            addVoice(SND_CLEAR[i].voice, SND_CLEAR[i].freq, SND_CLEAR[i].amp);
        for (auto& g : stage().goals) {
            if (g.x < cfg.gridW && g.y < cfg.gridH)
                cellBurst(g.x, g.y, 255, 230, 120, 20, 260);
        }
        G.shake = 9;
    }

    // ---- 進行度に応じて音楽レイヤーを更新 (ビットマスク) ----
    bool ballAlive = std::any_of(G.balls.begin(), G.balls.end(), [](const Ball& b){ return b.alive; });
    int tilesPlaced = 0;
    for (int y = 0; y < cfg.gridH; y++)
        for (int x = 0; x < cfg.gridW; x++)
            if (G.grid[y][x] != T_EMPTY) tilesPlaced++;
    int layer = 0;
    if (ballAlive)        layer |= 1;   // ハット
    if (tilesPlaced >= 1) layer |= 2;   // ベース
    if (tilesPlaced >= 2) layer |= 4;   // コードパッド
    if (G.cleared)        layer |= 8;   // メロディ
    A.layer.store(layer);
    A.ballLayer.store((int)G.balls.size() >= 2 ? 1 : 0);
}

// ------------------------------------------------------------
// 入力 (ドラッグ&ドロップ)
// ------------------------------------------------------------
static SDL_Rect paletteRect(int i) {
    return SDL_Rect{ cfg.panelX + 12, 140 + 38 + i * 62, 56, 56 };
}

static bool cellAt(int mx, int my, int& cx, int& cy) {
    if (mx < cfg.gridX || my < cfg.gridY) return false;
    cx = (mx - cfg.gridX) / cfg.cell;
    cy = (my - cfg.gridY) / cfg.cell;
    return cx < cfg.gridW && cy < cfg.gridH;
}

static const char* tileName(TileType t) {
    switch (t) {
        case T_TURN_R:  return "TURN R";
        case T_TURN_L:  return "TURN L";
        case T_SPLIT:   return "SPLIT";
        case T_SPLIT3:  return "SPLIT 3";
        case T_SPEED2:  return "SPEED X2";
        case T_SLOW:    return "SLOW";
        case T_STOP:    return "STOP 1";
        case T_PAINT_R: return "PAINT RED";
        case T_PAINT_B: return "PAINT BLUE";
        case T_PAINT_Y: return "PAINT YEL";
        default:        return "";
    }
}

static void gameMouseDown(int mx, int my) {
    for (size_t i = 0; i < G.inv.size(); i++) {
        SDL_Rect rc = paletteRect((int)i);
        SDL_Point p{ mx, my };
        if (SDL_PointInRect(&p, &rc) && G.inv[i].second > 0) {
            G.inv[i].second--;
            G.dragging = true;
            G.dragType = G.inv[i].first;
            addVoice(SND_DRAG_START.voice, SND_DRAG_START.freq, SND_DRAG_START.amp);
            return;
        }
    }
    int cx, cy;
    if (cellAt(mx, my, cx, cy) && G.grid[cy][cx] != T_EMPTY) {
        G.dragType = G.grid[cy][cx];
        G.grid[cy][cx] = T_EMPTY;
        G.dragging = true;
        addVoice(SND_DRAG_START.voice, SND_DRAG_START.freq, SND_DRAG_START.amp);
    }
}

static void returnToInventory(TileType t) {
    for (auto& it : G.inv)
        if (it.first == t) { it.second++; return; }
}

static void gameMouseUp(int mx, int my) {
    if (!G.dragging) return;
    G.dragging = false;
    int cx, cy;
    if (cellAt(mx, my, cx, cy) && cellPlaceable(cx, cy)) {
        G.grid[cy][cx] = G.dragType;
        addVoice(SND_TILE_PLACE_OK.voice, SND_TILE_PLACE_OK.freq, SND_TILE_PLACE_OK.amp);
    } else {
        returnToInventory(G.dragType);
        addVoice(SND_TILE_PLACE_NG.voice, SND_TILE_PLACE_NG.freq, SND_TILE_PLACE_NG.amp);
    }
}

// ------------------------------------------------------------
// ステージセレクトのボタン配置
// ------------------------------------------------------------
static SDL_Rect selectRect(int i) {
    int col = i % 4, row = i / 4;
    int colW = (cfg.winW - 100) / 4;
    return SDL_Rect{ 50 + col * colW, 180 + row * 150, colW - 20, 120 };
}

// ------------------------------------------------------------
// 描画: ゲーム画面
// ------------------------------------------------------------
static void renderGame(SDL_Renderer* r, double nowSec, int mx, int my) {
    int ox = (int)(G.shake * sin(nowSec * 70.0));
    int oy = (int)(G.shake * cos(nowSec * 53.0));

    int bpmeas = stage().beatsPM;
    double beatMs = 60000.0 / stage().bpm;
    double sinceBeat = SDL_GetTicks() - G.lastBeatTick;
    double pulse = exp(-sinceBeat / beatMs * 6.0);
    char buf[80];

    // ---- ヘッダ ----
    SDL_SetRenderDrawColor(r, 235, 235, 240, 255);
    drawText(r, cfg.gridX + ox, 24 + oy, 3, stage().name);
    snprintf(buf, sizeof(buf), "STAGE %d   BPM %d   %d BEAT",
             G.curStage + 1, stage().bpm, bpmeas);
    SDL_SetRenderDrawColor(r, 150, 150, 165, 255);
    drawText(r, cfg.gridX + ox, 58 + oy, 2, buf);

    // ---- 音楽レベルメーター (右上) ----
    int meterX = cfg.winW - 190;
    SDL_SetRenderDrawColor(r, 150, 150, 165, 255);
    drawText(r, meterX, 24, 1, "MUSIC LV");
    int lay = A.layer.load();
    for (int i = 0; i < 5; i++) {
        SDL_Rect rc{ meterX + i * 22, 38, 16, 12 };
        bool lit = (i == 0) || (lay & (1 << (i - 1)));
        if (lit) SDL_SetRenderDrawColor(r, 120, 230, 160, 255);
        else     SDL_SetRenderDrawColor(r, 60, 60, 76, 255);
        SDL_RenderFillRect(r, &rc);
    }
    snprintf(buf, sizeof(buf), "VOL %d", A.vol100.load());
    SDL_SetRenderDrawColor(r, 110, 110, 128, 255);
    drawText(r, meterX, 58, 1, buf);
    {
        long _pb = G.processedBeats > 0 ? G.processedBeats - 1 : 0;
        int prog = (int)((_pb / bpmeas) % 4);
        for (int c = 0; c < 4; c++) {
            if (c == prog) SDL_SetRenderDrawColor(r, 235, 235, 240, 255);
            else           SDL_SetRenderDrawColor(r, 85, 85, 100, 255);
            drawText(r, meterX + c * 28, 72, 2, CHORD_NAME[c]);
        }
    }

    // ---- 拍子インジケータ ----
    int bcx = cfg.gridX + cfg.gridW * cfg.cell / 2 - (bpmeas - 1) * 22 + ox;
    int bcy = 96 + oy;
    for (int i = 0; i < bpmeas; i++) {
        int x = bcx + i * 44;
        if (i == G.lastBeatIdx) {
            SDL_SetRenderDrawColor(r, 220, 175, 75, 255);
            fillCircle(r, x, bcy, (int)(12 + 2 * pulse));
        } else {
            SDL_SetRenderDrawColor(r, 120, 120, 135, 255);
            drawCircle(r, x, bcy, 12);
        }
    }
    // ---- 市松盤面 (拍でほんのり明滅) ----
    int bright = (int)(3 * pulse);
    for (int y = 0; y < cfg.gridH; y++)
        for (int x = 0; x < cfg.gridW; x++) {
            SDL_Rect rc{ cfg.gridX + x * cfg.cell + ox, cfg.gridY + y * cfg.cell + oy, cfg.cell, cfg.cell };
            if ((x + y) % 2 == 0)
                SDL_SetRenderDrawColor(r, (Uint8)(190+bright), (Uint8)(190+bright), (Uint8)(202+bright), 255);
            else
                SDL_SetRenderDrawColor(r, (Uint8)(158+bright), (Uint8)(158+bright), (Uint8)(172+bright), 255);
            SDL_RenderFillRect(r, &rc);
        }
    SDL_SetRenderDrawColor(r, 70, 70, 85, 255);
    SDL_Rect frame{ cfg.gridX - 2 + ox, cfg.gridY - 2 + oy,
                    cfg.gridW * cfg.cell + 4, cfg.gridH * cfg.cell + 4 };
    SDL_RenderDrawRect(r, &frame);

    // ---- スタート ----
    for (auto& s : stage().starts) {
        if (s.x >= cfg.gridW || s.y >= cfg.gridH) continue;
        SDL_Rect rc{ cfg.gridX + s.x * cfg.cell + 4 + ox, cfg.gridY + s.y * cfg.cell + 4 + oy,
                     cfg.cell - 8, cfg.cell - 8 };
        SDL_SetRenderDrawColor(r, 70, 175, 95, 255);
        SDL_RenderFillRect(r, &rc);
        SDL_SetRenderDrawColor(r, 245, 250, 245, 255);
        int tx = rc.x + rc.w / 4, ty = rc.y + rc.h / 2, tw = rc.w / 2, th = rc.h / 3;
        for (int i = 0; i < tw; i++) {
            int h = th - i * th / tw;
            SDL_RenderDrawLine(r, tx + i, ty - h, tx + i, ty + h);
        }
    }

    // ---- ゴール (色付き対応) ----
    for (size_t i = 0; i < stage().goals.size(); i++) {
        auto& g = stage().goals[i];
        if (g.x >= cfg.gridW || g.y >= cfg.gridH) continue;
        SDL_Rect rc{ cfg.gridX + g.x * cfg.cell + 4 + ox, cfg.gridY + g.y * cfg.cell + 4 + oy,
                     cfg.cell - 8, cfg.cell - 8 };
        Uint8 cr = 215, cg = 180, cb = 80;
        if (g.color >= 0) { cr = BALLCOL[g.color].r; cg = BALLCOL[g.color].g; cb = BALLCOL[g.color].b; }
        if (G.goalLit[i] > 0) {
            SDL_SetRenderDrawColor(r, cr, cg, cb, 255);
            SDL_RenderFillRect(r, &rc);
            SDL_SetRenderDrawColor(r, 255, 250, 230, 255);
        } else {
            SDL_SetRenderDrawColor(r, (Uint8)(cr/3), (Uint8)(cg/3), (Uint8)(cb/3), 255);
            SDL_RenderFillRect(r, &rc);
            SDL_SetRenderDrawColor(r, cr, cg, cb, 255);
        }
        SDL_RenderDrawRect(r, &rc);
        SDL_Rect rc2{ rc.x + 6, rc.y + 6, rc.w - 12, rc.h - 12 };
        SDL_RenderDrawRect(r, &rc2);
        if (g.color >= 0) fillCircle(r, rc.x + rc.w / 2, rc.y + rc.h / 2, 8);
    }

    // ---- ワープ (回転する渦) ----
    for (auto& w : stage().warps) {
        int pts[2][2] = {{w.x1, w.y1}, {w.x2, w.y2}};
        for (auto& p : pts) {
            if (p[0] >= cfg.gridW || p[1] >= cfg.gridH) continue;
            int cx = cfg.gridX + p[0] * cfg.cell + cfg.cell / 2 + ox;
            int cy = cfg.gridY + p[1] * cfg.cell + cfg.cell / 2 + oy;
            SDL_SetRenderDrawColor(r, 110, 230, 230, 255);
            double rot = nowSec * 3.0;
            drawArc(r, cx, cy, 22, rot, rot + 2.2);
            drawArc(r, cx, cy, 22, rot + M_PI, rot + M_PI + 2.2);
            drawArc(r, cx, cy, 13, -rot, -rot + 2.4);
            SDL_SetRenderDrawColor(r, 110, 230, 230, 120);
            drawCircle(r, cx, cy, 28);
        }
    }

    // ---- 配置済みギミック ----
    for (int y = 0; y < cfg.gridH; y++)
        for (int x = 0; x < cfg.gridW; x++)
            if (G.grid[y][x] != T_EMPTY)
                drawTileIcon(r, G.grid[y][x],
                             cfg.gridX + x * cfg.cell + ox,
                             cfg.gridY + y * cfg.cell + oy,
                             cfg.cell, 255);

    // ---- ギミック発動フラッシュ ----
    for (auto& f : G.flashes) {
        Uint8 a = (Uint8)std::max(0.0, 160.0 * (1.0 - f.t / 0.35));
        SDL_SetRenderDrawColor(r, 255, 255, 255, a);
        SDL_Rect rc{ cfg.gridX + f.x * cfg.cell + ox, cfg.gridY + f.y * cfg.cell + oy, cfg.cell, cfg.cell };
        SDL_RenderFillRect(r, &rc);
    }

    // ---- ドラッグ先ハイライト ----
    if (G.dragging) {
        int cx, cy;
        if (cellAt(mx, my, cx, cy) && cellPlaceable(cx, cy)) {
            SDL_SetRenderDrawColor(r, 120, 220, 160, 90);
            SDL_Rect rc{ cfg.gridX + cx * cfg.cell + ox, cfg.gridY + cy * cfg.cell + oy, cfg.cell, cfg.cell };
            SDL_RenderFillRect(r, &rc);
        }
    }

    // ---- パーティクル ----
    for (auto& p : G.parts) {
        Uint8 a = (Uint8)(220 * (p.life / p.max));
        SDL_SetRenderDrawColor(r, p.r, p.g, p.b, a);
        SDL_Rect rc{ (int)p.x - 2 + ox, (int)p.y - 2 + oy, 4, 4 };
        SDL_RenderFillRect(r, &rc);
    }

    // ---- 球 (補間 + 残像) ----
    double frac = std::min(1.0, sinceBeat / (beatMs * 0.4));
    double ease = 1.0 - (1.0 - frac) * (1.0 - frac);
    for (auto& b : G.balls) {
        double fx = b.px + (b.x - b.px) * ease;
        double fy = b.py + (b.y - b.py) * ease;
        float bx = (float)(cfg.gridX + fx * cfg.cell + cfg.cell / 2) + ox;
        float by = (float)(cfg.gridY + fy * cfg.cell + cfg.cell / 2) + oy;

        b.hx[b.hi] = bx; b.hy[b.hi] = by;             // 残像履歴を更新
        b.hi = (b.hi + 1) % 10;
        if (b.hn < 10) b.hn++;

        SDL_Color c = (b.color >= 0) ? BALLCOL[b.color] : SDL_Color{250, 250, 255, 255};
        for (int k = 2; k <= 8 && k < b.hn; k += 2) {  // 残像
            int hidx = (b.hi - 1 - k + 20) % 10;
            SDL_SetRenderDrawColor(r, c.r, c.g, c.b, (Uint8)(70 - k * 8));
            fillCircle(r, (int)b.hx[hidx], (int)b.hy[hidx], 11 - k);
        }
        SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 50);  // グロー
        fillCircle(r, (int)bx, (int)by, 19);
        SDL_SetRenderDrawColor(r, 40, 40, 55, 255);
        fillCircle(r, (int)bx, (int)by, 15);
        SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255);
        fillCircle(r, (int)bx, (int)by, 12);
    }

    // ---- 右: ギミックパネル置き場 ----
    SDL_SetRenderDrawColor(r, 40, 40, 54, 255);
    SDL_Rect pal{ cfg.panelX, 140, 196, cfg.winH - 140 - 30 };
    SDL_RenderFillRect(r, &pal);
    SDL_SetRenderDrawColor(r, 90, 90, 110, 255);
    SDL_RenderDrawRect(r, &pal);
    SDL_SetRenderDrawColor(r, 220, 220, 230, 255);
    drawText(r, cfg.panelX + 12, 140 + 14, 2, "TILES");
    for (size_t i = 0; i < G.inv.size(); i++) {
        SDL_Rect rc = paletteRect((int)i);
        SDL_SetRenderDrawColor(r, 58, 58, 76, 255);
        SDL_RenderFillRect(r, &rc);
        Uint8 a = G.inv[i].second > 0 ? 255 : 70;
        drawTileIcon(r, G.inv[i].first, rc.x, rc.y, 56, a);
        snprintf(buf, sizeof(buf), "X%d", G.inv[i].second);
        SDL_SetRenderDrawColor(r, 230, 230, 240, a);
        drawText(r, rc.x + 64, rc.y + 12, 2, buf);
        SDL_SetRenderDrawColor(r, 160, 160, 178, a);
        drawText(r, rc.x + 64, rc.y + 34, 1, tileName(G.inv[i].first));
    }

    // ---- ドラッグ中のタイル ----
    if (G.dragging)
        drawTileIcon(r, G.dragType, mx - cfg.cell / 2, my - cfg.cell / 2, cfg.cell, 220);

    // ---- クリア表示 ----
    if (G.cleared) {
        SDL_SetRenderDrawColor(r, 20, 20, 28, 140);
        SDL_Rect ov{ cfg.gridX, cfg.gridY, cfg.gridW * cfg.cell, cfg.gridH * cfg.cell };
        SDL_RenderFillRect(r, &ov);
        const char* msg = "CLEAR!";
        int s = 7;
        SDL_SetRenderDrawColor(r, 255, 220, 90, 255);
        drawText(r, cfg.gridX + (cfg.gridW * cfg.cell - textWidth(s, msg)) / 2 + ox,
                 cfg.gridY + cfg.gridH * cfg.cell / 2 - 64 + oy, s, msg);
        const char* msg2 = (G.curStage + 1 < (int)G.stages.size())
                               ? "N: NEXT STAGE   M: STAGE SELECT"
                               : "ALL STAGES DONE!  M: STAGE SELECT";
        SDL_SetRenderDrawColor(r, 235, 235, 240, 255);
        drawText(r, cfg.gridX + (cfg.gridW * cfg.cell - textWidth(2, msg2)) / 2,
                 cfg.gridY + cfg.gridH * cfg.cell / 2 + 28, 2, msg2);
    }

    // ---- 下部ヘルプ ----
    int helpY1 = cfg.gridY + cfg.gridH * cfg.cell + 12;
    int helpY2 = cfg.gridY + cfg.gridH * cfg.cell + 38;
    SDL_SetRenderDrawColor(r, 140, 140, 158, 255);
    drawText(r, cfg.gridX, helpY1, 2, "DRAG TILES ONTO THE BOARD");
    drawText(r, cfg.gridX, helpY2, 2, "R: RESET   M: SELECT   -/+: VOLUME");
    if (!g_audioOK) {
        SDL_SetRenderDrawColor(r, 220, 120, 120, 255);
        drawText(r, cfg.gridX + cfg.gridW * cfg.cell / 2, helpY2, 2, "NO AUDIO - SILENT");
    }
}

// ------------------------------------------------------------
// 描画: タイトル
// ------------------------------------------------------------
static void renderTitle(SDL_Renderer* r, double nowSec) {
    double beatMs = 60000.0 / A.bpm.load();
    double sinceBeat = SDL_GetTicks() - G.lastBeatTick;
    double pulse = exp(-sinceBeat / beatMs * 6.0);

    const char* title = "BEATPATH";
    int s = 9;
    int tx = (cfg.winW - textWidth(s, title)) / 2;
    SDL_SetRenderDrawColor(r, 60, 55, 90, 255);
    drawText(r, tx + 4, 154, s, title);
    SDL_SetRenderDrawColor(r, (Uint8)(235 + 20 * pulse > 255 ? 255 : 235 + 20 * pulse), 220, 130, 255);
    drawText(r, tx, 150, s, title);

    const char* sub = "RHYTHM TILE PUZZLE";
    SDL_SetRenderDrawColor(r, 160, 160, 180, 255);
    drawText(r, (cfg.winW - textWidth(2, sub)) / 2, 250, 2, sub);

    for (int i = 0; i < 4; i++) {
        int x = cfg.winW / 2 - 66 + i * 44;
        if (i == G.lastBeatIdx) {
            SDL_SetRenderDrawColor(r, 220, 175, 75, 255);
            fillCircle(r, x, 330, (int)(12 + 2 * pulse));
        } else {
            SDL_SetRenderDrawColor(r, 110, 110, 128, 255);
            drawCircle(r, x, 330, 11);
        }
    }

    double blink = 0.5 + 0.5 * sin(nowSec * 3.5);
    SDL_SetRenderDrawColor(r, 235, 235, 240, (Uint8)(120 + 135 * blink));
    const char* prompt = "CLICK TO START";
    drawText(r, (cfg.winW - textWidth(3, prompt)) / 2, 420, 3, prompt);

    SDL_SetRenderDrawColor(r, 100, 100, 118, 255);
    const char* foot = "PLACE TILES - GUIDE THE BALLS - MAKE MUSIC";
    drawText(r, (cfg.winW - textWidth(2, foot)) / 2, 500, 2, foot);
}

// ------------------------------------------------------------
// 描画: ステージセレクト
// ------------------------------------------------------------
static void renderSelect(SDL_Renderer* r, double nowSec, int mx, int my) {
    (void)nowSec;
    SDL_SetRenderDrawColor(r, 235, 235, 240, 255);
    const char* h = "SELECT STAGE";
    drawText(r, (cfg.winW - textWidth(4, h)) / 2, 60, 4, h);

    char buf[64];
    for (size_t i = 0; i < G.stages.size(); i++) {
        SDL_Rect rc = selectRect((int)i);
        SDL_Point p{ mx, my };
        bool hover = SDL_PointInRect(&p, &rc);
        bool unlocked = stageUnlocked((int)i);

        if (unlocked) SDL_SetRenderDrawColor(r, hover ? 62 : 48, hover ? 62 : 48, hover ? 84 : 66, 255);
        else          SDL_SetRenderDrawColor(r, 34, 34, 44, 255);
        SDL_RenderFillRect(r, &rc);
        SDL_SetRenderDrawColor(r, unlocked ? 130 : 70, unlocked ? 130 : 70, unlocked ? 155 : 85, 255);
        SDL_RenderDrawRect(r, &rc);

        snprintf(buf, sizeof(buf), "%d", (int)i + 1);
        SDL_SetRenderDrawColor(r, unlocked ? 250 : 110, unlocked ? 200 : 110, unlocked ? 90 : 120, 255);
        drawText(r, rc.x + 14, rc.y + 12, 4, buf);

        if (unlocked) {
            SDL_SetRenderDrawColor(r, 225, 225, 235, 255);
            drawText(r, rc.x + 14, rc.y + 54, 2, G.stages[i].name);
            snprintf(buf, sizeof(buf), "%d BEAT  BPM %d", G.stages[i].beatsPM, G.stages[i].bpm);
            SDL_SetRenderDrawColor(r, 140, 140, 158, 255);
            drawText(r, rc.x + 14, rc.y + 82, 1, buf);
            if (G.clearedStages[i]) {
                SDL_SetRenderDrawColor(r, 255, 215, 80, 255);
                fillCircle(r, rc.x + rc.w - 24, rc.y + 24, 9);
                SDL_SetRenderDrawColor(r, 120, 90, 20, 255);
                drawText(r, rc.x + rc.w - 60, rc.y + 40, 1, "CLEAR");
            }
        } else {
            SDL_SetRenderDrawColor(r, 110, 110, 125, 255);
            drawText(r, rc.x + 14, rc.y + 60, 2, "LOCKED");
        }
    }

    SDL_SetRenderDrawColor(r, 130, 130, 148, 255);
    const char* foot = "ESC: TITLE   -/+: VOLUME";
    drawText(r, (cfg.winW - textWidth(2, foot)) / 2, 530, 2, foot);
}

// ------------------------------------------------------------
// 背景 (拍で脈動する縦グラデーション)
// ------------------------------------------------------------
static void renderBackground(SDL_Renderer* r, double nowSec) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    double beatMs = 60000.0 / A.bpm.load();
    double sinceBeat = SDL_GetTicks() - G.lastBeatTick;
    double pulse = exp(-sinceBeat / beatMs * 6.0);
    for (int i = 0; i < 8; i++) {
        double w = sin(nowSec * 0.4 + i * 0.9) * 0.5 + 0.5;
        Uint8 base = (Uint8)(20 + i * 1.3 + 5 * w + 4 * pulse);
        SDL_SetRenderDrawColor(r, base, base, (Uint8)(base + 12), 255);
        SDL_Rect rc{ 0, i * cfg.winH / 8, cfg.winW, cfg.winH / 8 + 1 };
        SDL_RenderFillRect(r, &rc);
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
}

// ------------------------------------------------------------
// 設定ファイル (beatpath.cfg) の読み書き
// ------------------------------------------------------------
static void saveDefaultConfig() {
    FILE* f = fopen("beatpath.cfg", "w");
    if (!f) return;
    fprintf(f,
        "# BeatPath DX 設定ファイル\n"
        "# 変更後はゲームを再起動してください\n"
        "\n"
        "# 1マスの最大ピクセルサイズ in px (グリッドサイズに合わせて自動調整)\n"
        "CELL=120\n"
        "# 初期音量 (0-100)\n"
        "VOLUME=80\n"
        "# タイトル/セレクト画面のBPM\n"
        "MENU_BPM=112\n"
        "# 同時存在できる球の最大数\n"
        "MAX_BALLS=64\n"
    );
    fclose(f);
}

static void loadConfig() {
    FILE* f = fopen("beatpath.cfg", "r");
    if (!f) {
        saveDefaultConfig();
        cfg.computeDerived();
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;
        char key[64] = {}, val[64] = {};
        if (sscanf(p, "%63[^=\n]=%63s", key, val) == 2) {
            // キーの末尾スペースをトリム
            char* e = key + strlen(key) - 1;
            while (e > key && (*e == ' ' || *e == '\t')) *e-- = '\0';
            int v = atoi(val);
            if      (strcmp(key, "CELL")      == 0) cfg.maxCell  = std::max(30, std::min(150, v));
            else if (strcmp(key, "VOLUME")    == 0) cfg.volume   = std::max(0, std::min(100, v));
            else if (strcmp(key, "MENU_BPM")  == 0) cfg.menuBpm  = std::max(60, std::min(240, v));
            else if (strcmp(key, "MAX_BALLS") == 0) cfg.maxBalls = std::max(8, std::min(256, v));
        }
    }
    fclose(f);
    cfg.computeDerived();
}

// ------------------------------------------------------------
// main
// ------------------------------------------------------------
int main(int, char**) {
    loadConfig();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return 1;
        }
    }

    SDL_Window* win = SDL_CreateWindow("BeatPath DX - Rhythm Tile Puzzle",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       cfg.winW, cfg.winH, SDL_WINDOW_SHOWN);
    if (!win) { fprintf(stderr, "CreateWindow failed: %s\n", SDL_GetError()); return 1; }

    SDL_SetWindowOpacity(win, 1.0f);

    SDL_Renderer* ren = SDL_CreateRenderer(win, -1,
                          SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) { fprintf(stderr, "CreateRenderer failed: %s\n", SDL_GetError()); return 1; }

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = 48000;
    want.format = AUDIO_F32SYS;
    want.channels = 1;
    want.samples = 512;
    want.callback = audioCallback;
    g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (g_dev) {
        A.sampleRate = have.freq;
        g_audioOK = true;
        SDL_PauseAudioDevice(g_dev, 0);
    } else {
        fprintf(stderr, "Audio unavailable (%s) - running silent.\n", SDL_GetError());
    }

    A.vol100.store(cfg.volume);

    loadProgress();
    enterMenuMusic();

    bool running = true;
    Uint32 prevTicks = SDL_GetTicks();

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { running = false; continue; }

            if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_MINUS)
                    A.vol100.store(std::max(0, A.vol100.load() - 10));
                else if (k == SDLK_EQUALS || k == SDLK_PLUS)
                    A.vol100.store(std::min(100, A.vol100.load() + 10));

                if (G.scene == SC_TITLE) {
                    if (k == SDLK_ESCAPE) running = false;
                    else if (k == SDLK_RETURN || k == SDLK_SPACE) { G.scene = SC_SELECT; addVoice(SND_UI_ENTER.voice, SND_UI_ENTER.freq, SND_UI_ENTER.amp); }
                } else if (G.scene == SC_SELECT) {
                    if (k == SDLK_ESCAPE) G.scene = SC_TITLE;
                } else { // SC_GAME
                    if (k == SDLK_ESCAPE || k == SDLK_m) {
                        G.scene = SC_SELECT;
                        enterMenuMusic();
                    } else if (k == SDLK_r) {
                        loadStage(G.curStage);
                    } else if (k == SDLK_n && G.cleared) {
                        if (G.curStage + 1 < (int)G.stages.size())
                            loadStage(G.curStage + 1);
                        else { G.scene = SC_SELECT; enterMenuMusic(); }
                    }
                }
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                int mx = e.button.x, my = e.button.y;
                if (G.scene == SC_TITLE) {
                    G.scene = SC_SELECT;
                    addVoice(SND_UI_ENTER.voice, SND_UI_ENTER.freq, SND_UI_ENTER.amp);
                } else if (G.scene == SC_SELECT) {
                    for (size_t i = 0; i < G.stages.size(); i++) {
                        SDL_Rect rc = selectRect((int)i);
                        SDL_Point p{ mx, my };
                        if (SDL_PointInRect(&p, &rc)) {
                            if (stageUnlocked((int)i)) {
                                addVoice(SND_UI_SELECT_OK.voice, SND_UI_SELECT_OK.freq, SND_UI_SELECT_OK.amp);
                                loadStage((int)i);
                            } else {
                                addVoice(SND_UI_SELECT_LOCK.voice, SND_UI_SELECT_LOCK.freq, SND_UI_SELECT_LOCK.amp);
                            }
                            break;
                        }
                    }
                } else {
                    gameMouseDown(mx, my);
                }
            } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
                if (G.scene == SC_GAME) gameMouseUp(e.button.x, e.button.y);
            }
        }

        Uint32 now = SDL_GetTicks();
        double dt = (now - prevTicks) / 1000.0;
        prevTicks = now;
        double nowSec = now / 1000.0;

        // オーディオが無い環境では時間で拍を刻む
        if (!g_audioOK) {
            g_fallbackAccum += dt * 1000.0;
            double beatMs = 60000.0 / A.bpm.load();
            while (g_fallbackAccum >= beatMs) {
                g_fallbackAccum -= beatMs;
                g_fallbackBeats++;
            }
        }

        long tb = g_audioOK ? A.totalBeats.load() : g_fallbackBeats;
        while (G.processedBeats < tb) {
            if (G.scene == SC_GAME) {
                onBeat();
            } else {
                G.lastBeatIdx = (int)(G.processedBeats % A.beatsPM.load());
                G.lastBeatTick = SDL_GetTicks();
                G.shake = 1.5;
            }
            G.processedBeats++;
        }

        // ---- エフェクト更新 ----
        for (auto& f : G.flashes) f.t += dt;
        G.flashes.erase(std::remove_if(G.flashes.begin(), G.flashes.end(),
                                       [](const Flash& f){ return f.t > 0.35; }),
                        G.flashes.end());
        for (auto& p : G.parts) {
            p.x += p.vx * (float)dt;
            p.y += p.vy * (float)dt;
            p.vy += 320.0f * (float)dt;
            p.life -= (float)dt;
        }
        G.parts.erase(std::remove_if(G.parts.begin(), G.parts.end(),
                                     [](const Particle& p){ return p.life <= 0; }),
                      G.parts.end());
        G.shake *= pow(0.001, dt);

        // ---- 描画 ----
        int mx, my;
        SDL_GetMouseState(&mx, &my);
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(ren, 20, 20, 28, 255);
        SDL_RenderClear(ren);
        renderBackground(ren, nowSec);
        if (G.scene == SC_TITLE)       renderTitle(ren, nowSec);
        else if (G.scene == SC_SELECT) renderSelect(ren, nowSec, mx, my);
        else                           renderGame(ren, nowSec, mx, my);
        SDL_RenderPresent(ren);
        SDL_Delay(1);
    }

    if (g_dev) SDL_CloseAudioDevice(g_dev);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
