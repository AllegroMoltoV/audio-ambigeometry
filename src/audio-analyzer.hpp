#pragma once

// 音の特徴量を 1 フレームに 1 回求める。OBS に依存しないため、ユニットテストで確かめられる。
// 絶対の音量で図形の明るさと大きさを、相対の bass・mid・treb で動きを決める。
// bass・mid・treb の求め方は Butterchurn (https://github.com/jberg/butterchurn) の src/audio/audioLevels.js と同じで、
// 1,024 点の FFT の結果を 20〜320 Hz・320〜2,800 Hz・2,800〜11,025 Hz に足し、長い平均で割る。
// この値は音が鳴り続ければ音量によらず 1 前後に収まり、無音では 1 になるため、音の強さには使わない

#include "milkdrop-fft.hpp"
#include "sample-ring.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ambigeometry {

struct Features {
	// 絶対の音量。RMS が -60 dBFS 以下で 0、0 dBFS で 1
	float level = 0.0f;
	// level をなめらかにした値。図形の大きさと明るさに使う
	float level_att = 0.0f;
	// 帯域ごとの相対の値。1 が普通の大きさ
	float bass = 1.0f;
	float mid = 1.0f;
	float treb = 1.0f;
	// 時間でなめらかにした相対の値
	float bass_att = 1.0f;
	float mid_att = 1.0f;
	float treb_att = 1.0f;
};

class AudioAnalyzer {
public:
	// FFT に入れるサンプルの数
	static constexpr size_t kWindow = 1024;
	// 波形を描く図形に渡すサンプルの数
	static constexpr size_t kWave = 512;
	// 音量を 0 とみなす下限 (dBFS)
	static constexpr float kFloorDb = -60.0f;

	AudioAnalyzer();

	// 新しいサンプルを古いものから順に足す
	void push(const StereoSample *samples, size_t count);
	// 無音のサンプルを count 個足す。音声が届かないときに使う
	void push_silence(size_t count);
	// 経過時間と入力のサンプリング周波数から特徴量を更新する
	void update(double seconds, uint32_t sample_rate);

	const Features &features() const { return features_; }
	// 最新の kWave 個のサンプル。古いものから並ぶ
	const StereoSample *wave() const { return history_.data() + (kWindow - kWave); }
	// 相対化する前の帯域ごとの大きさ。ユニットテストで帯域の分け方を確かめるために公開する
	const std::array<float, 3> &band_energy() const { return imm_; }

private:
	void append(const StereoSample *samples, size_t count, bool silence);

	MilkdropFFT fft_;
	std::vector<StereoSample> history_;
	std::vector<float> mono_;
	std::vector<float> spectrum_;
	std::array<float, 3> imm_{};
	std::array<float, 3> avg_{};
	std::array<float, 3> long_avg_{};
	uint32_t frame_ = 0;
	Features features_;
};

} // namespace ambigeometry
