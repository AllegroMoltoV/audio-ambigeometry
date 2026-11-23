#pragma once

// Audio parts of the render test: starting the libobs audio pipeline, a test audio source, and feeding it known
// waveforms. obs_source_output_audio runs the audio capture callbacks of the source on the calling thread, so the
// plugin receives the samples before the call returns.

#include "render-fixture.hpp"

#include <obs.h>
#include <util/platform.h>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

namespace audio_test {

constexpr const char *kAudioSourceId = "render_test_audio";
constexpr uint32_t kSampleRate = 48000;

// Call after obs_test::start. Returns false on failure.
inline bool start_audio()
{
	obs_audio_info info = {};
	info.samples_per_sec = kSampleRate;
	info.speakers = SPEAKERS_STEREO;
	if (!obs_reset_audio(&info)) {
		std::cerr << "failed to initialize OBS audio\n";
		return false;
	}
	return true;
}

inline void register_audio_source()
{
	obs_source_info info = {};
	info.id = kAudioSourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_AUDIO;
	info.get_name = [](void *) {
		return "Render test audio";
	};
	info.create = [](obs_data_t *, obs_source_t *source) -> void * {
		return source;
	};
	info.destroy = [](void *) {
	};
	obs_register_source(&info);
}

inline obs_source_t *create_audio_source(const char *name)
{
	return obs_source_create(kAudioSourceId, name, nullptr, nullptr);
}

// A stereo waveform generator. left and right are functions of the time in seconds that return a sample in -1..1.
struct Signal {
	double (*left)(double t, double frequency, double amplitude) = nullptr;
	double (*right)(double t, double frequency, double amplitude) = nullptr;
	double frequency = 440.0;
	double amplitude = 0.5;
};

inline double sine(double t, double frequency, double amplitude)
{
	return amplitude * std::sin(2.0 * 3.14159265358979323846 * frequency * t);
}

inline double cosine(double t, double frequency, double amplitude)
{
	return amplitude * std::cos(2.0 * 3.14159265358979323846 * frequency * t);
}

inline double silent(double, double, double)
{
	return 0.0;
}

// Outputs frames samples of the signal to the source, continuing from sample position.
inline void output(obs_source_t *source, const Signal &signal, uint64_t &position, uint32_t frames)
{
	std::vector<float> left(frames);
	std::vector<float> right(frames);
	for (uint32_t i = 0; i < frames; ++i) {
		const double t = static_cast<double>(position + i) / kSampleRate;
		left[i] = static_cast<float>(signal.left(t, signal.frequency, signal.amplitude));
		right[i] = static_cast<float>(
			(signal.right ? signal.right : signal.left)(t, signal.frequency, signal.amplitude));
	}
	position += frames;
	obs_source_audio audio = {};
	audio.data[0] = reinterpret_cast<const uint8_t *>(left.data());
	audio.data[1] = reinterpret_cast<const uint8_t *>(right.data());
	audio.frames = frames;
	audio.speakers = SPEAKERS_STEREO;
	audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
	audio.samples_per_sec = kSampleRate;
	audio.timestamp = os_gettime_ns();
	obs_source_output_audio(source, &audio);
}

// A source and the signal to feed it.
struct Feed {
	obs_source_t *source = nullptr;
	Signal signal;
	uint64_t position = 0;
};

// Feeds every source for count video frames, about one video frame of audio per frame. Returns false if the
// graphics thread stops advancing.
inline bool pump_all(std::vector<Feed> &feeds, int count)
{
	const uint32_t frames_per_video_frame = kSampleRate / 60;
	for (int i = 0; i < count; ++i) {
		for (Feed &feed : feeds) {
			output(feed.source, feed.signal, feed.position, frames_per_video_frame);
		}
		if (!render_test::wait_frames(1)) {
			return false;
		}
	}
	return true;
}

inline bool pump(obs_source_t *source, const Signal &signal, uint64_t &position, int count)
{
	std::vector<Feed> feeds = {Feed{source, signal, position}};
	const bool ok = pump_all(feeds, count);
	position = feeds[0].position;
	return ok;
}

} // namespace audio_test
