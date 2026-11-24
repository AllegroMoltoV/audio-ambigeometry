// OBS Studio の中で動き、導入したプラグインを確かめる検査用のモジュール。
// 環境変数 AMBIGEOMETRY_PROBE_RESULT_DIR・AMBIGEOMETRY_PROBE_PHASE・AMBIGEOMETRY_PROBE_EXPECTED_NAME があるときだけ
// 読み込まれる。
// - setup: 読み込みと登録、リソース、名前、境界値を確かめる。正弦波の WAV を鳴らすメディアソースとこのソースをシーンに
//   置き、音に反応すること、ミュートで透明になること、メディアソースの名前の変更に設定が付いていくことを確かめて保存する。
//   このソースを置く前と後の CPU の使用率も記録する
// - restore: 再起動後に、保存した設定が残り、音声ソースを取り直して再び音に反応することを確かめる
// 結果を result.json に書き、OBS を閉じる。配布物には含めない。

#include "render-fixture.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMainWindow>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <vector>

OBS_DECLARE_MODULE()

namespace {

QString result_dir;
QString phase;
QString expected_name;
QJsonObject evidence;

constexpr const char *kModuleName = "audio-ambigeometry";
constexpr const char *kSourceId = "audio_ambigeometry_source";
constexpr const char *kSourceName = "Ambigeometry install check";
constexpr const char *kToneName = "Probe tone";
constexpr const char *kRenamedToneName = "Probe tone renamed";
constexpr const char *kFinalMix = "\037final-mix";

// 保存して再起動後に確かめる設定。既定値と違う値にする
constexpr long long kPattern = 1;
constexpr long long kSymmetry = 3;
constexpr long long kTrail = 0;
constexpr double kHueSpeed = 12.0;
constexpr long long kSaturation = 50;

QMainWindow *main_window()
{
	return static_cast<QMainWindow *>(obs_frontend_get_main_window());
}

void after(int ms, std::function<void()> step)
{
	QTimer::singleShot(ms, main_window(), std::move(step));
}

void finish(bool passed, const QString &reason)
{
	QJsonObject result{{"passed", passed},
			   {"reason", reason},
			   {"obs", obs_get_version_string()},
			   {"phase", phase},
			   {"evidence", evidence}};
	if (auto *module = obs_get_module(kModuleName))
		result.insert("product_binary_path", QString::fromUtf8(obs_get_module_binary_path(module)));
	QFile file(result_dir + "/result.json");
	const auto json = QJsonDocument(result).toJson();
	if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size() || !file.flush())
		passed = false;
	file.close();
	blog(passed ? LOG_INFO : LOG_ERROR, "AMBIGEOMETRY_PROBE %s phase=%s %s", passed ? "PASS" : "FAIL",
	     phase.toUtf8().constData(), reason.toUtf8().constData());
	auto *window = main_window();
	QTimer::singleShot(0, window, [window] { window->close(); });
}

// 不透明度が 16 を超える画素の数と、不透明度の最大値
struct Coverage {
	bool rendered = false;
	int visible = 0;
	int max_alpha = 0;
};

Coverage measure(obs_source_t *source)
{
	Coverage c;
	render_test::Image image;
	if (!render_test::render(source, image)) {
		return c;
	}
	c.rendered = true;
	for (uint32_t y = 0; y < image.height; ++y) {
		for (uint32_t x = 0; x < image.width; ++x) {
			const int a = image.at(x, y)[3];
			c.visible += a > 16 ? 1 : 0;
			c.max_alpha = std::max(c.max_alpha, a);
		}
	}
	return c;
}

QJsonObject to_json(const Coverage &c)
{
	return QJsonObject{{"rendered", c.rendered}, {"visible_pixels", c.visible}, {"max_alpha", c.max_alpha}};
}

