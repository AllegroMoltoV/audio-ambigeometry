#pragma once

// simple-spectrum-audio (MIT、AllegroMoltoV) の src/sample-ring.h をもとに、左右の値を持つ形にした

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ambigeometry {

struct StereoSample {
	float left = 0.0f;
	float right = 0.0f;
};

// 音声のスレッドから描画のスレッドへ、ロックなしでサンプルを渡す。書き手は待たずに古いサンプルを上書きし、
// 読み手は新しいものだけを読む。読んでいる間に上書きされた可能性のある読み取りは捨てる。
// 書き手は常に 1 つにする。音声ソースを切り替えるときは、古いソースの callback を外してから新しいものを付ける
class SampleRing {
public:
	static constexpr size_t kCapacity = size_t(1) << 14;

	// 音声のスレッド。right が無いモノラルのソースでは左右に同じ値を書く。left が無いときは 0 を書く
	void write(const float *left, const float *right, uint32_t frames)
	{
		uint64_t w = written_.load(std::memory_order_relaxed);
		for (uint32_t i = 0; i < frames; ++i, ++w) {
			StereoSample &s = buffer_[size_t(w) & (kCapacity - 1)];
			s.left = left ? left[i] : 0.0f;
			s.right = right ? right[i] : s.left;
		}
		written_.store(w, std::memory_order_release);
	}

	// 描画のスレッド。cursor より後に書かれたサンプルのうち、新しいものから最大 maxSamples 個を写し、cursor を進める。
	// 写した数を返す。maxSamples は kCapacity / 2 以下にする
	size_t read(uint64_t &cursor, StereoSample *out, size_t maxSamples) const
	{
		const uint64_t w = written_.load(std::memory_order_acquire);
		if (w <= cursor) {
			cursor = w;
			return 0;
		}
		const uint64_t start = w - cursor > maxSamples ? w - maxSamples : cursor;
		const size_t count = size_t(w - start);
		for (size_t i = 0; i < count; ++i)
			out[i] = buffer_[size_t(start + i) & (kCapacity - 1)];
		cursor = w;
		if (written_.load(std::memory_order_acquire) - start > kCapacity)
			return 0;
		return count;
	}

private:
	std::array<StereoSample, kCapacity> buffer_{};
	std::atomic<uint64_t> written_{0};
};

} // namespace ambigeometry
