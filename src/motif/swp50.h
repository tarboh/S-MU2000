// license:BSD-3-Clause
// SWP50（T8F02TB-0102。MOTIF ES / MOTIF-RACK ES の音源チップ）の骨組み。
// doc/motif-rack-es.md と doc/motif-rack-es-swp50-pins.md を、コードの形に写したもの。
//
// **音は出ない。** 持っているのは、レジスタの受け口と、本体のプログラムが待っている
// 返事（まとめ書きの確定、読み戻し、状態の旗）と、書かれた値を意味ごとに取り出す口だけ。
// 中の計算（波形の読み、補間、EG、エフェクト）は 1 つも入っていない。
//
// 根拠はすべて本体プログラムの静的な読みと回路図で、動かして確かめたものは無い。
// 文書で「らしい」「見立て」と書いた所は、ここでも注釈にそう書いてある。
//
// 番地は 16bit の語番地で受ける（CPU から見た番地 = 土台 + 語番地 × 2）。
//   語番地 = a << 9 | r     a: 7bit   r: 9bit
//   r = 0x7E・0x7F は制御レジスタ（a が番号）、r の bit 8 は「溜めておく側」

#ifndef SMU2000_MOTIF_SWP50_H
#define SMU2000_MOTIF_SWP50_H

#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

class swp50_device
{
public:
	// 制御レジスタの語番地（文書の表と同じ並び）
	enum : uint32_t {
		GLOBAL_MODE   = 0x007e,   // 検査は両方のチップに 0xA0A8 を書く（中身は未読）
		CHIP_ROLE     = 0x027e,   // 検査はマスターに 0x1000、スレーブに 0x2080 を書く（役目の設定と見ている）
		BATCH_BUSY    = 0x187e,   // bit 0 = まとめ書きの処理中
		SAMPLE_COUNT  = 0x1c7e,   // サンプルごとに進む数え（らしい）
		WAVE_CMD      = 0x207e,   // 0xC000 読み / 0x8000 書き / 0xD000 まとめ読み / 0 終わり
		WAVE_ADDR_H   = 0x247e,
		WAVE_ADDR_L   = 0x247f,
		WAVE_STATUS   = 0x287e,   // 0xC000 のどちらかのビットが立つまで待つ（検査の 8C097A84）
		WAVE_DATA     = 0x2e7e,
		KON_MASK      = 0x407e,   // 0x407E-0x467F の 8 語。0x467F = ボイス 0-15、0x467E = 16-31、0x447F = 32-47、0x447E = 48-63
		KON_TRIGGER   = 0x487e,   // 0x8000・0x9000・0x8800（チップの型で変わる）
		TRIGGER_ACK   = 0x587e,   // bit 15 = r | 0x80 の側へ書いた引き金を受け取った（発音開始と、力ずくで止める手順が待つ）
		BATCH_COMMIT  = 0x4a7e,   // 0x8000 = ボイス、0x4000 = エフェクト
		READBACK_CMD  = 0x5c7e,   // 種類 | スロット番号 を書く。読むと bit 15 = 用意ができた
		READBACK_DATA = 0x5c7f,
		ABUS_CLEAR    = 0x7a7e,   // 0 を書く
		ABUS_SEND     = 0x7c7e,   // チップをまたぐ線の登録（見立て: A-Bus へ載せる線）
		TABLE_ADDR_H  = 0x847e,   // 選択 << 7 | 番地の上位
		TABLE_ADDR_L  = 0x847f,
		TABLE_DATA    = 0x867e,   // 1 語書くごとに番地が進む
		BLOCK_STOP_L  = 0x887e,   // ブロックを止めるビット（32 本）
		BLOCK_STOP_H  = 0x887f,
		FX_STATUS     = 0x8a7e,   // bit 8 = 表の流し込みの処理中、bit 4 = 読み戻しの処理中
		COEF_INDEXED  = 0x8e7e,   // (係数の番号 << 3) | 3bit、続けて 0x8E7F に値
		PROBE_SELECT  = 0x967e,   // 中の値の読み戻しの番号
		UNIT_MASK     = 0xb07e,   // 0xB27F・0xB27E・0xB07F・0xB07E の順にユニット 0-15・16-31・32-47・48-63
		UNIT_COMMIT   = 0xb87e,   // 2 を書くと上の 4 語が効く（らしい）
	};

	// 外へ出す口
	std::function<uint16_t(uint32_t)> wave_read;              // 波形メモリの 1 語（16bit の語番地）
	std::function<void(const uint64_t mask[2], uint16_t mode)> on_keyon;   // まとめて発音の合図