// 440 Hz・振幅 0.5・2 秒のステレオの正弦波を、16 ビットの WAV に書く
bool write_tone(const QString &path)
{
	constexpr uint32_t rate = 48000;
	constexpr uint32_t frames = rate * 2;
	std::vector<int16_t> samples(static_cast<size_t>(frames) * 2);
	for (uint32_t i = 0; i < frames; ++i) {
		const double v = 0.5 * std::sin(2.0 * 3.14159265358979323846 * 440.0 * i / rate);
		const auto s = static_cast<int16_t>(std::lround(v * 32767.0));
		samples[static_cast<size_t>(i) * 2] = s;
		samples[static_cast<size_t>(i) * 2 + 1] = s;
	}
	const uint32_t data_size = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
	QByteArray header;
	const auto put32 = [&header](uint32_t v) {
		for (int i = 0; i < 4; ++i)
			header.append(static_cast<char>((v >> (8 * i)) & 0xFF));
	};
	const auto put16 = [&header](uint16_t v) {
		header.append(static_cast<char>(v & 0xFF));
		header.append(static_cast<char>(v >> 8));
	};
	header.append("RIFF");
	put32(36 + data_size);
	header.append("WAVEfmt ");
	put32(16);
	put16(1);
	put16(2);
	put32(rate);
	put32(rate * 4);
	put16(4);
	put16(16);
	header.append("data");
	put32(data_size);
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly))
		return false;
	file.write(header);
	file.write(reinterpret_cast<const char *>(samples.data()), data_size);
	return file.flush();
}

// 3 秒間の OBS の CPU の使用率 (%)
void measure_cpu(const char *key, std::function<void()> next)
{
	auto *info = os_cpu_usage_info_start();
	os_cpu_usage_info_query(info);
	after(3000, [info, key, next] {
		evidence.insert(key, os_cpu_usage_info_query(info));
		os_cpu_usage_info_destroy(info);
		next();
	});
}

struct BoundaryCase {
	const char *name;
	std::function<void(obs_data_t *)> write;
	uint32_t expected_width;
	uint32_t expected_height;
};

// 範囲の端と範囲外の値で、作成・描画・大きさを確かめる。範囲外の値は範囲に収める
bool check_boundaries()
{
	const BoundaryCase cases[] = {
		{"defaults", [](obs_data_t *) {}, 1280, 720},
		{"pattern-out-of-range", [](obs_data_t *s) { obs_data_set_int(s, "pattern", 99); }, 1280, 720},
		{"pattern-negative", [](obs_data_t *s) { obs_data_set_int(s, "pattern", -1); }, 1280, 720},
		{"shapes-and-symmetry-max",
		 [](obs_data_t *s) {
			 obs_data_set_int(s, "shape_count", 12);
			 obs_data_set_int(s, "symmetry", 8);
		 },
		 1280, 720},
		{"shapes-and-symmetry-out-of-range",
		 [](obs_data_t *s) {
			 obs_data_set_int(s, "shape_count", 1000);
			 obs_data_set_int(s, "symmetry", -3);
		 },
		 1280, 720},
		{"trail-out-of-range", [](obs_data_t *s) { obs_data_set_int(s, "trail", 1000); }, 1280, 720},
		{"hue-and-saturation-out-of-range",
		 [](obs_data_t *s) {
			 obs_data_set_double(s, "hue_speed", -10.0);
			 obs_data_set_int(s, "saturation", 500);
		 },
		 1280, 720},
		{"missing-audio-source",
		 [](obs_data_t *s) { obs_data_set_string(s, "audio_source_1", "No such source"); }, 1280, 720},
		{"size-min",
		 [](obs_data_t *s) {
			 obs_data_set_int(s, "width", 16);
			 obs_data_set_int(s, "height", 16);
		 },
		 16, 16},
		{"size-below-range",
		 [](obs_data_t *s) {
			 obs_data_set_int(s, "width", 0);
			 obs_data_set_int(s, "height", -5);
		 },
		 16, 16},
		{"size-max",
		 [](obs_data_t *s) {
			 obs_data_set_int(s, "width", 4096);
			 obs_data_set_int(s, "height", 4096);
		 },
		 4096, 4096},
		{"size-above-range",
		 [](obs_data_t *s) {
			 obs_data_set_int(s, "width", 99999);
			 obs_data_set_int(s, "height", 99999);
		 },
		 4096, 4096},
	};
	QJsonArray results;
	bool all = true;
	for (const BoundaryCase &c : cases) {
		obs_data_t *settings = obs_data_create();
		c.write(settings);
		obs_source_t *source = obs_source_create_private(kSourceId, c.name, settings);
		obs_data_release(settings);
		render_test::Image image;
		const bool rendered = source && render_test::render(source, image);
		const bool size_ok = rendered && image.width == c.expected_width && image.height == c.expected_height;
		obs_source_release(source);
		results.append(QJsonObject{{"case", c.name},
					   {"rendered", rendered},
					   {"size", QString("%1x%2").arg(image.width).arg(image.height)},
					   {"passed", size_ok}});
		all = all && size_ok;
	}
	evidence.insert("boundaries", results);
	return all;
}

