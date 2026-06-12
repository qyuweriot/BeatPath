#pragma once
#include "stages.h"

// ------------------------------------------------------------
// 音名定数 (等音律, A4 = 440 Hz)
// ------------------------------------------------------------
static constexpr double C2  =   65.406;
static constexpr double D2  =   73.416;
static constexpr double E2  =   82.407;
static constexpr double F2  =   87.307;
static constexpr double G2  =   97.999;
static constexpr double A2  =  110.000;
static constexpr double B2  =  123.471;
static constexpr double C3  =  130.813;
static constexpr double D3  =  146.832;
static constexpr double E3  =  164.814;
static constexpr double F3  =  174.614;
static constexpr double G3  =  195.998;
static constexpr double A3  =  220.000;
static constexpr double B3  =  246.942;
static constexpr double C4  =  261.626;
static constexpr double D4  =  293.665;
static constexpr double E4  =  329.628;
static constexpr double F4  =  349.228;
static constexpr double G4  =  391.995;
static constexpr double A4  =  440.000;
static constexpr double B4  =  493.883;
static constexpr double C5  =  523.251;
static constexpr double D5  =  587.330;
static constexpr double E5  =  659.255;
static constexpr double F5  =  698.456;
static constexpr double G5  =  783.991;
static constexpr double A5  =  880.000;
static constexpr double B5  =  987.767;
static constexpr double C6  = 1046.502;
static constexpr double D6  = 1174.659;
static constexpr double E6  = 1318.510;
static constexpr double F6  = 1396.913;
static constexpr double G6  = 1567.982;
static constexpr double A6  = 1760.000;
static constexpr double B6  = 1975.533;
static constexpr double C7  = 2093.005;

// SE 用非音名周波数
static constexpr double SFX_BALL_OUT    = 180.0;   // 場外クリック (F#3 近辺)
static constexpr double SFX_SWEEP_START = 300.0;   // ワープ スウィープ開始
static constexpr double SFX_PLACE_NG    = 500.0;   // 配置失敗 (B4-C5 間)
static constexpr double SFX_PLACE_OK    = 850.0;   // 配置成功 (G#5-A5 間)
static constexpr double SFX_DRAG        = 1400.0;  // ドラッグ開始 (F6 近辺)

// ------------------------------------------------------------
// ボイス種別
// ------------------------------------------------------------
enum VoiceType {
    V_KICK, V_PLUCK, V_HAT, V_BASS, V_BELL, V_CLICK, V_SNARE, V_SWEEP, V_PAD,
    V_MARIMBA, V_TOM, V_ORGAN, V_RIDE,
    V_MARIMBA2, V_MARIMBA3
};

// ------------------------------------------------------------
// BGM: コード進行 Am -> F -> C -> G (4小節ループ)
// ------------------------------------------------------------
static const char*  CHORD_NAME[4]   = {"AM", "F", "C", "G"};

static const double BASS_ROOT[4]    = { A2,  F2,  C2,  G2 };
static const double BASS_FIFTH[4]   = { E2,  C2,  G2,  D2 };

static const double PAD_CH[4][3] = {
    { A3, C4, E4 },   // Am
    { F3, A3, C4 },   // F
    { C3, G3, E4 },   // C
    { G3, B3, D4 },   // G
};
static const double MELODY[4][4] = {
    { A4, C5, E5, C5 },   // Am
    { F4, A4, C5, A4 },   // F
    { C5, E5, G5, E5 },   // C
    { G4, B4, D5, B4 },   // G
};

// ------------------------------------------------------------
// BGM ミックス振幅
// ------------------------------------------------------------
static constexpr float KICK_AMP_DOWN  = 1.2f;   // 小節頭キック
static constexpr float KICK_AMP_WEAK  = 0.8f;   // その他の拍
static constexpr float CRASH_AMP      = 0.7f;   // 小節頭シンバル
static constexpr float BASS_AMP_ROOT  = 2.0f;
static constexpr float BASS_AMP_FIFTH = 1.5f;
static constexpr float PAD_AMP        = 1.5f;
static constexpr float MELODY_AMP     = 0.55f;
static constexpr float HAT_AMP        = 0.9f;
static constexpr float SHAKER_AMP     = 0.35f;

// ------------------------------------------------------------
// ギミックタイル サウンド表  (TileType でインデックス)
// wave パラメータ (V_PLUCK のみ有効) は colorWave(b.color) で実行時決定
// ------------------------------------------------------------
struct TileSound { VoiceType voice; double freq; float amp; };

static const TileSound TILE_SOUNDS[T_COUNT] = {
    /* T_EMPTY   */ {},
    /* T_TURN_R  */ { V_SNARE,    C4, 0.8f },
    /* T_TURN_L  */ { V_SNARE,    E4, 0.8f },
    /* T_SPLIT   */ { V_MARIMBA2, E5, 1.0f },
    /* T_SPLIT3  */ { V_MARIMBA3, G5, 1.0f },
    /* T_SPEED2  */ { V_TOM,      G3, 0.9f },
    /* T_STOP    */ { V_TOM,      G3, 0.7f },
    /* T_PAINT_R */ { V_ORGAN,    C5, 0.8f },
    /* T_PAINT_B */ { V_ORGAN,    E5, 0.8f },
    /* T_PAINT_Y */ { V_ORGAN,    G5, 0.8f },
};

// ------------------------------------------------------------
// ゲームイベント サウンド定数
// ------------------------------------------------------------
struct Sound { VoiceType voice; double freq; float amp; };

// 球イベント
static const Sound SND_BALL_OUT       = { V_TOM,   SFX_BALL_OUT,    0.4f };
static const Sound SND_WARP           = { V_SWEEP, SFX_SWEEP_START, 1.2f };
static const Sound SND_GOAL_WRONG     = { V_CLICK,           C3,    0.5f };

// ゴール到達 (gi % 4 でインデックス)
static const double GOAL_FREQ[4]  = { C6, E6, G5, A5 };
static constexpr float GOAL_AMP   = 1.0f;

// クリア演出 (4音ファンファーレ)
static const Sound SND_CLEAR[4] = {
    { V_BELL, C5, 0.65f },
    { V_BELL, E5, 0.55f },
    { V_BELL, G5, 0.55f },
    { V_BELL, C6, 0.45f },
};

// UI 操作
static const Sound SND_DRAG_START      = { V_CLICK, SFX_DRAG,    0.6f };
static const Sound SND_TILE_PLACE_OK   = { V_CLICK, SFX_PLACE_OK, 0.8f };
static const Sound SND_TILE_PLACE_NG   = { V_CLICK, SFX_PLACE_NG, 0.5f };
static const Sound SND_UI_ENTER        = { V_BELL,  E5,  0.8f };
static const Sound SND_UI_SELECT_OK    = { V_BELL,  G5,  0.8f };
static const Sound SND_UI_SELECT_LOCK  = { V_CLICK, C3,  0.6f };
