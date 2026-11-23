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

#include <obs-module.h>

#include <cstdint>

namespace {

constexpr const char *kSourceId = "audio_ambigeometry_source";
constexpr uint32_t kDefaultWidth = 1280;
constexpr uint32_t kDefaultHeight = 720;

struct AmbigeometrySource {
	obs_source_t *source = nullptr;
	uint32_t width = kDefaultWidth;
	uint32_t height = kDefaultHeight;
};

void update(void *data, obs_data_t *settings)
{
	auto *s = static_cast<AmbigeometrySource *>(data);
	s->width = static_cast<uint32_t>(obs_data_get_int(settings, "width"));
	s->height = static_cast<uint32_t>(obs_data_get_int(settings, "height"));
}

} // namespace

extern "C" void register_ambigeometry_source(void)
{
	obs_source_info info = {};
	info.id = kSourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = [](void *) {
		return obs_module_text("AudioAmbigeometry");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		auto *s = new AmbigeometrySource;
		s->source = source;
		update(s, settings);
		return s;
	};
	info.destroy = [](void *data) {
		delete static_cast<AmbigeometrySource *>(data);
	};
	info.update = update;
	info.get_defaults = [](obs_data_t *settings) {
		obs_data_set_default_int(settings, "width", kDefaultWidth);
		obs_data_set_default_int(settings, "height", kDefaultHeight);
	};
	info.get_properties = [](void *) {
		obs_properties_t *props = obs_properties_create();
		obs_properties_add_int(props, "width", obs_module_text("Width"), 16, 4096, 1);
		obs_properties_add_int(props, "height", obs_module_text("Height"), 16, 4096, 1);
		return props;
	};
	info.get_width = [](void *data) {
		return static_cast<AmbigeometrySource *>(data)->width;
	};
	info.get_height = [](void *data) {
		return static_cast<AmbigeometrySource *>(data)->height;
	};
	info.video_render = [](void *, gs_effect_t *) {};
	obs_register_source(&info);
}