bool add_to_current_scene(obs_source_t *source)
{
	auto *scene_source = obs_frontend_get_current_scene();
	auto *scene = scene_source ? obs_scene_from_source(scene_source) : nullptr;
	const bool attached = source && scene && obs_scene_add(scene, source);
	obs_source_release(scene_source);
	return attached;
}

bool settings_retained(obs_source_t *source)
{
	auto *settings = obs_source_get_settings(source);
	const bool retained = obs_data_get_int(settings, "pattern") == kPattern &&
			      obs_data_get_int(settings, "symmetry") == kSymmetry &&
			      obs_data_get_int(settings, "trail") == kTrail &&
			      obs_data_get_double(settings, "hue_speed") == kHueSpeed &&
			      obs_data_get_int(settings, "saturation") == kSaturation &&
			      std::strcmp(obs_data_get_string(settings, "audio_source_1"), kRenamedToneName) == 0 &&
			      std::strcmp(obs_data_get_string(settings, "audio_source_2"), kFinalMix) == 0;
	QJsonObject saved = QJsonDocument::fromJson(obs_data_get_json(settings)).object();
	// 制御文字で始まる最終ミックスの値は、読みやすいように置き換えて残す
	if (saved.value("audio_source_2").toString() == QString::fromUtf8(kFinalMix))
		saved.insert("audio_source_2", "<final mix>");
	evidence.insert("source_settings", saved);
	obs_data_release(settings);
	return retained;
}

void run_setup()
{
	const QString tone_path = result_dir + "/probe-tone.wav";
	if (!write_tone(tone_path)) {
		finish(false, "failed to write the test tone");
		return;
	}
	obs_data_t *media_settings = obs_data_create();
	obs_data_set_bool(media_settings, "is_local_file", true);
	obs_data_set_string(media_settings, "local_file", tone_path.toUtf8().constData());
	obs_data_set_bool(media_settings, "looping", true);
	obs_source_t *media = obs_source_create("ffmpeg_source", kToneName, media_settings, nullptr);
	obs_data_release(media_settings);
	if (!add_to_current_scene(media)) {
		obs_source_release(media);
		finish(false, "failed to add the media source");
		return;
	}

	// このソースを置く前の CPU の使用率を測ってから置く
	measure_cpu("cpu_percent_before", [media] {
		obs_data_t *settings = obs_data_create();
		obs_data_set_int(settings, "pattern", kPattern);
		obs_data_set_int(settings, "symmetry", kSymmetry);
		obs_data_set_int(settings, "trail", kTrail);
		obs_data_set_double(settings, "hue_speed", kHueSpeed);
		obs_data_set_int(settings, "saturation", kSaturation);
		obs_data_set_string(settings, "audio_source_1", kToneName);
		obs_data_set_string(settings, "audio_source_2", kFinalMix);
		obs_source_t *source = obs_source_create(kSourceId, kSourceName, settings, nullptr);
		obs_data_release(settings);
		if (!add_to_current_scene(source)) {
			obs_source_release(source);
			obs_source_release(media);
			finish(false, "failed to add the source");
			return;
		}
		measure_cpu("cpu_percent_after", [media, source] {
			const Coverage sound = measure(source);
			evidence.insert("with_sound", to_json(sound));
			if (sound.visible <= 100) {
				obs_source_release(source);
				obs_source_release(media);
				finish(false, "the source does not react to the media source");
				return;
			}
			obs_source_set_muted(media, true);
			after(2000, [media, source] {
				const Coverage muted = measure(source);
				evidence.insert("muted", to_json(muted));
				obs_source_set_muted(media, false);
				if (muted.max_alpha != 0) {
					obs_source_release(source);
					obs_source_release(media);
					finish(false, "the source reacts to the muted media source");
					return;
				}
				obs_source_set_name(media, kRenamedToneName);
				after(3000, [media, source] {
					const Coverage renamed = measure(source);
					evidence.insert("renamed", to_json(renamed));
					const bool retained = settings_retained(source);
					obs_source_release(source);
					obs_source_release(media);
					if (!retained || renamed.visible <= 100) {
						finish(false, "the setting does not follow the renamed media source");
						return;
					}
					obs_frontend_save();
					finish(true,
					       "resources, boundaries, sound, mute, rename and saved settings checked");
				});
			});
		});
	});
}

