// license:BSD-3-Clause
// SWP50 の骨組み（swp50.h）が、doc/motif-rack-es.md に書いた手順どおりに返事をするかを確かめる。
// 本体のプログラムは使わない。文書の「書く順番」を手でなぞるだけ。
//
//   g++ -std=c++17 -O1 -o swp50test src/motif/swp50test.cpp && ./swp50test

#include "swp50.h"
#include "swp50_cutoff.h"

#include <cmath>
#include <cstdio>

static int g_fail = 0;

#define CHECK(cond) do { if (!(cond)) { std::printf("NG  %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

int main()
{
	swp50_device s;
	s.reset();

	// まとめ書き: r | 0x100 へ溜め、0x4A7E で確定、0x187E の bit 0 が 0 になるまで待つ
	s.write(3 << 9 | 0x102, 0x1234);
	s.write(3 << 9 | 0x130, 0x2000);
	CHECK(s.reg(3, 0x02) == 0);
	CHECK(s.pending() == 2);
	s.write(swp50_device::BATCH_COMMIT, 0x8000);
	CHECK((s.read(swp50_device::BATCH_BUSY) & 1) == 0);
	CHECK(s.reg(3, 0x02) == 0x1234);
	CHECK(s.reg(3, 0x30) == 0x2000);
	CHECK(s.pending() == 0);

	// 音程と読み戻し: 0x5C7E に 種類 | スロット、bit 15 を待って 0x5C7F を読む
	s.write(5 << 9 | 0x0a, 0x3c00);                 // -1024 = 1 オクターブ下
	CHECK(s.pitch(5) == -1024);
	s.write(swp50_device::READBACK_CMD, 0x0400 | 5);
	CHECK(s.read(swp50_device::READBACK_CMD) & 0x8000);
	CHECK(s.read(swp50_device::READBACK_DATA) == 0x3c00);

	// 読む速さの天井: 3 倍に届くと 1 オクターブ下へ戻り、何度でもくり返す
	CHECK(swp50_device::fold_pitch(-1024) == -1024);
	CHECK(swp50_device::fold_pitch(1623) == 1623);
	CHECK(swp50_device::fold_pitch(1624) == 600);
	CHECK(swp50_device::fold_pitch(1624 + 2048) == 600);
	CHECK(swp50_device::pitch_to_step(1623) < 3.0 && swp50_device::pitch_to_step(1624) < 1.51);

	// カットオフの目盛りの表（実機の測定）: 60〜222 で増え続け、154 で 585Hz、205 で 2238Hz、223 から上は範囲の外
	for (int c = 60; c < 222; c++)
		CHECK(swp50_cutoff::lpf24d_6db_hz[c + 1] >= swp50_cutoff::lpf24d_6db_hz[c]);
	CHECK(std::fabs(swp50_cutoff::lpf24d_6db_hz[154] - 585.1f) < 0.1f);
	CHECK(std::fabs(swp50_cutoff::lpf24d_6db_hz[205] - 2238.2f) < 0.1f);
	CHECK(swp50_cutoff::lpf24d_6db_hz[223] == 0.0f);

	// 送りの 4 本: r = 0x22 に送り先、r = 0x23 に種類と量
	s.write(5 << 9 | 0x22, 0x1205);
	s.write(5 << 9 | 0x23, 0x0110);
	CHECK(s.send_target(5, 1) == 0x12 && s.send_kind(5, 1) == 1 && s.send_atten(5, 1) == 0x10);

	// 係数: 番号 0x000・0x003・0x1FF・0x200・0x3FF の番地
	CHECK(swp50_device::coef_addr(0x000) == (0x00 << 9 | 0x31));
	CHECK(swp50_device::coef_addr(0x003) == (0x00 << 9 | 0x3d));
	CHECK(swp50_device::coef_addr(0x1ff) == (0x7f << 9 | 0x3d));
	CHECK(swp50_device::coef_addr(0x200) == (0x00 << 9 | 0x33));
	CHECK(swp50_device::coef_addr(0x3ff) == (0x7f << 9 | 0x3f));

	// 32bit の値: 番号 0・1・0xA9・0xAA の番地（上位語）
	CHECK(swp50_device::value32_addr(0x00) == (0x00 << 9 | 0x48));
	CHECK(swp50_device::value32_addr(0x01) == (0x00 << 9 | 0x4c));
	CHECK(swp50_device::value32_addr(0xa9) == (0x54 << 9 | 0x4c));
	CHECK(swp50_device::value32_addr(0xaa) == (0x00 << 9 | 0x4a));
	s.write(swp50_device::value32_addr(7), 0x0012);
	s.write(swp50_device::value32_addr(7) + 1, 0x3456);
	CHECK(s.value32(7) == 0x00123456);

	// 番号つきの書き込み口
	s.write(swp50_device::COEF_INDEXED, 0x123 << 3 | 5);
	s.write(swp50_device::COEF_INDEXED + 1, 0x7fff);
	CHECK(s.coef(0x123) == 0x7fff);

	// 曲線の表の流し込み: 選択 1、番地 0x012345 から 3 語
	s.write(swp50_device::TABLE_ADDR_H, 1 << 7 | 0x01);
	s.write(swp50_device::TABLE_ADDR_L, 0x2345);
	for (uint16_t v : { 0x4000, 0x405b, 0x40b9 }) {
		CHECK((s.read(swp50_device::FX_STATUS) & 0x100) == 0);
		s.write(swp50_device::TABLE_DATA, v);
	}
	CHECK(s.table(1, 0x012345) == 0x4000);
	CHECK(s.table(1, 0x012347) == 0x40b9);
	CHECK(s.table(0, 0x012345) == 0);

	// ブロックを止めるビット: 書いた値がそのまま読める
	s.write(swp50_device::BLOCK_STOP_L, 0x0030);
	CHECK(s.read(swp50_device::BLOCK_STOP_L) == 0x0030);

	// ユニットの旗: 4 語を書いて 0xB87E に 2
	s.write(0xb27f, 0x0005);
	s.write(0xb07e, 0x8000);
	CHECK(!s.unit_enabled(0));
	s.write(swp50_device::UNIT_COMMIT, 2);
	CHECK(s.unit_enabled(0) && !s.unit_enabled(1) && s.unit_enabled(2) && s.unit_enabled(63));

	// ユニット 1 個ぶんの引き金: 0-15 は 0xA27F、48-63 は 0xA07E
	s.write(0xa27f, 1 << 3);
	s.write(0xa07e, 1 << 15);
	CHECK(s.unit_strobes() == (1ull << 3 | 1ull << 63));
	CHECK(s.unit_strobes() == 0);

	// まとめて発音: 0x467F がボイス 0-15、0x447E が 48-63。0x487E に合図
	uint64_t got = 0;
	uint16_t mode = 0;
	s.on_keyon = [&](const uint64_t m[2], uint16_t md) { got = m[0]; mode = md; };
	s.write(0x467f, 0x0001);
	s.write(0x447e, 0x8000);
	s.write(swp50_device::KON_TRIGGER, 0x9000);
	CHECK(got == 0x8000000000000001ull);
	CHECK(mode == 0x9000);

	// 波形メモリを 1 語読む
	s.wave_read = [](uint32_t a) { return uint16_t(a ^ 0xa5a5); };
	s.write(swp50_device::WAVE_ADDR_H, 0x0100);
	s.write(swp50_device::WAVE_ADDR_L, 0x0002);
	s.write(swp50_device::WAVE_CMD, 0xc000);
	CHECK(s.read(swp50_device::WAVE_STATUS) & 0xc000);
	CHECK(s.read(swp50_device::WAVE_DATA) == uint16_t(0x01000002 ^ 0xa5a5));

	// つなぎ替え: 外からの入力を受けるユニットと、チップをまたぐ線の登録
	s.write(8 << 9 | 0x2c, 0x0f80 | 2);
	CHECK(s.unit_source_kind(8) == 0x1f && s.unit_source(8) == 2);
	s.write(8 << 9 | 0x1f, 0x7f40);
	CHECK(s.unit_level_l(8) == 0x7f && s.unit_level_r(8) == 0x40);
	s.write(swp50_device::ABUS_CLEAR, 0);
	s.write(swp50_device::ABUS_SEND, 0x8000 | 1 << 6 | 0x12);
	CHECK(s.abus_sends().size() == 1);
	CHECK(swp50_device::abus_send_line(s.abus_sends()[0]) == 0x12);

	// ボイスを力ずくで止める手順: r = 0x90 に 0x8000、0x587E の bit 15 を待つ
	s.write(0x45 << 9 | 0x90, 0x8000);
	s.write(0x05 << 9 | 0x90, 0x8000);
	CHECK(s.read(swp50_device::TRIGGER_ACK) & 0x8000);

	// 目盛り: 0x10 で半分（6.02dB）、0x20 で 12.04dB（実機で音量 2 → 1 が 12.04dB だった）
	CHECK(std::fabs(swp50_device::atten_to_gain(0x10) - 0.5) < 1e-12);
	CHECK(std::fabs(swp50_device::atten_to_db(0x20) + 12.0412) < 1e-3);
	CHECK(swp50_device::atten_to_gain(0xff) == 0.0);
	// 実機の値: 下げ幅 7 = −2.20dB、15 = −5.69dB、26 = −9.37dB、123 = −45.91dB
	CHECK(std::fabs(20 * std::log10(swp50_device::level_to_gain(7)) + 2.20) < 0.02);
	CHECK(std::fabs(20 * std::log10(swp50_device::level_to_gain(15)) + 5.69) < 0.02);
	CHECK(std::fabs(20 * std::log10(swp50_device::level_to_gain(26)) + 9.37) < 0.02);
	CHECK(std::fabs(20 * std::log10(swp50_device::level_to_gain(123)) + 45.91) < 0.02);
	CHECK(swp50_device::level_to_gain(0x10) == 0.5 && swp50_device::level_to_gain(0xff) == 0.0);
	// 送り: 実機の Dry Level（下げ幅 3 = −0.84dB、6 = −1.80dB、11 = −3.67dB）、パン（17 = −6.30dB）
	CHECK(std::fabs(20 * std::log10(swp50_device::send_to_gain(3)) + 0.84) < 0.03);
	CHECK(std::fabs(20 * std::log10(swp50_device::send_to_gain(6)) + 1.80) < 0.03);
	CHECK(std::fabs(20 * std::log10(swp50_device::send_to_gain(11)) + 3.67) < 0.03);
	CHECK(std::fabs(20 * std::log10(swp50_device::send_to_gain(17)) + 6.30) < 0.03);
	CHECK(swp50_device::send_to_gain(0) == 1.0 && swp50_device::send_to_gain(0x10) == 0.5 && swp50_device::send_to_gain(0xff) == 0.0);
	// 音量の式 = 送りの式に 0.75 目盛りを足したもの（オクターブの中で一致）
	for (uint32_t a = 0; a < 16; a++)
		CHECK(std::fabs(swp50_device::level_to_gain(a) - (32.0 - a - 0.75) / 31.25) < 1e-12);

	// 数え
	const uint16_t c0 = s.read(swp50_device::SAMPLE_COUNT);
	s.tick();
	CHECK(uint16_t(s.read(swp50_device::SAMPLE_COUNT) - c0) == 1);

	std::printf(g_fail ? "swp50test: %d 件の食い違い\n" : "swp50test: OK\n", g_fail);
	return g_fail ? 1 : 0;
}
