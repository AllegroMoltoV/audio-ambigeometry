#pragma once

// 図形の頂点を CPU で作る。OBS に依存しないため、ユニットテストで確かめられる。
// 頂点は 2 つで 1 本の線分を表す (GS_LINES)。座標は出力の左上を原点とする画素で、y は下向き

#include "audio-analyzer.hpp"

#include <cstddef>
#include <cstdint>

namespace ambigeometry {

enum class Pattern : int {
	Polygons = 0,
	Ring = 1,
	Lissajous = 2,
};

struct GeometrySettings {
	Pattern pattern = Pattern::Polygons;
	// 漂う多角形の数
	int shape_count = 6;
	// 中心のまわりに等角で複製する数。1 で複製しない
	int symmetry = 1;
	// 色相が 1 周する回数 (回/分)
	float hue_cycles_per_minute = 6.0f;
	// 彩度 (0〜1)
	float saturation = 0.8f;
};

struct Vertex {
	float x = 0.0f;
	float y = 0.0f;
	// 0xAABBGGRR。OBS の頂点の色と同じ並び
	uint32_t color = 0;
};

constexpr int kMinShapeCount = 1;
constexpr int kMaxShapeCount = 12;
constexpr int kMinSymmetry = 1;
constexpr int kMaxSymmetry = 8;
// 1 フレームの頂点の上限。どのパターンも、最大の設定でこれを超えないように点の数を決めている
constexpr size_t kMaxVertices = 2048;

// out には kMaxVertices 個の領域を渡す。書いた頂点の数を返す
size_t build_geometry(const GeometrySettings &settings, const Features &features, const StereoSample *wave,
		      size_t wave_count, double time, float width, float height, Vertex *out);

} // namespace ambigeometry
