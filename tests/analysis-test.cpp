// 音の解析と図形の頂点のユニットテスト。OBS を使わない。
// 終了コード: すべての検査が通れば 0、検査に落ちれば 1

#include "audio-analyzer.hpp"
#include "geometry.hpp"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace ambigeometry;

constexpr uint32_t kRate = 48000;
constexpr double kFrame = 1.0 / 60.0;
constexpr size_t kSamplesPerFrame = kRate / 60;

int failures = 0;

void expect(bool condition, const std::string &message)
{
	if (!condition) {
		std::cerr << "FAIL: " << message << '\n';
		++failures;
	}
}

// 1 フレーム分の正弦波を足して特徴量を更新する
void feed_sine(AudioAnalyzer &a, double frequency, double amplitude, uint64_t &position, int frames)
{
	std::vector<StereoSample> buffer(kSamplesPerFrame);
	for (int f = 0; f < frames; ++f) {
		for (size_t i = 0; i < kSamplesPerFrame; ++i) {
			const double t = static_cast<double>(position + i) / kRate;
			const float v =
				static_cast<float>(amplitude * std::sin(2.0 * 3.14159265358979323846 * frequency * t));
			buffer[i] = StereoSample{v, v};
		}
		position += kSamplesPerFrame;
		a.push(buffer.data(), buffer.size());
		a.update(kFrame, kRate);
	}
}

void feed_silence(AudioAnalyzer &a, int frames)
{
	for (int f = 0; f < frames; ++f) {
		a.push_silence(kSamplesPerFrame);
		a.update(kFrame, kRate);
	}
}

int loudest_band(const AudioAnalyzer &a)
{
	const auto &e = a.band_energy();
	int best = 0;
	for (int i = 1; i < 3; ++i) {
		if (e[i] > e[best]) {
			best = i;
		}
	}
	return best;
}

// 正弦波の周波数に当たる帯域の大きさが、ほかの帯域の 10 倍を超える
void test_band_separation()
{
	const double frequencies[3] = {100.0, 1000.0, 5000.0};
	for (int band = 0; band < 3; ++band) {
		AudioAnalyzer a;
		uint64_t position = 0;
		feed_sine(a, frequencies[band], 0.5, position, 10);
		const auto &e = a.band_energy();
		expect(loudest_band(a) == band, "sine " + std::to_string(frequencies[band]) +
							" Hz is not loudest in band " + std::to_string(band));
		for (int other = 0; other < 3; ++other) {
			if (other != band) {
				expect(e[band] > 10.0f * e[other], "band " + std::to_string(band) +
									   " is not 10 times band " +
									   std::to_string(other));
			}
		}
	}
}

// 無音から低音へ切り替えた直後に、bass だけが大きく上がる。音の無い帯域は 1 のまま
void test_transient()
{
	AudioAnalyzer a;
	feed_silence(a, 60);
	expect(std::fabs(a.features().bass - 1.0f) < 1e-6f, "bass is not 1 in silence");
	uint64_t position = 0;
	feed_sine(a, 100.0, 0.5, position, 3);
	expect(a.features().bass > 1.5f, "bass does not rise after silence: " + std::to_string(a.features().bass));
	expect(std::fabs(a.features().treb - 1.0f) < 0.1f,
	       "treb moves without treble: " + std::to_string(a.features().treb));
}

// 絶対の音量は無音で 0、振幅 1 の矩形波 (RMS 0 dBFS) で 1、-60 dBFS 以下で 0
void test_level()
{
	{
		AudioAnalyzer a;
		feed_silence(a, 10);
		expect(a.features().level == 0.0f, "level is not 0 in silence");
		expect(a.features().level_att == 0.0f, "level_att is not 0 in silence");
	}
	{
		AudioAnalyzer a;
		std::vector<StereoSample> buffer(AudioAnalyzer::kWindow);
		for (size_t i = 0; i < buffer.size(); ++i) {
			const float v = (i / 24) % 2 == 0 ? 1.0f : -1.0f;
			buffer[i] = StereoSample{v, v};
		}
		a.push(buffer.data(), buffer.size());
		a.update(kFrame, kRate);
		expect(std::fabs(a.features().level - 1.0f) < 1e-3f,
		       "level of a full scale square is not 1: " + std::to_string(a.features().level));
	}
	{
		AudioAnalyzer a;
		uint64_t position = 0;
		// 振幅 0.0005 の正弦波の RMS は約 -69 dBFS
		feed_sine(a, 440.0, 0.0005, position, 10);
		expect(a.features().level == 0.0f,
		       "level below -60 dBFS is not 0: " + std::to_string(a.features().level));
	}
	{
		AudioAnalyzer a;
		uint64_t position = 0;
		feed_sine(a, 440.0, 0.5, position, 30);
		feed_silence(a, 60);
		expect(a.features().level_att < 0.002f,
		       "level_att does not fall to 0 after sound stops: " + std::to_string(a.features().level_att));
	}
}

bool all_finite(const std::vector<Vertex> &v, size_t count)
{
	for (size_t i = 0; i < count; ++i) {
		if (!std::isfinite(v[i].x) || !std::isfinite(v[i].y)) {
			return false;
		}
	}
	return true;
}

// どのパターンも、最大の設定で頂点の上限を超えず、座標が有限で、線分の組 (偶数個) になる
void test_geometry_limits()
{
	AudioAnalyzer a;
	uint64_t position = 0;
	feed_sine(a, 220.0, 0.9, position, 10);
	std::vector<Vertex> out(kMaxVertices);
	const Pattern patterns[3] = {Pattern::Polygons, Pattern::Ring, Pattern::Lissajous};
	for (Pattern p : patterns) {
		for (int symmetry = kMinSymmetry; symmetry <= kMaxSymmetry; ++symmetry) {
			GeometrySettings s;
			s.pattern = p;
			s.shape_count = kMaxShapeCount;
			s.symmetry = symmetry;
			const size_t n = build_geometry(s, a.features(), a.wave(), AudioAnalyzer::kWave, 12.3, 1920.0f,
							1080.0f, out.data());
			const std::string name = "pattern " + std::to_string(static_cast<int>(p)) + " symmetry " +
						 std::to_string(symmetry);
			expect(n > 0, name + " draws nothing");
			expect(n <= kMaxVertices, name + " exceeds the vertex limit: " + std::to_string(n));
			expect(n % 2 == 0, name + " has an odd number of vertices");
			expect(all_finite(out, n), name + " has a non-finite vertex");
		}
	}
}

// 無音では頂点の色が透明になる
void test_geometry_silence_is_transparent()
{
	AudioAnalyzer a;
	feed_silence(a, 10);
	std::vector<Vertex> out(kMaxVertices);
	GeometrySettings s;
	const size_t n =
		build_geometry(s, a.features(), a.wave(), AudioAnalyzer::kWave, 0.0, 640.0f, 360.0f, out.data());
	bool transparent = true;
	for (size_t i = 0; i < n; ++i) {
		transparent = transparent && (out[i].color >> 24) == 0;
	}
	expect(transparent, "silence draws opaque vertices");
}

} // namespace

int main()
{
	test_band_separation();
	test_transient();
	test_level();
	test_geometry_limits();
	test_geometry_silence_is_transparent();
	if (failures == 0) {
		std::cout << "PASS analysis\n";
		return 0;
	}
	std::cerr << failures << " check(s) failed in analysis\n";
	return 1;
}
