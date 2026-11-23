// Audio Ambigeometry の描画テスト。
// libobs を画面なしで起動し、試験用の音声ソースに決まった波形を流してから、出力の画素を読み戻して確かめる。
// レンダラーごとの丸めの差を吸収するため、画像の完全一致ではなく許容差つきの性質で比べる。
//
// 使い方: render-test <plugin module> <plugin data folder> <graphics module> <libobs data folder> <scenario>
// 終了コード: すべての検査が通れば 0、検査に落ちれば 1、テストを始められなければ 2

#include "audio-fixture.hpp"
#include "render-fixture.hpp"

#include <util/base.h>

#include <algorithm>
#include <cstdarg>
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
	obs_data_set_string(settings, "audio_source_1", kToneName);
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
		c.expect(max_alpha(image) == 0,
			 "silence is not transparent: max alpha " + std::to_string(max_alpha(image)));
	}
	obs_source_release(source);
	obs_source_release(audio);
}

// 音を流してから描いた画像
bool render_with_sound(const SettingsWriter &write, Image &image, int frames = 40, double amplitude = 0.5)
{
	obs_source_t *audio = create_audio_source(kToneName);
	obs_source_t *source = create_visualizer(write);
	uint64_t position = 0;
	const bool ok = pump(audio, tone(amplitude), position, frames) && render(source, image);
	obs_source_release(source);
	obs_source_release(audio);
	return ok;
}

// 一方では見え、他方では見えない画素の数
int count_difference(const Image &a, const Image &b)
{
	int count = 0;
	for (uint32_t y = 0; y < a.height; ++y) {
		for (uint32_t x = 0; x < a.width; ++x) {
			count += (a.at(x, y)[3] > 16) != (b.at(x, y)[3] > 16) ? 1 : 0;
		}
	}
	return count;
}

// 3 つのパターンは、同じ音でも違う画像になる
void scenario_patterns(Checker &c)
{
	Image images[3];
	for (int p = 0; p < 3; ++p) {
		const bool ok = render_with_sound([p](obs_data_t *s) { obs_data_set_int(s, "pattern", p); }, images[p]);
		c.expect(ok, "failed to render pattern " + std::to_string(p));
		if (!ok) {
			return;
		}
		save_image("pattern-" + std::to_string(p), images[p]);
		c.expect(count_visible(images[p]) > 100, "pattern " + std::to_string(p) + " draws too few pixels");
	}
	for (int p = 0; p < 3; ++p) {
		const int q = (p + 1) % 3;
		const int diff = count_difference(images[p], images[q]);
		c.expect(diff > 100, "patterns " + std::to_string(p) + " and " + std::to_string(q) +
					     " look the same: " + std::to_string(diff));
	}
}

// 中心について点対称な画素の割合。見える画素のうち、中心の反対側の 3x3 の近くも見えるものの割合
double point_symmetry(const Image &image)
{
	int visible = 0;
	int matched = 0;
	for (uint32_t y = 1; y + 1 < image.height; ++y) {
		for (uint32_t x = 1; x + 1 < image.width; ++x) {
			if (image.at(x, y)[3] <= 16) {
				continue;
			}
			++visible;
			const uint32_t mx = image.width - 1 - x;
			const uint32_t my = image.height - 1 - y;
			bool found = false;
			for (int dy = -1; dy <= 1 && !found; ++dy) {
				for (int dx = -1; dx <= 1 && !found; ++dx) {
					found = image.at(mx + dx, my + dy)[3] > 16;
				}
			}
			matched += found ? 1 : 0;
		}
	}
	return visible > 0 ? static_cast<double>(matched) / visible : 0.0;
}

// 対称の数 2 では点対称になる。対称の数 1 では点対称にならない (対照)
void scenario_symmetry(Checker &c)
{
	Image one;
	Image two;
	const bool ok = render_with_sound([](obs_data_t *s) { obs_data_set_int(s, "symmetry", 1); }, one) &&
			render_with_sound([](obs_data_t *s) { obs_data_set_int(s, "symmetry", 2); }, two);
	c.expect(ok, "failed to render");
	if (!ok) {
		return;
	}
	save_image("symmetry-1", one);
	save_image("symmetry-2", two);
	const double s1 = point_symmetry(one);
	const double s2 = point_symmetry(two);
	std::cout << "point symmetry: 1 -> " << s1 << ", 2 -> " << s2 << '\n';
	c.expect(s2 > 0.95, "symmetry 2 is not point symmetric: " + std::to_string(s2));
	c.expect(s1 < 0.8, "symmetry 1 is point symmetric, so the check cannot tell: " + std::to_string(s1));
}

// OBS が表示中のソースを毎フレーム描くのと同じく、音を流しながら毎フレーム描く。残像は描くたびに進む
bool pump_rendering(obs_source_t *audio, obs_source_t *source, const Signal &signal, uint64_t &position, int count,
		    Image &last)
{
	for (int i = 0; i < count; ++i) {
		if (!pump(audio, signal, position, 1) || !render(source, last)) {
			return false;
		}
	}
	return true;
}

