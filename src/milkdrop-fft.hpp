/*
  LICENSE
  -------
Copyright 2005-2013 Nullsoft, Inc.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

  * Redistributions of source code must retain the above copyright notice,
    this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.

  * Neither the name of Nullsoft nor the names of its contributors may be used to
    endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// projectM の src/libprojectM/Audio/MilkdropFFT.hpp (https://github.com/projectM-visualizer/projectm) から写した。
// 変えたのは、名前空間、エクスポートの指定を外したこと、毎回の確保を避ける作業用の配列を持たせたことだけである

#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace ambigeometry {

// 音のサンプルを高速フーリエ変換し、周波数ごとの大きさを返す。低音と高音の大きさをおおまかにそろえる等化と、
// 矩形の窓の周波数特性を抑える窓を掛ける
class MilkdropFFT {
public:
	// samplesIn: 変換に使うサンプルの数。samplesOut: 出力する周波数の数で、2 の累乗にする。
	// 変換の長さは samplesOut の 2 倍で、出力の i 番目はサンプリング周波数 x i / (2 x samplesOut) を表す
	MilkdropFFT(size_t samplesIn, size_t samplesOut, bool equalize = true, float envelopePower = 1.0f);

	// waveformData は samplesIn 個以上を渡す。spectralData は samplesOut 個になる
	void TimeToFrequencyDomain(const std::vector<float> &waveformData, std::vector<float> &spectralData);

	size_t NumFrequencies() const { return m_numFrequencies; }

private:
	void InitEnvelopeTable(float power);
	void InitEqualizeTable(bool equalize);
	void InitBitRevTable();
	void InitCosSinTable();

	size_t m_samplesIn{};
	size_t m_numFrequencies{};

	std::vector<size_t> m_bitRevTable;
	std::vector<float> m_envelope;
	std::vector<float> m_equalize;
	std::vector<std::complex<float>> m_cosSinTable;
	std::vector<std::complex<float>> m_work;
};

} // namespace ambigeometry
