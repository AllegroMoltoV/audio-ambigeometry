#include "geometry.hpp"

#include <algorithm>
#include <cmath>

namespace ambigeometry {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;
// 個体ごとの位相をずらす黄金角 (ラジアン)
constexpr float kGoldenAngle = 2.39996323f;

// 漂う多角形の辺の数の上限。頂点は 1 個あたり辺の数の 2 倍になる
constexpr int kMaxSides = 8;
// 波の輪とリサージュの点の数
constexpr int kRingPoints = 128;
constexpr int kLissajousPoints = 128;

float frac(float v)
{
	return v - std::floor(v);
}

// 相対の値は鳴り始めなどで大きく跳ねるため、動きに使う範囲に収める
float relative(float v)
{
	return std::clamp(v, 0.0f, 3.0f);
}

uint32_t to_byte(float v)
{
	return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

// HSV を 0xAABBGGRR にする。h は 0〜1
uint32_t hsv_color(float h, float s, float v, float a)
{
	const float h6 = frac(h) * 6.0f;
	const int sector = static_cast<int>(h6) % 6;
	const float f = h6 - std::floor(h6);
	const float p = v * (1.0f - s);
	const float q = v * (1.0f - s * f);
	const float t = v * (1.0f - s * (1.0f - f));
	float r = v, g = t, b = p;
	switch (sector) {
	case 0:
		r = v;
		g = t;
		b = p;
		break;
	case 1:
		r = q;
		g = v;
		b = p;
		break;
	case 2:
		r = p;
		g = v;
		b = t;
		break;
	case 3:
		r = p;
		g = q;
		b = v;
		break;
	case 4:
		r = t;
		g = p;
		b = v;
		break;
	default:
		r = v;
		g = p;
		b = q;
		break;
	}
	return (to_byte(a) << 24) | (to_byte(b) << 16) | (to_byte(g) << 8) | to_byte(r);
}

// 中心からの相対の座標で線分を足し、複製は最後にまとめて行う
class Builder {
public:
	Builder(Vertex *out, size_t capacity) : out_(out), capacity_(capacity) {}

	void line(float x0, float y0, float x1, float y1, uint32_t color)
	{
		if (count_ + 2 > capacity_) {
			return;
		}
		out_[count_++] = Vertex{x0, y0, color};
		out_[count_++] = Vertex{x1, y1, color};
	}

	size_t count() const { return count_; }

private:
	Vertex *out_;
	size_t capacity_;
	size_t count_ = 0;
};

void build_polygons(Builder &b, const GeometrySettings &s, const Features &f, double time, float unit, float hue,
		    float value, float alpha, float scale)
{
	const float t = static_cast<float>(time);
	const int count = std::clamp(s.shape_count, kMinShapeCount, kMaxShapeCount);
	for (int i = 0; i < count; ++i) {
		const float phase = static_cast<float>(i) * kGoldenAngle;
		const float fi = static_cast<float>(i);
		// 周期の違う sin と cos で、中心がゆっくり漂う
		const float cx = 0.45f * unit * std::sin(t * 0.13f * (1.0f + 0.10f * fi) + phase);
		const float cy = 0.45f * unit * std::cos(t * 0.11f * (1.0f + 0.07f * fi) + 2.0f * phase);
		const int sides = 3 + (i % (kMaxSides - 2));
		const float base = unit * (0.12f + 0.18f * frac(fi * 0.618034f));
		const float radius = base * scale * (0.8f + 0.2f * relative(f.bass_att));
		const float direction = (i % 2 == 0) ? 1.0f : -1.0f;
		const float rotation = direction * t * (0.2f + 0.3f * relative(f.treb_att)) + phase;
		const float wobble = 0.08f * relative(f.mid);
		const uint32_t color = hsv_color(hue + fi / static_cast<float>(count), s.saturation, value, alpha);

		float px = 0.0f, py = 0.0f;
		for (int k = 0; k <= sides; ++k) {
			const float a = rotation + kTwoPi * static_cast<float>(k % sides) / static_cast<float>(sides);
			const float r =
				radius * (1.0f + wobble * std::sin(static_cast<float>(k % sides) * 1.7f + t * 2.0f));
			const float x = cx + r * std::cos(a);
			const float y = cy + r * std::sin(a);
			if (k > 0) {
				b.line(px, py, x, y, color);
			}
			px = x;
			py = y;
		}
	}
}

// wave の最新 points 個を等間隔に選ぶ
float wave_mono(const StereoSample *wave, size_t wave_count, int index, int points)
{
	if (wave_count == 0) {
		return 0.0f;
	}
	const size_t step = std::max<size_t>(1, wave_count / static_cast<size_t>(points));
	const size_t start = wave_count - std::min(wave_count, step * static_cast<size_t>(points));
	const size_t at = std::min(wave_count - 1, start + step * static_cast<size_t>(index));
	return 0.5f * (wave[at].left + wave[at].right);
}

void build_ring(Builder &b, const GeometrySettings &s, const Features &f, const StereoSample *wave, size_t wave_count,
		double time, float unit, float hue, float value, float alpha, float scale)
{
	const float t = static_cast<float>(time);
	const float base = unit * 0.45f * scale * (0.9f + 0.1f * relative(f.bass_att));
	const float rotation = t * 0.1f;
	float first_x = 0.0f, first_y = 0.0f, px = 0.0f, py = 0.0f;
	for (int k = 0; k < kRingPoints; ++k) {
		const float a = rotation + kTwoPi * static_cast<float>(k) / static_cast<float>(kRingPoints);
		const float sample = std::clamp(wave_mono(wave, wave_count, k, kRingPoints), -1.0f, 1.0f);
		const float r = base + unit * 0.15f * sample;
		const float x = r * std::cos(a);
		const float y = r * std::sin(a);
		const uint32_t color = hsv_color(hue + static_cast<float>(k) / static_cast<float>(kRingPoints) * 0.25f,
						 s.saturation, value, alpha);
		if (k == 0) {
			first_x = x;
			first_y = y;
		} else {
			b.line(px, py, x, y, color);
		}
		px = x;
		py = y;
		if (k == kRingPoints - 1) {
			b.line(px, py, first_x, first_y, color);
		}
	}
}

void build_lissajous(Builder &b, const GeometrySettings &s, const StereoSample *wave, size_t wave_count, double time,
		     float unit, float hue, float value, float alpha, float scale)
{
	if (wave_count == 0) {
		return;
	}
	const size_t step = std::max<size_t>(1, wave_count / kLissajousPoints);
	const size_t used = std::min(wave_count, step * kLissajousPoints);
	const size_t start = wave_count - used;

	// 左右がほぼ同じ音では斜めの直線になるため、少し遅らせた自分自身を縦軸にする
	float diff = 0.0f, total = 0.0f, peak = 0.0f;
	for (size_t i = start; i < wave_count; ++i) {
		diff += std::fabs(wave[i].left - wave[i].right);
		total += std::fabs(wave[i].left);
		peak = std::max({peak, std::fabs(wave[i].left), std::fabs(wave[i].right)});
	}
	const bool mono = diff <= 1e-3f * total;
	const size_t delay = step * 4;
	const float gain = unit * 0.85f * scale / std::max(peak, 0.05f);
	const float rotation = static_cast<float>(time) * 0.05f;
	const float cr = std::cos(rotation), sr = std::sin(rotation);

	float px = 0.0f, py = 0.0f;
	for (int k = 0; k < kLissajousPoints; ++k) {
		const size_t at = start + step * static_cast<size_t>(k);
		if (at >= wave_count) {
			break;
		}
		float u = wave[at].left;
		float v = wave[at].right;
		if (mono) {
			u = 0.5f * (wave[at].left + wave[at].right);
			const size_t back = at >= delay ? at - delay : 0;
			v = 0.5f * (wave[back].left + wave[back].right);
		}
		const float x = gain * (u * cr - v * sr);
		const float y = -gain * (u * sr + v * cr);
		const uint32_t color =
			hsv_color(hue + static_cast<float>(k) / kLissajousPoints * 0.25f, s.saturation, value, alpha);
		if (k > 0) {
			b.line(px, py, x, y, color);
		}
		px = x;
		py = y;
	}
}

} // namespace

size_t build_geometry(const GeometrySettings &settings, const Features &features, const StereoSample *wave,
		      size_t wave_count, double time, float width, float height, Vertex *out)
{
	const int symmetry = std::clamp(settings.symmetry, kMinSymmetry, kMaxSymmetry);
	const size_t per_copy = kMaxVertices / static_cast<size_t>(symmetry);
	const float unit = 0.5f * std::min(width, height);
	const float level = std::clamp(features.level_att, 0.0f, 1.0f);
	// 無音では小さく、透明になる
	const float scale = 0.15f + 0.85f * level;
	const float alpha = level;
	const float value = 0.5f + 0.5f * level;
	const float hue = frac(static_cast<float>(time) * settings.hue_cycles_per_minute / 60.0f);

	Builder builder(out, per_copy);
	switch (settings.pattern) {
	case Pattern::Ring:
		build_ring(builder, settings, features, wave, wave_count, time, unit, hue, value, alpha, scale);
		break;
	case Pattern::Lissajous:
		build_lissajous(builder, settings, wave, wave_count, time, unit, hue, value, alpha, scale);
		break;
	case Pattern::Polygons:
	default:
		build_polygons(builder, settings, features, time, unit, hue, value, alpha, scale);
		break;
	}
	const size_t base_count = builder.count();

	// 中心のまわりに複製し、最後に出力の中心へ動かす
	const float cx = 0.5f * width;
	const float cy = 0.5f * height;
	for (int j = symmetry - 1; j >= 0; --j) {
		const float a = kTwoPi * static_cast<float>(j) / static_cast<float>(symmetry);
		const float c = std::cos(a), s = std::sin(a);
		Vertex *dst = out + base_count * static_cast<size_t>(j);
		for (size_t i = 0; i < base_count; ++i) {
			const Vertex v = out[i];
			dst[i] = Vertex{cx + v.x * c - v.y * s, cy + v.x * s + v.y * c, v.color};
		}
	}
	return base_count * static_cast<size_t>(symmetry);
}

} // namespace ambigeometry