// 残像があると、音が止まったあとも少しの間は線が残り、やがて完全に透明になる
void scenario_trail(Checker &c)
{
	obs_source_t *audio = create_audio_source(kToneName);
	obs_source_t *source = create_visualizer([](obs_data_t *s) { obs_data_set_int(s, "trail", 90); });
	uint64_t position = 0;
	Image sounding;
	Image after;
	Image faded;
	Signal quiet;
	quiet.left = silent;
	const bool ok = pump_rendering(audio, source, tone(), position, 40, sounding) &&
			pump_rendering(audio, source, quiet, position, 3, after) &&
			pump_rendering(audio, source, quiet, position, 120, faded);
	c.expect(ok, "failed to feed audio or render");
	if (ok) {
		save_image("trail-sounding", sounding);
		save_image("trail-after", after);
		save_image("trail-faded", faded);
		c.expect(count_visible(sounding) > 100, "trail draws too few pixels with sound");
		c.expect(count_visible(after, 4) > 0, "trail disappears right after the sound stops");
		c.expect(max_alpha(faded) == 0,
			 "trail does not fade to transparent: max alpha " + std::to_string(max_alpha(faded)));
	}
	obs_source_release(source);
	obs_source_release(audio);
}

// ミュートした音声ソースの音には反応しない
void scenario_muted(Checker &c)
{
	obs_source_t *audio = create_audio_source(kToneName);
	obs_source_set_muted(audio, true);
	obs_source_t *source = create_visualizer(nullptr);
	uint64_t position = 0;
	Image image;
	const bool ok = pump(audio, tone(), position, 40) && render(source, image);
	c.expect(ok, "failed to feed audio or render");
	if (ok) {
		save_image("muted", image);
		c.expect(max_alpha(image) == 0, "muted audio is drawn: max alpha " + std::to_string(max_alpha(image)));
	}
	obs_source_release(source);
	obs_source_release(audio);
}

// 音声ソースの名前を変えても、設定が新しい名前に付いていき、音に反応し続ける
void scenario_rename(Checker &c)
{
	obs_source_t *audio = create_audio_source(kToneName);
	obs_source_t *source = create_visualizer(nullptr);
	uint64_t position = 0;
	Image image;
	bool ok = pump(audio, tone(), position, 10);
	obs_source_set_name(audio, "renamed tone");
	ok = ok && pump(audio, tone(), position, 40) && render(source, image);
	c.expect(ok, "failed to feed audio or render");
	if (ok) {
		obs_data_t *settings = obs_source_get_settings(source);
		const std::string name = obs_data_get_string(settings, "audio_source_1");
		obs_data_release(settings);
		c.expect(name == "renamed tone", "the setting does not follow the new name: " + name);
		c.expect(count_visible(image) > 100, "the renamed source is not drawn");
	}
	obs_source_release(source);
	obs_source_release(audio);
}

// 選んだ音声ソースが後から作られても、名前から引き直して音に反応する。シーンコレクションの読み込みの順番に当たる
void scenario_late_source(Checker &c)
{
	obs_source_t *source = create_visualizer(nullptr);
	bool ok = wait_frames(10);
	obs_source_t *audio = create_audio_source(kToneName);
	uint64_t position = 0;
	Image image;
	// 引き直しは 1 秒ごとのため、それより長く流す
	ok = ok && pump(audio, tone(), position, 100) && render(source, image);
	c.expect(ok, "failed to feed audio or render");
	if (ok) {
		save_image("late-source", image);
		c.expect(count_visible(image) > 100, "a source created later is not drawn");
	}
	obs_source_release(source);
	obs_source_release(audio);
}

int g_log_errors = 0;
log_handler_t g_previous_log_handler = nullptr;
void *g_previous_log_param = nullptr;

void count_log_errors(int level, const char *format, va_list args, void *)
{
	if (level == LOG_ERROR) {
		++g_log_errors;
	}
	if (g_previous_log_handler) {
		g_previous_log_handler(level, format, args, g_previous_log_param);
	}
}

// 頂点の最も多い設定でも描け、libobs がエラーを記録しない。即時描画の 512 頂点の上限を超えないことを確かめる
void scenario_max_settings(Checker &c)
{
	const int before = g_log_errors;
	for (int p = 0; p < 3; ++p) {
		Image image;
		const bool ok = render_with_sound(
			[p](obs_data_t *s) {
				obs_data_set_int(s, "pattern", p);
				obs_data_set_int(s, "shape_count", 12);
				obs_data_set_int(s, "symmetry", 8);
				obs_data_set_int(s, "trail", 98);
			},
			image, 20, 0.9);
		c.expect(ok, "failed to render pattern " + std::to_string(p));
		if (ok) {
			save_image("max-" + std::to_string(p), image);
			c.expect(count_visible(image) > 100, "pattern " + std::to_string(p) + " draws too few pixels");
		}
	}
	c.expect(g_log_errors == before, "libobs logged " + std::to_string(g_log_errors - before) + " error(s)");
}

