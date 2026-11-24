#include "audio-input.hpp"

#include <cmath>

namespace ambigeometry {

namespace {

// 音声が届かない時間がこれを超えたら、無音として扱う。音声は数十ミリ秒ごとにまとめて届くため、
// 1 フレーム届かないだけでは無音にしない
constexpr double kSilenceAfter = 0.1;
// 選んだ音声ソースが見つからないときに、名前から引き直す間隔。シーンコレクションの読み込みで、選んだソースが
// このソースより後に作られる場合に取り直すため
constexpr double kResolveInterval = 1.0;
// 1 フレームで読むサンプルの上限
constexpr size_t kReadMax = 4096;
// 最終ミックスはトラック 1 を使う。配信の既定の音声トラックである
constexpr size_t kFinalMixTrack = 0;

} // namespace

AudioInput::AudioInput() : scratch_(kReadMax) {}

AudioInput::~AudioInput()
{
	Detach();
}

void AudioInput::SetName(const std::string &name)
{
	std::lock_guard<std::mutex> lock(name_mutex_);
	name_ = name;
}

std::string AudioInput::Name()
{
	std::lock_guard<std::mutex> lock(name_mutex_);
	return name_;
}

bool AudioInput::Rename(const char *prev, const char *next)
{
	std::lock_guard<std::mutex> lock(name_mutex_);
	if (name_.empty() || name_ == kFinalMix || name_ != prev) {
		return false;
	}
	name_ = next;
	return true;
}

void AudioInput::OnSourceAudio(void *param, obs_source_t *, const audio_data *audio, bool muted)
{
	auto *self = static_cast<AudioInput *>(param);
	// ミュート中も callback は呼ばれる。音として扱わないよう 0 を書く
	if (muted) {
		self->ring_.write(nullptr, nullptr, audio->frames);
		return;
	}
	self->ring_.write(reinterpret_cast<const float *>(audio->data[0]),
			  reinterpret_cast<const float *>(audio->data[1]), audio->frames);
}

void AudioInput::OnMixAudio(void *param, size_t, audio_data *audio)
{
	auto *self = static_cast<AudioInput *>(param);
	self->ring_.write(reinterpret_cast<const float *>(audio->data[0]),
			  reinterpret_cast<const float *>(audio->data[1]), audio->frames);
}

void AudioInput::DrainRing()
{
	// 付ける前の古いサンプルを読まないよう、読む位置を書き込みの位置へ進める
	StereoSample discard[1];
	while (ring_.read(cursor_, discard, 1) > 0) {
	}
}

void AudioInput::Detach()
{
	if (weak_source_) {
		obs_source_t *source = obs_weak_source_get_source(weak_source_);
		if (source) {
			obs_source_remove_audio_capture_callback(source, OnSourceAudio, this);
			obs_source_release(source);
		}
		obs_weak_source_release(weak_source_);
		weak_source_ = nullptr;
	}
	if (mix_attached_) {
		obs_remove_raw_audio_callback(kFinalMixTrack, OnMixAudio, this);
		mix_attached_ = false;
	}
	attached_name_.clear();
	attempted_ = false;
}

void AudioInput::Resolve(float seconds, const std::string &name, uint32_t sample_rate)
{
	// 付けている先が今の名前と合っていれば、そのまま使う。音声ソースはフェーダーの値を読む
	if (mix_attached_ && name == kFinalMix) {
		gain_ = 1.0f;
		return;
	}
	if (weak_source_) {
		obs_source_t *source = obs_weak_source_get_source(weak_source_);
		const bool same = source && name == attached_name_ && name == obs_source_get_name(source);
		if (same) {
			gain_ = obs_source_get_volume(source);
		}
		obs_source_release(source);
		if (same) {
			return;
		}
	}
	if (weak_source_ || mix_attached_) {
		Detach();
	}
	if (name.empty()) {
		return;
	}

	if (name == kFinalMix) {
		DrainRing();
		audio_convert_info conversion = {};
		conversion.samples_per_sec = sample_rate;
		conversion.format = AUDIO_FORMAT_FLOAT_PLANAR;
		conversion.speakers = SPEAKERS_STEREO;
		obs_add_raw_audio_callback(kFinalMixTrack, &conversion, OnMixAudio, this);
		mix_attached_ = true;
		attached_name_ = name;
		gain_ = 1.0f;
		return;
	}

	since_resolve_ += seconds;
	if (attempted_ && since_resolve_ < kResolveInterval) {
		return;
	}
	attempted_ = true;
	since_resolve_ = 0.0;

	obs_source_t *source = obs_get_source_by_name(name.c_str());
	if (!source) {
		return;
	}
	if ((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) != 0) {
		DrainRing();
		obs_source_add_audio_capture_callback(source, OnSourceAudio, this);
		weak_source_ = obs_source_get_weak_source(source);
		attached_name_ = name;
		gain_ = obs_source_get_volume(source);
	}
	obs_source_release(source);
}

void AudioInput::Tick(float seconds, uint32_t sample_rate)
{
	const std::string name = Name();
	Resolve(seconds, name, sample_rate);
	active_ = !name.empty();
	if (!active_) {
		return;
	}

	size_t total = 0;
	if (weak_source_ || mix_attached_) {
		size_t n;
		while ((n = ring_.read(cursor_, scratch_.data(), kReadMax)) > 0) {
			window_.push(scratch_.data(), n);
			total += n;
			if (n < kReadMax) {
				break;
			}
		}
	}
	if (total > 0) {
		since_data_ = 0.0;
	} else {
		since_data_ += seconds;
		if (since_data_ > kSilenceAfter) {
			window_.push_silence(static_cast<size_t>(std::lround(seconds * sample_rate)));
		}
	}
}

} // namespace ambigeometry