	void reset()
	{
		m_reg.assign(0x10000, 0);
		m_pending.clear();
		m_table[0].assign(TABLE_WORDS, 0);
		m_table[1].assign(TABLE_WORDS, 0);
		m_table_pos = 0;
		m_table_sel = 0;
		m_samples = 0;
		m_unit_enable = 0;
		m_unit_strobe = 0;
		m_abus_send.clear();
		m_commits = 0;
	}

	// 1 サンプル進める。数えを進めるだけ
	void tick() { m_samples++; }

	uint16_t read(uint32_t w)
	{
		w &= 0xffff;
		switch (w) {
		case BATCH_BUSY:    return 0;                           // いつも済んでいる
		case SAMPLE_COUNT:  return uint16_t(m_samples);
		case WAVE_STATUS:   return wave_status_ready;
		case TRIGGER_ACK:   return m_reg[w] | 0x8000;           // すぐ受け取ったことにする
		case READBACK_CMD:  return m_reg[w] | 0x8000;           // すぐ用意ができる
		case READBACK_DATA: return readback();
		case FX_STATUS:     return 0;                           // 処理中の旗はいつも落ちている
		case 0x987e: case 0x987f: case 0x9a7e: case 0x9a7f:
			return 0;                                           // 中の値（メーターらしい）。計算が無いので 0
		default:            return m_reg[w];                    // BLOCK_STOP_* も書かれた値がそのまま返る
		}
	}

	void write(uint32_t w, uint16_t v)
	{
		w &= 0xffff;
		const uint32_t r = w & 0x1ff;
		m_reg[w] = v;

		// 溜めておく側（r の bit 8）。確定の合図まで本体へは写さない
		if (r >= 0x100) {
			m_pending.push_back(w);
			return;
		}

		switch (w) {
		case BATCH_COMMIT:
			// ボイス（0x8000）とエフェクト（0x4000）で溜めが別かどうかは分からない。ここでは全部流す
			for (uint32_t p : m_pending)
				m_reg[p & ~0x100u] = m_reg[p];
			m_pending.clear();
			m_commits++;
			break;

		case WAVE_CMD:
			if ((v & 0xf000) == 0xc000 && wave_read)            // 1 語読む
				m_reg[WAVE_DATA] = wave_read(uint32_t(m_reg[WAVE_ADDR_H]) << 16 | m_reg[WAVE_ADDR_L]);
			break;

		case KON_TRIGGER:
			if (on_keyon) {
				// mask[0] = ボイス 0-63。mask[1] は 8 語版（1 ボイス 1 スロットの型）の残り。並びは同じ規則と見ている
				const uint64_t mask[2] = { words64(0x447e), words64(0x407e) };
				on_keyon(mask, v);
			}
			break;

		case ABUS_CLEAR:
			if (v == 0)
				m_abus_send.clear();
			break;

		case ABUS_SEND:
			if (v & 0x8000)
				m_abus_send.push_back(v);
			break;

		case TABLE_ADDR_H:
		case TABLE_ADDR_L:
			m_table_sel = (m_reg[TABLE_ADDR_H] >> 7) & 1;
			m_table_pos = uint32_t(m_reg[TABLE_ADDR_H] & 0x7f) << 16 | m_reg[TABLE_ADDR_L];
			break;

		case TABLE_DATA:
			m_table[m_table_sel][m_table_pos % TABLE_WORDS] = v;
			m_table_pos++;
			break;

		case UNIT_COMMIT:
			if (v == 2)
				m_unit_enable = uint64_t(m_reg[0xb27f]) | uint64_t(m_reg[0xb27e]) << 16
				              | uint64_t(m_reg[0xb07f]) << 32 | uint64_t(m_reg[0xb07e]) << 48;
			break;

		// ユニット 1 個ぶんの引き金。書いたビットを溜めるだけ。
		// 語とユニットの対応は旗と同じ並び（表 8C0EB0A8: 0-15 = 0xA27F、16-31 = 0xA27E、32-47 = 0xA07F、48-63 = 0xA07E）
		case 0xa07e: case 0xa07f: case 0xa27e: case 0xa27f:
			m_unit_strobe |= uint64_t(v) << ((w & 0x200 ? 0 : 32) + (w & 1 ? 0 : 16));
			break;

		case COEF_INDEXED + 1: {
			// 番号つきの書き込み口。下位 3bit の意味は未読なので、値だけ係数へ入れる
			const uint32_t i = m_reg[COEF_INDEXED] >> 3;
			if (i < 0x400)
				m_reg[coef_addr(i)] = v;
			break;
		}

		default:
			break;
		}
	}