Signal quiet_signal()
{
	Signal s;
	s.left = silent;
	return s;
}

// 2 つの欄の音を足し合わせる。欄 1 が無音でも、欄 2 の音に反応する
void scenario_two_inputs(Checker &c)
{
	obs_source_t *first = create_audio_source("first");
	obs_source_t *second = create_audio_source("second");
	obs_source_t *source = create_visualizer([](obs_data_t *s) {
		obs_data_set_string(s, "audio_source_1", "first");
		obs_data_set_string(s, "audio_source_2", "second");
	});
	std::vector<Feed> feeds = {Feed{first, quiet_signal()}, Feed{second, tone()}};
	Image image;
	const bool ok = pump_all(feeds, 40) && render(source, image);
	c.expect(ok, "failed to feed audio or render");
	if (ok) {
		save_image("two-inputs", image);
		c.expect(count_visible(image) > 100, "the second input is not drawn");
	}
	obs_source_release(source);
	obs_source_release(second);
	obs_source_release(first);
}

// 片方の欄のソースをミュートしても、もう片方の音に反応する。両方をミュートすると透明になる
void scenario_mute_one(Checker &c)
{
	obs_source_t *first = create_audio_source("first");
	obs_source_t *second = create_audio_source("second");
	obs_source_t *source = create_visualizer([](obs_data_t *s) {
		obs_data_set_string(s, "audio_source_1", "first");
		obs_data_set_string(s, "audio_source_2", "second");
	});
	obs_source_set_muted(first, true);
	std::vector<Feed> feeds = {Feed{first, tone()}, Feed{second, tone()}};
	Image one_muted;
	Image both_muted;
	bool ok = pump_all(feeds, 40) && render(source, one_muted);
	obs_source_set_muted(second, true);
	ok = ok && pump_all(feeds, 40) && render(source, both_muted);
	c.expect(ok, "failed to feed audio or render");
	if (ok) {
		save_image("mute-one", one_muted);
		save_image("mute-both", both_muted);
		c.expect(count_visible(one_muted) > 100, "muting one input hides the other");
		c.expect(max_alpha(both_muted) == 0, "muting both inputs is not transparent");
	}
	obs_source_release(source);
	obs_source_release(second);
	obs_source_release(first);
}

// フェーダーを 0 にした音声ソースには反応しない。受け取る音はフェーダーを掛ける前の値のため、本体が音量を掛ける
void scenario_volume_zero(Checker &c)
{
	obs_source_t *audio = create_audio_source(kToneName);
	obs_source_set_volume(audio, 0.0f);
	obs_source_t *source = create_visualizer(nullptr);
	uint64_t position = 0;
	Image image;
	const bool ok = pump(audio, tone(), position, 40) && render(source, image);
	c.expect(ok, "failed to feed audio or render");
	if (ok) {
		save_image("volume-zero", image);
		c.expect(max_alpha(image) == 0,
			 "a source at zero volume is drawn: max alpha " + std::to_string(max_alpha(image)));
	}
	obs_source_release(source);
	obs_source_release(audio);
}

// 最終ミックスの欄は、出力に流れる音に反応する。音声ソースを出力のチャンネルに置いて、最終ミックスに入れる
void scenario_final_mix(Checker &c)
{
	obs_source_t *audio = create_audio_source(kToneName);
	obs_set_output_source(1, audio);
	// 本体の設定で最終ミックスを表す値 (src/audio-input.hpp の kFinalMix)
	obs_source_t *source =
		create_visualizer([](obs_data_t *s) { obs_data_set_string(s, "audio_source_1", "\037final-mix"); });
	uint64_t position = 0;
	Image sounding;
	Image quiet;
	bool ok = pump(audio, tone(), position, 90) && render(source, sounding);
	ok = ok && pump(audio, quiet_signal(), position, 90) && render(source, quiet);
	c.expect(ok, "failed to feed audio or render");
	if (ok) {
		save_image("final-mix", sounding);
		c.expect(count_visible(sounding) > 100, "the final mix is not drawn");
		c.expect(max_alpha(quiet) == 0, "the final mix is drawn after the sound stops");
	}
	obs_set_output_source(1, nullptr);
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
	{"patterns", scenario_patterns},
	{"symmetry", scenario_symmetry},
	{"trail", scenario_trail},
	{"muted", scenario_muted},
	{"rename", scenario_rename},
	{"late-source", scenario_late_source},
	{"max-settings", scenario_max_settings},
	{"two-inputs", scenario_two_inputs},
	{"mute-one", scenario_mute_one},
	{"volume-zero", scenario_volume_zero},
	{"final-mix", scenario_final_mix},
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
	base_get_log_handler(&g_previous_log_handler, &g_previous_log_param);
	base_set_log_handler(count_log_errors, nullptr);
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
