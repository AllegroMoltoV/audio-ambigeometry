/*
Audio Ambigeometry
Copyright (C) 2026 AllegroMoltoV

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

// 選んだ音声 (最大 4 つ) の音に合わせて、幾何学図形が漂う映像ソース。
// 音は欄ごとの AudioInput が受け取り、足し合わせた窓を video_tick で 1 フレームに 1 回解析して頂点を作る。
// 頂点は作成時に 1 回だけ作った動的な頂点バッファに書き換えて描く。即時描画は 1 回に 512 頂点が上限のため使わない

#include "audio-analyzer.hpp"
#include "audio-input.hpp"
#include "geometry.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace ambigeometry;

constexpr const char *kSourceId = "audio_ambigeometry_source";

constexpr size_t kInputCount = 4;
constexpr const char *kAudioSources[kInputCount] = {"audio_source_1", "audio_source_2", "audio_source_3",
						    "audio_source_4"};
constexpr const char *kAudioSourceLabels[kInputCount] = {"AudioSource1", "AudioSource2", "AudioSource3",
							 "AudioSource4"};
constexpr const char *kPattern = "pattern";
constexpr const char *kShapeCount = "shape_count";
constexpr const char *kSymmetry = "symmetry";
constexpr const char *kTrail = "trail";
constexpr const char *kHueSpeed = "hue_speed";
constexpr const char *kSaturation = "saturation";
constexpr const char *kWidth = "width";
constexpr const char *kHeight = "height";

constexpr int kMinSize = 16;
constexpr int kMaxSize = 4096;
constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 720;
// 残像 (%)。0 で無効
constexpr int kMaxTrail = 98;
constexpr int kDefaultTrail = 90;
// 減衰のあとに引く値。8 ビットの色で、薄い残像が丸めで消えずに残らないようにする
constexpr float kFadeStep = 1.5f / 255.0f;

int get_int_clamped(obs_data_t *settings, const char *name, int min, int max)
{
	return static_cast<int>(std::clamp<long long>(obs_data_get_int(settings, name), min, max));
}

class AmbigeometrySource final {
public:
	AmbigeometrySource(obs_data_t *settings, obs_source_t *self) : self_(self), mix_(AudioAnalyzer::kWindow)
	{
		for (auto &input : inputs_) {
			input = std::make_unique<AudioInput>();
		}
		vertices_.resize(kMaxVertices);

		obs_enter_graphics();
		char *path = obs_module_file("ambigeometry.effect");
		if (path) {
			effect_ = gs_effect_create_from_file(path, nullptr);
			bfree(path);
		}
		if (effect_) {
			param_image_ = gs_effect_get_param_by_name(effect_, "image");
			param_decay_ = gs_effect_get_param_by_name(effect_, "decay");
			param_fade_step_ = gs_effect_get_param_by_name(effect_, "fade_step");
		} else {
			obs_log(LOG_WARNING, "[audio_ambigeometry] Failed to load effect: ambigeometry.effect");
		}
		gs_vb_data *data = gs_vbdata_create();
		data->num = kMaxVertices;
		data->points = static_cast<vec3 *>(bzalloc(sizeof(vec3) * kMaxVertices));
		data->colors = static_cast<uint32_t *>(bzalloc(sizeof(uint32_t) * kMaxVertices));
		vertex_buffer_ = gs_vertexbuffer_create(data, GS_DYNAMIC);
		trails_[0] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		trails_[1] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		obs_leave_graphics();

		signal_handler_connect(obs_get_signal_handler(), "source_rename", OnSourceRename, this);

		Update(settings);
	}

	~AmbigeometrySource()
	{
		signal_handler_disconnect(obs_get_signal_handler(), "source_rename", OnSourceRename, this);
		for (auto &input : inputs_) {
			input->Detach();
		}

		obs_enter_graphics();
		gs_texrender_destroy(trails_[0]);
		gs_texrender_destroy(trails_[1]);
		gs_vertexbuffer_destroy(vertex_buffer_);
		gs_effect_destroy(effect_);
		obs_leave_graphics();
	}

	AmbigeometrySource(const AmbigeometrySource &) = delete;
	AmbigeometrySource &operator=(const AmbigeometrySource &) = delete;

	void Update(obs_data_t *settings)
	{
		for (size_t i = 0; i < kInputCount; ++i) {
			inputs_[i]->SetName(obs_data_get_string(settings, kAudioSources[i]));
		}
		GeometrySettings geometry;
		geometry.pattern = static_cast<Pattern>(get_int_clamped(settings, kPattern, 0, 2));
		geometry.shape_count = get_int_clamped(settings, kShapeCount, kMinShapeCount, kMaxShapeCount);
		geometry.symmetry = get_int_clamped(settings, kSymmetry, kMinSymmetry, kMaxSymmetry);
		geometry.hue_cycles_per_minute =
			static_cast<float>(std::clamp(obs_data_get_double(settings, kHueSpeed), 0.0, 60.0));
		geometry.saturation = static_cast<float>(get_int_clamped(settings, kSaturation, 0, 100)) / 100.0f;
		geometry_ = geometry;
		trail_ = static_cast<float>(get_int_clamped(settings, kTrail, 0, kMaxTrail)) / 100.0f;
		width_ = static_cast<uint32_t>(get_int_clamped(settings, kWidth, kMinSize, kMaxSize));
		height_ = static_cast<uint32_t>(get_int_clamped(settings, kHeight, kMinSize, kMaxSize));
	}

	std::string AudioName(size_t index) { return inputs_[index]->Name(); }

	uint32_t Width() const { return width_; }
	uint32_t Height() const { return height_; }

	void Tick(float seconds)
	{
		obs_audio_info audio_info = {};
		const uint32_t sample_rate = obs_get_audio_info(&audio_info) ? audio_info.samples_per_sec : 48000;

		// 欄ごとの窓に音量を掛けて足し合わせ、まとめて 1 回だけ解析する
		std::fill(mix_.begin(), mix_.end(), StereoSample{});
		for (auto &input : inputs_) {
			input->Tick(seconds, sample_rate);
			if (!input->Active()) {
				continue;
			}
			const StereoSample *window = input->Window().data();
			const float gain = input->Gain();
			for (size_t i = 0; i < mix_.size(); ++i) {
				mix_[i].left += window[i].left * gain;
				mix_[i].right += window[i].right * gain;
			}
		}
		analyzer_.assign(mix_.data());
		analyzer_.update(seconds, sample_rate);

		time_ += seconds;
		vertex_count_ = build_geometry(geometry_, analyzer_.features(), analyzer_.wave(), AudioAnalyzer::kWave,
					       time_, static_cast<float>(width_), static_cast<float>(height_),
					       vertices_.data());
		dirty_ = true;
	}

	void Render()
	{
		if (!effect_ || !vertex_buffer_) {
			return;
		}
		// video_render は同じフレームに何度も呼ばれることがある (プレビューと配信など)。
		// 頂点の書き換えと残像の更新は、video_tick のあとの最初の 1 回だけ行う
		const bool advance = dirty_;
		dirty_ = false;
		if (advance) {
			gs_vb_data *data = gs_vertexbuffer_get_data(vertex_buffer_);
			for (size_t i = 0; i < vertex_count_; ++i) {
				vec3_set(&data->points[i], vertices_[i].x, vertices_[i].y, 0.0f);
				data->colors[i] = vertices_[i].color;
			}
			gs_vertexbuffer_flush(vertex_buffer_);
		}

		const bool previous_srgb = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(false);
		gs_blend_state_push();

		if (trail_ <= 0.0f) {
			trail_valid_ = false;
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
			DrawLines();
		} else {
			if (advance) {
				AdvanceTrail();
			}
			gs_texture_t *texture = gs_texrender_get_texture(trails_[current_]);
			if (trail_valid_ && texture) {
				gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
				gs_effect_set_texture(param_image_, texture);
				while (gs_effect_loop(effect_, "Copy")) {
					gs_draw_sprite(texture, 0, width_, height_);
				}
			}
		}

		gs_blend_state_pop();
		gs_enable_framebuffer_srgb(previous_srgb);
	}

private:
	static void OnSourceRename(void *param, calldata_t *data)
	{
		auto *self = static_cast<AmbigeometrySource *>(param);
		const char *prev_name = calldata_string(data, "prev_name");
		const char *new_name = calldata_string(data, "new_name");
		if (!prev_name || !new_name) {
			return;
		}
		for (size_t i = 0; i < kInputCount; ++i) {
			if (self->inputs_[i]->Rename(prev_name, new_name)) {
				obs_data_t *settings = obs_source_get_settings(self->self_);
				obs_data_set_string(settings, kAudioSources[i], new_name);
				obs_data_release(settings);
			}
		}
	}

	void DrawLines()
	{
		if (vertex_count_ == 0) {
			return;
		}
		gs_load_vertexbuffer(vertex_buffer_);
		gs_load_indexbuffer(nullptr);
		while (gs_effect_loop(effect_, "Lines")) {
			gs_draw(GS_LINES, 0, static_cast<uint32_t>(vertex_count_));
		}
		gs_load_vertexbuffer(nullptr);
	}

	void AdvanceTrail()
	{
		if (trail_width_ != width_ || trail_height_ != height_) {
			trail_valid_ = false;
			trail_width_ = width_;
			trail_height_ = height_;
		}
		const int previous = current_;
		current_ ^= 1;
		gs_texrender_reset(trails_[current_]);
		if (!gs_texrender_begin(trails_[current_], width_, height_)) {
			trail_valid_ = false;
			return;
		}
		vec4 clear = {};
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, static_cast<float>(width_), 0.0f, static_cast<float>(height_), -100.0f, 100.0f);

		gs_texture_t *last = gs_texrender_get_texture(trails_[previous]);
		if (trail_valid_ && last) {
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
			gs_effect_set_texture(param_image_, last);
			gs_effect_set_float(param_decay_, trail_);
			gs_effect_set_float(param_fade_step_, kFadeStep);
			while (gs_effect_loop(effect_, "Fade")) {
				gs_draw_sprite(last, 0, width_, height_);
			}
		}
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		DrawLines();
		gs_texrender_end(trails_[current_]);
		trail_valid_ = true;
	}

	obs_source_t *self_;

	std::array<std::unique_ptr<AudioInput>, kInputCount> inputs_;
	std::vector<StereoSample> mix_;
	AudioAnalyzer analyzer_;

	GeometrySettings geometry_;
	float trail_ = kDefaultTrail / 100.0f;
	uint32_t width_ = kDefaultWidth;
	uint32_t height_ = kDefaultHeight;
	double time_ = 0.0;

	std::vector<Vertex> vertices_;
	size_t vertex_count_ = 0;
	bool dirty_ = false;

	gs_effect_t *effect_ = nullptr;
	gs_eparam_t *param_image_ = nullptr;
	gs_eparam_t *param_decay_ = nullptr;
	gs_eparam_t *param_fade_step_ = nullptr;
	gs_vertbuffer_t *vertex_buffer_ = nullptr;
	gs_texrender_t *trails_[2] = {nullptr, nullptr};
	int current_ = 0;
	bool trail_valid_ = false;
	uint32_t trail_width_ = 0;
	uint32_t trail_height_ = 0;
};

bool add_audio_source(void *param, obs_source_t *source)
{
	auto *list = static_cast<obs_property_t *>(param);
	if ((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) != 0) {
		const char *name = obs_source_get_name(source);
		obs_property_list_add_string(list, name, name);
	}
	return true;
}

bool list_has_string(obs_property_t *list, const std::string &value)
{
	const size_t count = obs_property_list_item_count(list);
	for (size_t i = 0; i < count; ++i) {
		const char *item = obs_property_list_item_string(list, i);
		if (item && value == item) {
			return true;
		}
	}
	return false;
}

bool pattern_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const bool polygons = obs_data_get_int(settings, kPattern) == static_cast<int>(Pattern::Polygons);
	obs_property_set_visible(obs_properties_get(props, kShapeCount), polygons);
	return true;
}

obs_properties_t *get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	for (size_t i = 0; i < kInputCount; ++i) {
		obs_property_t *audio = obs_properties_add_list(props, kAudioSources[i],
								obs_module_text(kAudioSourceLabels[i]),
								OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		obs_property_list_add_string(audio, obs_module_text("AudioSource.None"), "");
		obs_property_list_add_string(audio, obs_module_text("AudioSource.FinalMix"), kFinalMix);
		obs_enum_sources(add_audio_source, audio);
		// 選んだソースが今は無いときも、設定の名前を一覧に残す。プロパティを開いただけで選択が消えないようにするため
		if (data) {
			const std::string current = static_cast<AmbigeometrySource *>(data)->AudioName(i);
			if (!current.empty() && !list_has_string(audio, current)) {
				obs_property_list_add_string(audio, current.c_str(), current.c_str());
			}
		}
	}

	obs_property_t *pattern = obs_properties_add_list(props, kPattern, obs_module_text("Pattern"),
							  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(pattern, obs_module_text("Pattern.Polygons"), static_cast<int>(Pattern::Polygons));
	obs_property_list_add_int(pattern, obs_module_text("Pattern.Ring"), static_cast<int>(Pattern::Ring));
	obs_property_list_add_int(pattern, obs_module_text("Pattern.Lissajous"), static_cast<int>(Pattern::Lissajous));
	obs_property_set_modified_callback(pattern, pattern_modified);

	obs_properties_add_int_slider(props, kShapeCount, obs_module_text("ShapeCount"), kMinShapeCount, kMaxShapeCount,
				      1);
	obs_properties_add_int_slider(props, kSymmetry, obs_module_text("Symmetry"), kMinSymmetry, kMaxSymmetry, 1);
	obs_property_t *trail = obs_properties_add_int_slider(props, kTrail, obs_module_text("Trail"), 0, kMaxTrail, 1);
	obs_property_int_set_suffix(trail, "%");
	obs_properties_add_float_slider(props, kHueSpeed, obs_module_text("HueSpeed"), 0.0, 60.0, 0.5);
	obs_property_t *saturation =
		obs_properties_add_int_slider(props, kSaturation, obs_module_text("Saturation"), 0, 100, 1);
	obs_property_int_set_suffix(saturation, "%");
	obs_properties_add_int(props, kWidth, obs_module_text("Width"), kMinSize, kMaxSize, 1);
	obs_properties_add_int(props, kHeight, obs_module_text("Height"), kMinSize, kMaxSize, 1);
	return props;
}

} // namespace

extern "C" void register_ambigeometry_source(void)
{
	obs_source_info info = {};
	info.id = kSourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	info.get_name = [](void *) {
		return obs_module_text("AudioAmbigeometry");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new AmbigeometrySource(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<AmbigeometrySource *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<AmbigeometrySource *>(data)->Update(settings);
	};
	info.get_defaults = [](obs_data_t *settings) {
		for (const char *name : kAudioSources) {
			obs_data_set_default_string(settings, name, "");
		}
		obs_data_set_default_int(settings, kPattern, static_cast<int>(Pattern::Polygons));
		obs_data_set_default_int(settings, kShapeCount, 6);
		obs_data_set_default_int(settings, kSymmetry, 1);
		obs_data_set_default_int(settings, kTrail, kDefaultTrail);
		obs_data_set_default_double(settings, kHueSpeed, 6.0);
		obs_data_set_default_int(settings, kSaturation, 80);
		obs_data_set_default_int(settings, kWidth, kDefaultWidth);
		obs_data_set_default_int(settings, kHeight, kDefaultHeight);
	};
	info.get_properties = get_properties;
	info.get_width = [](void *data) {
		return static_cast<AmbigeometrySource *>(data)->Width();
	};
	info.get_height = [](void *data) {
		return static_cast<AmbigeometrySource *>(data)->Height();
	};
	info.video_tick = [](void *data, float seconds) {
		static_cast<AmbigeometrySource *>(data)->Tick(seconds);
	};
	info.video_render = [](void *data, gs_effect_t *) {
		static_cast<AmbigeometrySource *>(data)->Render();
	};
	obs_register_source(&info);
}
