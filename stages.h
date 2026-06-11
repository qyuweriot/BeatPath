#pragma once
#include <utility>
#include <vector>

// ------------------------------------------------------------
// タイル定数
// ------------------------------------------------------------
enum TileType {
    T_EMPTY = 0,
    T_TURN_R, T_TURN_L,      // 方向転換
    T_SPLIT, T_SPLIT3,       // 分裂
    T_SPEED2, T_SLOW,        // 速度変化
    T_STOP,                  // 1拍停止
    T_PAINT_R, T_PAINT_B, T_PAINT_Y, // 色変更 (音色も変わる)
    T_COUNT
};

// ------------------------------------------------------------
// ステージ定義型
// ------------------------------------------------------------
struct StartDef { int x, y, dx, dy; };
struct GoalDef  { int x, y; int color; };       // color -1 = 何色でもOK
struct WallDef  { int x, y; };
struct WarpDef  { int x1, y1, x2, y2; };        // 双方向ワープ

struct StageDef {
    const char* name;
    int bpm, beatsPM;
    std::vector<StartDef> starts;
    std::vector<GoalDef>  goals;
    std::vector<WallDef>  walls;
    std::vector<WarpDef>  warps;
    std::vector<std::pair<TileType, int>> inv;
};

// ------------------------------------------------------------
// ビルトインステージ (8ステージ)
// 新しいステージを追加する場合はこのリストに追記してください
// ------------------------------------------------------------
inline std::vector<StageDef> makeStages() {
    return {
        // 1: チュートリアル - 曲がる
        { "FIRST STEPS", 112, 4,
          {{0,2,1,0}}, {{6,0,-1}}, {}, {},
          {{T_TURN_L,1},{T_TURN_R,1}} },
        // 2: 分裂で2つのゴールへ同時到達
        { "SPLIT", 116, 6,
          {{0,2,1,0}}, {{6,1,-1},{6,3,-1}}, {}, {},
          {{T_SPLIT,1},{T_TURN_R,1},{T_TURN_L,1}} },
        // 3: 三分裂で3ゴール
        { "TRIPLE", 120, 8,
          {{0,2,1,0}}, {{6,0,-1},{6,2,-1},{6,4,-1}}, {}, {},
          {{T_SPLIT3,1},{T_TURN_R,1},{T_TURN_L,1}} },
        // 4: 壁を球をぶつけて破壊 (壁ヒット = スネア)
        { "BREAK", 118, 4,
          {{0,2,1,0}}, {{6,2,-1}},
          {{4,1},{4,2},{4,3}}, {},
          {{T_SPLIT3,1},{T_TURN_R,1},{T_TURN_L,1}} },
        // 5: ワープ (入口と出口のセット)
        { "WARP", 122, 6,
          {{0,2,1,0}}, {{6,4,-1}},
          {}, {{2,2, 4,0}},
          {{T_TURN_R,1},{T_TURN_L,1}} },
        // 6: 色ギミック - ペイントして同色ゴールへ
        { "COLORS", 124, 6,
          {{0,2,1,0}}, {{6,1,0},{6,3,1}}, {}, {},
          {{T_SPLIT,1},{T_TURN_R,1},{T_TURN_L,1},{T_PAINT_R,1},{T_PAINT_B,1}} },
        // 7: 複数スタート + 3ゴール
        { "DUET", 126, 8,
          {{0,1,1,0},{0,3,1,0}}, {{6,0,-1},{6,2,-1},{6,4,-1}}, {}, {},
          {{T_SPLIT,1},{T_TURN_R,2},{T_TURN_L,2}} },
        // 8: 総決算 - 壁 + ワープ + 色 + 速度系のおまけ付き
        { "FINALE", 128, 8,
          {{0,2,1,0}}, {{6,1,0},{6,3,1}},
          {{4,2},{4,3}}, {{5,0, 1,4}},
          {{T_SPLIT,1},{T_TURN_R,2},{T_TURN_L,1},
           {T_PAINT_R,1},{T_PAINT_B,1},{T_SPEED2,1},{T_STOP,1}} },
    };
}
