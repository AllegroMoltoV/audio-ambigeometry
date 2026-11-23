// Audio Ambigeometry の描画テスト。
// libobs を画面なしで起動し、試験用の音声ソースに決まった波形を流してから、出力の画素を読み戻して確かめる。
// レンダラーごとの丸めの差を吸収するため、画像の完全一致ではなく許容差つきの性質で比べる。
//
// 使い方: render-test <plugin module> <plugin data folder> <graphics module> <libobs data folder> <scenario>
// 終了コード: すべての検査が通れば 0、検査に落ちれば 1、テストを始められなければ 2

#include "audio-fixture.hpp"
#include "render-fixture.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

namespace {

using namespace render_test;
using namespace audio_test;

constexpr const char *kSourceId = "audio_ambigeometry_source";
constexpr const char *kToneName = "tone";

constexpr uint32_t kCanvasWidth = 160;
constexpr uint32_t kCanvasHeight = 120;
constexpr uint32_t kSourceWidth = 160;
constexpr uint32_t kSourceHeight = 120;

using SettingsWriter = std::function<void(obs_data_t *)>;

obs_source_t *create_visualizer(const SettingsWriter &write)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "audio_source", kToneName);
	obs_data_set_int(settings, "trail", 0);
	obs_data_set_int(settings, "width", kSourceWidth);
	obs_data_set_int(settings, "height", kSourceHeight);
	if (write) {
		write(settings);
	}
	obs_source_t *source = obs_source_create(kSourceId, "visualizer", settings, nullptr);
	obs_data_release(settings);
	return source;
}

// 不透明度が threshold を超える画素の数
int count_visible(const Image &image, int threshold = 16)
{
	int count = 0;
	for (uint32_t y = 0; y < image.height; ++y) {
		for (uint32_t x = 0; x < image.width; ++x) {
			count += image.at(x, y)[3] > threshold ? 1 : 0;
		}
	}
	return count;
}

int max_alpha(const Image &image)
{
	int value = 0;
	for (uint32_t y = 0; y < image.height; ++y) {
		for (uint32_t x = 0; x < image.width; ++x) {
			value = std::max<int>(value, image.at(x, y)[3]);
		}
	}
	return value;
}

Signal tone(double amplitude = 0.5)
{
	Signal s;
	s.left = sine;
	s.frequency = 220.0;
	s.amplitude = amplitude;
	return s;
}

// 音が届くと線が描かれる
void scenario_sound(Checker &c)
{
	obs_source_t *audio = create_audio_source(kToneName);
	obs_source_t *source = create_visualizer(nullptr);
	uint64_t position = 0;
	Image image;
	const bool ok = pump(audio, tone(), position, 40) && render(source, image);
	c.expect(ok, "failed to feed audio or render");
	if (ok) {
		save_image("sound", image);
		const int visible = count_visible(image);
		c.expect(visible > 100, "too few visible pixels with sound: " + std::to_string(visible));
	}
	obs_source_release(source);
	obs_source_release(audio);
}

// 音が無いと、図形は透明になる
void scenario_silence(Checker &c)
{
	obs_source_t *audio = create_audio_source(kToneName);
	obs_source_t *source = create_visualizer(nullptr);
	Image image;
	const bool ok = wait_frames(30) && render(source, image);
	c.expect(ok, "failed to render");
	if (ok) {
		save_image("silence", image);
		c.expect(max_alpha(image) == 0, "silence is not transparent: max alpha " + std::to_string(max_alpha(image)));
	}
	obs_source_release(source);
	obs_source_release(audio);
}

struct Scenario {
	const char *name;
	void (*run)(Checker &);
};

const Scenario scenarios[] = {
	{"sound", scenario_sound},
	{"silence", scenario_silence},
};

} // namespace

int main(int argc, char **argv)
{
	if (argc != 6) {
		std::cerr << "usage: render-test <plugin> <plugin data> <graphics module> <libobs data> <scenario>\n";
		return 2;
	}
	const Scenario *scenario = nullptr;
	for (const Scenario &s : scenarios) {
		if (std::strcmp(argv[5], s.name) == 0) {
			scenario = &s;
		}
	}
	if (!scenario) {
		std::cerr << "unknown scenario: " << argv[5] << '\n';
		return 2;
	}
	if (!obs_test::start(argv[3], argv[4], kCanvasWidth, kCanvasHeight)) {
		return 2;
	}
	if (!start_audio()) {
		obs_shutdown();
		return 2;
	}
	register_test_source();
	register_audio_source();
	int result = 2;
	if (obs_test::load_plugin(argv[1], argv[2])) {
		Checker checker;
		scenario->run(checker);
		result = checker.failures == 0 ? 0 : 1;
		if (result != 0) {
			std::cerr << checker.failures << " check(s) failed in " << scenario->name << '\n';
		} else {
			std::cout << "PASS " << scenario->name << '\n';
		}
	}
	obs_shutdown();
	return result;
}
