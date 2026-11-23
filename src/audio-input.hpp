#pragma once

// 音声の欄 1 つ分の入力。OBS の音声ソースか、最終ミックス (トラック 1) から音を受け取る。
// 音声のスレッドの callback はサンプルをリングバッファに書くだけにし、窓の更新は video_tick で行う

#include "audio-analyzer.hpp"
#include "sample-ring.hpp"

#include <obs-module.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace ambigeometry {

// 最終ミックスを表す設定の値。ソースの名前と重ならないよう、制御文字 (0x1F) で始める
constexpr const char *kFinalMix = "\037final-mix";

class AudioInput {
public:
	AudioInput();
	~AudioInput();

	AudioInput(const AudioInput &) = delete;
	AudioInput &operator=(const AudioInput &) = delete;

	// 設定の値。空ならこの欄を使わない。kFinalMix なら最終ミックス、それ以外は音声ソースの名前
	void SetName(const std::string &name);
	std::string Name();
	// 音声ソースの名前が prev から next に変わったときに呼ぶ。この欄の名前を変えたら true を返す
	bool Rename(const char *prev, const char *next);

	// video_tick から呼ぶ。付ける先を合わせ、届いたサンプルで窓を進める
	void Tick(float seconds, uint32_t sample_rate);
	// 使っているなら true。window と gain は使っているときだけ意味を持つ
	bool Active() const { return active_; }
	const SampleWindow &Window() const { return window_; }
	// 窓に掛ける音量。音声ソースではフェーダーの値、最終ミックスでは 1
	float Gain() const { return gain_; }

	// 付けている先から外す
	void Detach();

private:
	static void OnSourceAudio(void *param, obs_source_t *, const audio_data *audio, bool muted);
	static void OnMixAudio(void *param, size_t mix_idx, audio_data *audio);

	void Resolve(float seconds, const std::string &name, uint32_t sample_rate);
	void DrainRing();

	std::mutex name_mutex_;
	std::string name_;

	// 付けている先。音声ソースなら weak_source_、最終ミックスなら mix_attached_
	std::string attached_name_;
	obs_weak_source_t *weak_source_ = nullptr;
	bool mix_attached_ = false;
	bool attempted_ = false;
	double since_resolve_ = 0.0;

	SampleRing ring_;
	uint64_t cursor_ = 0;
	std::vector<StereoSample> scratch_;
	SampleWindow window_;
	double since_data_ = 0.0;
	bool active_ = false;
	float gain_ = 1.0f;
};

} // namespace ambigeometry