void run_restore()
{
	obs_source_t *source = obs_get_source_by_name(kSourceName);
	if (!source || std::strcmp(obs_source_get_id(source), kSourceId) != 0) {
		obs_source_release(source);
		finish(false, "persisted source missing");
		return;
	}
	if (!settings_retained(source)) {
		obs_source_release(source);
		finish(false, "settings are not retained");
		return;
	}
	// 再起動のあと、名前から音声ソースを取り直して反応するまで待つ
	after(3000, [source] {
		const Coverage sound = measure(source);
		evidence.insert("with_sound", to_json(sound));
		obs_source_release(source);
		finish(sound.visible > 100, "persisted settings and the reaction after restart checked");
	});
}

void inspect()
{
	auto *module = obs_get_module(kModuleName);
	if (!module) {
		finish(false, "product module missing");
		return;
	}
	int registrations = 0;
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_input_types(i, &id); ++i)
		registrations += std::strcmp(id, kSourceId) == 0;
	evidence.insert("source_registrations", registrations);
	if (registrations != 1) {
		finish(false, "source registration count mismatch");
		return;
	}
	char *effect = obs_find_module_file(module, "ambigeometry.effect");
	char *locale = obs_find_module_file(module, "locale/ja-JP.ini");
	char *license = obs_find_module_file(module, "licenses/milkdrop-fft.txt");
	const QString translated = QString::fromUtf8(obs_module_get_locale_text(module, "AudioAmbigeometry"));
	evidence.insert("effect_path", effect ? QString::fromUtf8(effect) : QString());
	evidence.insert("locale_path", locale ? QString::fromUtf8(locale) : QString());
	evidence.insert("license_path", license ? QString::fromUtf8(license) : QString());
	evidence.insert("source_name", translated);
	const bool resources = effect && locale && license && translated == expected_name;
	bfree(effect);
	bfree(locale);
	bfree(license);
	if (!resources) {
		finish(false, "installed effect, license or translated locale unavailable");
		return;
	}
	if (phase == "setup") {
		if (!check_boundaries()) {
			finish(false, "boundary value check failed");
			return;
		}
		run_setup();
	} else {
		run_restore();
	}
}

void frontend_event(obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
		after(1000, inspect);
}

} // namespace

bool obs_module_load()
{
	result_dir = qEnvironmentVariable("AMBIGEOMETRY_PROBE_RESULT_DIR");
	phase = qEnvironmentVariable("AMBIGEOMETRY_PROBE_PHASE");
	expected_name = qEnvironmentVariable("AMBIGEOMETRY_PROBE_EXPECTED_NAME");
	if (result_dir.isEmpty() || (phase != "setup" && phase != "restore") || expected_name.isEmpty())
		return false;
	obs_frontend_add_event_callback(frontend_event, nullptr);
	return true;
}

void obs_module_unload()
{
	obs_frontend_remove_event_callback(frontend_event, nullptr);
}