	// ---- 目盛り（2026-10-13 に実機の録音で確かめたもの。doc/motif-rack-es-capture.md）----

	// 減らす量（送りの量、EG の高さ）→ 倍率。0x10 = 6.02dB、0 = そのまま、0xFF = 切る
	static double atten_to_gain(uint32_t a) { return a >= 0xff ? 0.0 : std::pow(2.0, -double(a) / 16.0); }
	static double atten_to_db(uint32_t a) { return -double(a) * (20.0 * 0.30102999566398120 / 16.0); }

	// ボイスの音量（r = 0x14 の下のバイト）の倍率。オクターブの中は dB ではなく振幅でまっすぐ下がる。
	// 2026-10-14 に実機の 35 点と合わせたもの（エレメントの音量の 15 点は 0.01dB 以内）。送りの量が同じ式かは未確認
	static double level_to_gain(uint32_t a) { return a >= 0xff ? 0.0 : std::ldexp(1.0 - double(a & 15) / 31.25, -int(a >> 4)); }

	// 送りの量（r = 0x21・0x23・0x25・0x27 の下のバイト）とパン（r = 0x1C）の倍率。同じく振幅でまっすぐだが、分母は 32。
	// 2026-10-18 に Dry Level の 7 点（0.03dB 以内）とパンの 1 点で確かめた。
	// level_to_gain の分母 31.25 は、この式に「0.75 目盛りの下げ」を足したものと同じ（ボイスの音量の道にだけ足されているらしい）
	static double send_to_gain(uint32_t a) { return a >= 0xff ? 0.0 : std::ldexp(double(32 - (a & 15)) / 32.0, -int(a >> 4)); }
	double send_gain(uint32_t n, int k) const { return send_to_gain(send_atten(n, k)); }

	// ---- 書かれた値を、意味ごとに取り出す口 ----

	uint16_t reg(uint32_t a, uint32_t r) const { return m_reg[(a & 0x7f) << 9 | (r & 0x1ff)]; }

	// 係数（16bit × 1024）。番号 → 語番地
	static uint32_t coef_addr(uint32_t i)
	{
		return i < 0x200 ? (i >> 2) << 9 | (0x31 + (i & 3) * 4)
		                 : ((i - 0x200) >> 2) << 9 | (0x33 + (i & 3) * 4);
	}
	uint16_t coef(uint32_t i) const { return m_reg[coef_addr(i)]; }

	// 32bit の値（340 個）。番号は「係数の番号 ÷ 3」。上位語の番地を返す（下位はその次）
	static uint32_t value32_addr(uint32_t j)
	{
		return j < 0xaa ? (j >> 1) << 9 | (0x48 + (j & 1) * 4)
		                : ((j - 0xaa) >> 1) << 9 | (0x4a + (j & 1) * 4);
	}
	uint32_t value32(uint32_t j) const
	{
		const uint32_t w = value32_addr(j);
		return uint32_t(m_reg[w]) << 16 | m_reg[w + 1];
	}

	// ボイス（スロット n = 0-63。相棒は n + 0x40）
	int      pitch(uint32_t n) const                // 1 オクターブ = 1024、符号つき 14bit
	{
		const int v = reg(n, 0x0a) & 0x3fff;
		return v & 0x2000 ? v - 0x4000 : v;
	}
	// 読む速さが 3 倍（サンプル 3 つ / 出力 1 つ）に届くと、1 オクターブ下へ戻る。何オクターブ上でもくり返す。
	// 実機の下録りで、折り返しのある 1 区間の波形 289 本中 288 本と合う（2026-10-18、doc/motif-rack-es-capture.md）。
	// 境目は 1622〜1631 目盛りの間までしか決まっていないので、「3 倍以上」= 1624 と置く
	static constexpr int PITCH_FOLD = 1624;
	static int fold_pitch(int p) { while (p >= PITCH_FOLD) p -= 1024; return p; }
	static double pitch_to_step(int p) { return std::exp2(fold_pitch(p) / 1024.0); }
	uint32_t wave_address(uint32_t n, int k) const  // k = 0-2。r = 0x16/17・0x18/19・0x1A/1B（上位語の上 8bit は別の旗）
	{
		const uint32_t r = 0x16 + k * 2;
		return uint32_t(reg(n, r) & 0xff) << 16 | reg(n, r + 1);
	}
	// 送りの 4 本（k = 0-3）。量は減らす量で、0x10 が 6dB、0 = そのまま、0xFF = 切る
	uint32_t send_target(uint32_t n, int k) const { return reg(n, 0x20 + k * 2) >> 8; }
	uint32_t send_kind(uint32_t n, int k) const { return (reg(n, 0x21 + k * 2) >> 8) & 3; }
	uint32_t send_atten(uint32_t n, int k) const { return reg(n, 0x21 + k * 2) & 0xff; }
	// EG（k = 0-3: アタック・ディケイ 1・ディケイ 2・リリース）。速さは 7bit の番号で、8 目盛りで時間が半分（表 8C0DDA08）
	uint32_t eg_rate(uint32_t n, int k) const { return (reg(n, 0x11 + k) >> 8) & 0x7f; }
	uint32_t eg_level(uint32_t n, int k) const { return reg(n, 0x11 + k) & 0xff; }   // 減らす量。0 = いちばん大きい、0xFF = 無音
	bool     released(uint32_t n) const { return reg(n, 0x14) & 0x8000; }   // リリースへ進む合図

