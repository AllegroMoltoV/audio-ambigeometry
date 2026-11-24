#include "audio-analyzer.hpp"

#include <algorithm>
#include <cmath>

namespace ambigeometry {

namespace {

// Butterchurn の帯域の境界 (Hz)
constexpr float kBandEdges[4] = {20.0f, 320.0f, 2800.0f, 11025.0f};

// 長い平均がこれを下回ったら、相対の値を 1 とする。Butterchurn は -128〜127 の整数の入力に 0.001 を使う。
// ここでは ±1 の浮動小数を入れるため、その 1/128 にした
constexpr float kRelativeFloor = 0.001f / 128.0f;

// 30 fps を基準にした減衰の係数を、実際のフレームの間隔に合わせる (Butterchurn の adjustRateToFPS)
float adjust_rate(float rate, double fps)
{
	return static_cast<float>(std::pow(static_cast<double>(rate), 30.0 / fps));
}

} // namespace

void SampleWindow::append(const StereoSample *samples, size_t count, bool silence)
{
	if (count >= kSize) {
		if (silence) {
			std::fill(samples_.begin(), samples_.end(), StereoSample{});
		} else {
			std::copy(samples + (count - kSize), samples + count, samples_.begin());
		}
		return;
	}
	std::copy(samples_.begin() + static_cast<std::ptrdiff_t>(count), samples_.end(), samples_.begin());
	auto tail = samples_.begin() + static_cast<std::ptrdiff_t>(kSize - count);
	if (silence) {
		std::fill(tail, samples_.end(), StereoSample{});
	} else {
		std::copy(samples, samples + count, tail);
	}
}

void SampleWindow::push(const StereoSample *samples, size_t count)
{
	append(samples, count, false);
}

void SampleWindow::push_silence(size_t count)
{
	append(nullptr, count, true);
}

void SampleWindow::assign(const StereoSample *samples)
{
	std::copy(samples, samples + kSize, samples_.begin());
}

AudioAnalyzer::AudioAnalyzer() : fft_(kWindow, kWindow / 2, true), mono_(kWindow) {}

void AudioAnalyzer::update(double seconds, uint32_t sample_rate)
{
	double fps = seconds > 0.0 ? 1.0 / seconds : 30.0;
	fps = std::clamp(fps, 15.0, 144.0);

	const StereoSample *history = window_.data();
	double sum_squares = 0.0;
	for (size_t i = 0; i < kWindow; ++i) {
		const float m = 0.5f * (history[i].left + history[i].right);
		mono_[i] = m;
		sum_squares += static_cast<double>(m) * m;
	}
	const double mean_square = sum_squares / static_cast<double>(kWindow);
	const float db = static_cast<float>(10.0 * std::log10(mean_square + 1e-12));
	features_.level = std::clamp((db - kFloorDb) / -kFloorDb, 0.0f, 1.0f);
	{
		const float rate = adjust_rate(features_.level > features_.level_att ? 0.2f : 0.5f, fps);
		features_.level_att = features_.level_att * rate + features_.level * (1.0f - rate);
	}

	fft_.TimeToFrequencyDomain(mono_, spectrum_);
	const float bucket_hz = static_cast<float>(sample_rate) / static_cast<float>(fft_.NumFrequencies());
	const int last = static_cast<int>(spectrum_.size()) - 1;
	int index[4];
	for (int b = 0; b < 4; ++b) {
		index[b] = std::clamp(static_cast<int>(std::lround(kBandEdges[b] / bucket_hz)) - 1, 0, last);
	}

	float val[3];
	float att[3];
	for (int b = 0; b < 3; ++b) {
		float sum = 0.0f;
		for (int j = index[b]; j < index[b + 1]; ++j) {
			sum += spectrum_[static_cast<size_t>(j)];
		}
		imm_[b] = sum;

		float rate = adjust_rate(sum > avg_[b] ? 0.2f : 0.5f, fps);
		avg_[b] = avg_[b] * rate + sum * (1.0f - rate);
		rate = adjust_rate(frame_ < 50 ? 0.9f : 0.992f, fps);
		long_avg_[b] = long_avg_[b] * rate + sum * (1.0f - rate);

		if (long_avg_[b] < kRelativeFloor) {
			val[b] = 1.0f;
			att[b] = 1.0f;
		} else {
			val[b] = sum / long_avg_[b];
			att[b] = avg_[b] / long_avg_[b];
		}
	}
	features_.bass = val[0];
	features_.mid = val[1];
	features_.treb = val[2];
	features_.bass_att = att[0];
	features_.mid_att = att[1];
	features_.treb_att = att[2];
	++frame_;
}

} // namespace ambigeometry