	// つなぎ替え
	bool     unit_enabled(uint32_t u) const { return (m_unit_enable >> (u & 63)) & 1; }
	uint64_t unit_strobes() { const uint64_t s = m_unit_strobe; m_unit_strobe = 0; return s; }
	uint32_t unit_source_kind(uint32_t u) const { return (reg(u, 0x2c) >> 7) & 0x1f; }   // 0x1F = 外からの入力（MEL）
	uint32_t unit_source(uint32_t u) const { return reg(u, 0x2c) & 0x3f; }
	uint32_t unit_level_l(uint32_t u) const { return reg(u, 0x1f) >> 8; }
	uint32_t unit_level_r(uint32_t u) const { return reg(u, 0x1f) & 0xff; }
	uint16_t unit_osc_rate(uint32_t u) const { return reg(u, 0x58); }                    // ユニットの発振器（検査の領域の音出しの関数が使う。目盛りは未確認）
	uint16_t unit_osc_shape(uint32_t u) const { return reg(u, 0x59); }
	uint32_t output_line(uint32_t u) const { return reg(u, 0x2d) & 0x3f; }               // ユニット 0-5 が出口 3 組（見立て）
	const std::vector<uint16_t> &abus_sends() const { return m_abus_send; }
	static uint32_t abus_send_line(uint16_t e) { return e & 0x3f; }
	static uint32_t abus_send_serial(uint16_t e) { return (e >> 6) & 0xf; }

	// 曲線の表のメモリ（選択 0・1）
	uint16_t table(int sel, uint32_t addr) const { return m_table[sel & 1][addr % TABLE_WORDS]; }

	uint32_t commits() const { return m_commits; }
	size_t   pending() const { return m_pending.size(); }

	// 波形メモリの状態で返す値。本体は 0xC000 のどちらかが立つまで待つので、いつも立てておく
	uint16_t wave_status_ready = 0xc000;

private:
	// エフェクト用の SDRAM は 64Mbit を 16bit 幅で使う = 4M 語
	static constexpr uint32_t TABLE_WORDS = 0x400000;

	uint64_t words64(uint32_t w) const   // w・w+1・w+0x200・w+0x201 の 4 語（先の語ほど上位）
	{
		return uint64_t(m_reg[w]) << 48 | uint64_t(m_reg[w + 1]) << 32
		     | uint64_t(m_reg[w + 0x200]) << 16 | uint64_t(m_reg[w + 0x201]);
	}

	uint16_t readback() const
	{
		const uint16_t req = m_reg[READBACK_CMD];
		if ((req & 0x7f00) == 0x0400)                          // 今の音程。計算が無いので書かれた値を返す
			return reg(req & 0x7f, 0x0a) & 0x3fff;
		return 0;                                              // EG の今の高さなど。未実装
	}

	std::vector<uint16_t> m_reg = std::vector<uint16_t>(0x10000);
	std::vector<uint32_t> m_pending;
	std::vector<uint16_t> m_table[2] = { std::vector<uint16_t>(TABLE_WORDS), std::vector<uint16_t>(TABLE_WORDS) };
	std::vector<uint16_t> m_abus_send;
	uint32_t m_table_pos = 0;
	int      m_table_sel = 0;
	uint64_t m_samples = 0;
	uint64_t m_unit_enable = 0;
	uint64_t m_unit_strobe = 0;
	uint32_t m_commits = 0;
};

#endif
