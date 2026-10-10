// license:BSD-3-Clause

#include "compat/cli_text.h"
#include "smartmedia.h"
#include "state.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace smu2000 {

namespace {

constexpr u8 MAKER_TOSHIBA = 0x98;

// 名前は UTF-8。Windows の fopen は ANSI のコードページで読むので、日本語の名前は wide で開く
std::FILE *open_file(const std::string &path, const char *mode)
{
#ifdef _WIN32
	const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
	if (n > 0) {
		std::wstring w(size_t(n), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), n);
		std::wstring m;
		for (const char *c = mode; *c; c++)
			m += wchar_t(*c);
		return _wfopen(w.c_str(), m.c_str());
	}
#endif
	return std::fopen(path.c_str(), mode);
}

// 3.3V の SmartMedia の名乗りの装置番号
u8 device_code_for(u32 megabytes)
{
	switch (megabytes) {
	case 16:  return 0x73;
	case 32:  return 0x75;
	case 64:  return 0x76;
	case 128: return 0x79;
	default:  return 0;
	}
}

// SmartMedia の ECC（256 バイトに 3 バイト、22bit のハミング符号）。
// 列の偶奇 6bit と、奇数個の 1 を持つバイトの位置の排他的論理和から作り、反転して入れる
void ecc256(const u8 *d, u8 out[3])
{
	u8 reg1 = 0, reg2 = 0, reg3 = 0;
	for (int j = 0; j < 256; j++) {
		const u8 b = d[j];
		auto bit = [b](int k) { return (b >> k) & 1; };
		const u8 cp = u8((bit(0) ^ bit(2) ^ bit(4) ^ bit(6)) |
		                 ((bit(1) ^ bit(3) ^ bit(5) ^ bit(7)) << 1) |
		                 ((bit(0) ^ bit(1) ^ bit(4) ^ bit(5)) << 2) |
		                 ((bit(2) ^ bit(3) ^ bit(6) ^ bit(7)) << 3) |
		                 ((bit(0) ^ bit(1) ^ bit(2) ^ bit(3)) << 4) |
		                 ((bit(4) ^ bit(5) ^ bit(6) ^ bit(7)) << 5));
		int par = 0;
		for (int k = 0; k < 8; k++)
			par ^= bit(k);
		reg1 ^= cp;
		if (par) {
			reg3 ^= u8(j);
			reg2 ^= u8(~j);
		}
	}
	u8 t1 = 0, t2 = 0, a = 0x80, bm = 0x80;
	for (int i = 0; i < 4; i++) {
		if (reg3 & a) t1 |= bm;
		bm >>= 1;
		if (reg2 & a) t1 |= bm;
		bm >>= 1;
		a >>= 1;
	}
	bm = 0x80;
	for (int i = 0; i < 4; i++) {
		if (reg3 & a) t2 |= bm;
		bm >>= 1;
		if (reg2 & a) t2 |= bm;
		bm >>= 1;
		a >>= 1;
	}
	const u8 c0 = u8(~t1), c1 = u8(~t2);
	// SmartMedia の並びは、行の偶奇の 2 バイトが入れ替わる
	out[0] = c1;
	out[1] = c0;
	out[2] = u8(((~reg1) << 2) | 0x03);
}

// 論理の書式（SSFDC の FAT）。MU2000 の UTIL → CARD → Format が書くものと同じにする
// （エミュの firmware に 4 つの容量で書式化させて、書かれたページを突き合わせて決めた）。
// 区画表の CHS・隠しセクター数・FAT の大きさ・ヘッド数などは容量ごとに規格で決まっている
struct logical_format
{
	u32 megabytes;
	u8  chs_start[3], type, chs_end[3];
	u32 hidden, total;       // 区画の頭（論理セクター）と、区画のセクター数
	u16 fat_sectors, sectors_per_track, heads;
	bool fat16;
};
constexpr logical_format FORMATS[] = {
	{ 16,  { 0x02, 0x0a, 0x00 }, 0x01, { 0x03, 0x50, 0xf3 }, 41, 31959,  3,  16, 4,  false },
	{ 32,  { 0x02, 0x04, 0x00 }, 0x01, { 0x07, 0x50, 0xf3 }, 35, 63965,  6,  16, 8,  false },
	{ 64,  { 0x01, 0x18, 0x00 }, 0x01, { 0x07, 0x60, 0xf3 }, 55, 127945, 12, 32, 8,  false },
	{ 128, { 0x01, 0x10, 0x00 }, 0x06, { 0x0f, 0x60, 0xf3 }, 47, 255953, 32, 32, 16, true },
};

void put16(u8 *p, u32 v) { p[0] = u8(v); p[1] = u8(v >> 8); }
void put32(u8 *p, u32 v) { put16(p, v); put16(p + 2, v >> 16); }

} // namespace

namespace {

constexpr u32 CLUSTER = 32 * smartmedia::PAGE;   // 1 クラスタ 32 セクター（16KB）

// 区画の中で、ブート・FAT・ルートの後ろに置けるクラスタの数
u32 data_clusters(const logical_format &f)
{
	return (f.total - (1 + 2 * u32(f.fat_sectors) + 16)) / 32;
}

// 8.3 の名前（「NAME.EXT」、大文字）を、ディレクトリの項目の 11 バイトにする。使えない名前なら false
bool dir_name(const std::string &name, u8 out[11])
{
	std::fill(out, out + 11, u8(' '));
	const size_t dot = name.find('.');
	const std::string stem = name.substr(0, dot);
	const std::string ext = dot == std::string::npos ? std::string() : name.substr(dot + 1);
	if (stem.empty() || stem.size() > 8 || ext.size() > 3)
		return false;
	auto ok = [](char c) {
		return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || std::string("_-$~!#%&'()@^`{}").find(c) != std::string::npos;
	};
	for (size_t i = 0; i < stem.size(); i++) {
		if (!ok(stem[i]))
			return false;
		out[i] = u8(stem[i]);
	}
	for (size_t i = 0; i < ext.size(); i++) {
		if (!ok(ext[i]))
			return false;
		out[8 + i] = u8(ext[i]);
	}
	return true;
}

} // namespace

u32 smartmedia::megabytes_for(size_t bytes)
{
	const u64 need = (u64(bytes) + CLUSTER - 1) / CLUSTER;
	for (const logical_format &f : FORMATS)
		if (need <= data_clusters(f))
			return f.megabytes;
	return 0;
}

bool smartmedia::format(const std::vector<root_file> &files)
{
	const logical_format *f = nullptr;
	for (const logical_format &x : FORMATS)
		if (x.megabytes == megabytes())
			f = &x;
	if (!f)
		return false;
	// 区画の頭からブート 1 + FAT 2 組 + ルートディレクトリ 256 項目（16 セクター）までを書く。
	// ファイルがあれば、その後ろにクラスタ 2 から順に並べる。
	// 論理ブロック n は物理ブロック（ゾーン z = n / 1000 の頭 + n % 1000、ゾーン 0 は CIS の次から）。
	// 書くブロックは全部のページを 0 で埋める
	const u32 root_at = f->hidden + 1 + 2 * u32(f->fat_sectors);
	const u32 data_at = root_at + 16;
	u32 clusters = 0;
	if (files.size() > 256)
		return false;
	for (const root_file &rf : files)
		clusters += u32((rf.bytes.size() + CLUSTER - 1) / CLUSTER);
	if (clusters > data_clusters(*f))
		return false;
	const u32 used = data_at + clusters * 32;
	const u32 blocks = (used + PAGES_PER_BLOCK - 1) / PAGES_PER_BLOCK;
	std::vector<u8> sec(size_t(blocks) * PAGES_PER_BLOCK * PAGE, 0);
	// MBR の区画表（1 つ目の項目）
	u8 *mbr = sec.data();
	mbr[446] = 0x80;
	std::copy(f->chs_start, f->chs_start + 3, mbr + 447);
	mbr[450] = f->type;
	std::copy(f->chs_end, f->chs_end + 3, mbr + 451);
	put32(mbr + 454, f->hidden);
	put32(mbr + 458, f->total);
	mbr[510] = 0x55;
	mbr[511] = 0xaa;
	// ブートセクター。名前の欄は空白、拡張の印は無し（firmware と同じ）
	u8 *bs = sec.data() + size_t(f->hidden) * PAGE;
	bs[0] = 0xe9;
	std::fill(bs + 3, bs + 11, u8(' '));
	put16(bs + 11, PAGE);
	bs[13] = 32;                      // 1 クラスタ 32 セクター
	put16(bs + 14, 1);                // 予約 1
	bs[16] = 2;                       // FAT 2 組
	put16(bs + 17, 256);              // ルートの項目数
	if (f->total < 0x10000)
		put16(bs + 19, f->total);
	else
		put32(bs + 32, f->total);
	bs[21] = 0xf8;
	put16(bs + 22, f->fat_sectors);
	put16(bs + 24, f->sectors_per_track);
	put16(bs + 26, f->heads);
	put32(bs + 28, f->hidden);
	std::copy_n(f->fat16 ? "FAT16   " : "FAT12   ", 8, bs + 54);
	bs[510] = 0x55;
	bs[511] = 0xaa;
	// FAT の頭（2 組）
	for (u32 k = 0; k < 2; k++) {
		u8 *fat = sec.data() + size_t(f->hidden + 1 + k * f->fat_sectors) * PAGE;
		fat[0] = 0xf8;
		fat[1] = fat[2] = 0xff;
		if (f->fat16)
			fat[3] = 0xff;
	}
	// ファイル。ディレクトリの項目と、クラスタの鎖（FAT は 2 組とも）と中身
	{
		const std::time_t now = std::time(nullptr);
		const std::tm *tm = std::localtime(&now);
		const u16 dos_time = tm ? u16((tm->tm_hour << 11) | (tm->tm_min << 5) | (tm->tm_sec / 2)) : 0;
		const u16 dos_date = tm ? u16(((std::max(tm->tm_year, 80) - 80) << 9) | ((tm->tm_mon + 1) << 5) | tm->tm_mday)
		                        : u16(1 << 5 | 1);
		auto set_fat = [&](u32 cl, u32 v) {
			for (u32 k = 0; k < 2; k++) {
				u8 *fat = sec.data() + size_t(f->hidden + 1 + k * f->fat_sectors) * PAGE;
				if (f->fat16) {
					put16(fat + cl * 2, v);
				} else {
					u8 *p = fat + cl * 3 / 2;
					if (cl & 1) {
						p[0] = u8((p[0] & 0x0f) | ((v << 4) & 0xf0));
						p[1] = u8(v >> 4);
					} else {
						p[0] = u8(v);
						p[1] = u8((p[1] & 0xf0) | ((v >> 8) & 0x0f));
					}
				}
			}
		};
		const u32 end_mark = f->fat16 ? 0xffff : 0xfff;
		u32 next = 2;
		for (size_t i = 0; i < files.size(); i++) {
			const root_file &rf = files[i];
			u8 *de = sec.data() + size_t(root_at) * PAGE + i * 32;
			if (!dir_name(rf.name, de))
				return false;
			de[11] = 0x20;                // 書庫の印
			put16(de + 22, dos_time);
			put16(de + 24, dos_date);
			const u32 n = u32((rf.bytes.size() + CLUSTER - 1) / CLUSTER);
			put16(de + 26, n ? next : 0);
			put32(de + 28, u32(rf.bytes.size()));
			for (u32 k = 0; k < n; k++)
				set_fat(next + k, k + 1 < n ? next + k + 1 : end_mark);
			std::copy(rf.bytes.begin(), rf.bytes.end(), sec.begin() + std::ptrdiff_t(size_t(data_at + (next - 2) * 32) * PAGE));
			next += n;
		}
	}
	// 物理のページへ。予備の領域はブロックの番地（0001 0bbb bbbb bbbp、p で 1 の数を偶数に）と ECC
	for (u32 lb = 0; lb < blocks; lb++) {
		const u32 zone = lb / 1000, in_zone = lb % 1000;
		const u32 phys = zone * 1024 + in_zone + (zone == 0 ? 1 : 0);
		u16 addr = u16(0x1000 | (in_zone << 1));
		int ones = 0;
		for (u16 v = addr; v; v &= u16(v - 1))
			ones++;
		if (ones & 1)
			addr |= 1;
		for (u32 pg = 0; pg < PAGES_PER_BLOCK; pg++) {
			u8 *p = m_data.data() + (size_t(phys) * PAGES_PER_BLOCK + pg) * page_bytes();
			const u8 *src = sec.data() + (size_t(lb) * PAGES_PER_BLOCK + pg) * PAGE;
			std::copy(src, src + PAGE, p);
			u8 *sp = p + PAGE;
			std::fill(sp, sp + SPARE, u8(0xff));
			u8 e1[3], e2[3];
			ecc256(p, e1);
			ecc256(p + 256, e2);
			sp[6] = sp[11] = u8(addr >> 8);
			sp[7] = sp[12] = u8(addr);
			sp[8] = e2[0]; sp[9] = e2[1]; sp[10] = e2[2];
			sp[13] = e1[0]; sp[14] = e1[1]; sp[15] = e1[2];
		}
	}
	m_dirty = true;
	return true;
}

bool smartmedia::create(u32 megabytes)
{
	const u8 code = device_code_for(megabytes);
	if (!code)
		return false;
	m_pages = megabytes * 1024 * 1024 / PAGE;
	m_device_code = code;
	m_data.assign(size_t(m_pages) * page_bytes(), 0xff);
	m_dirty_blocks.assign(m_pages / PAGES_PER_BLOCK, 1);
	// 物理の書式（SSFDC）。店で売っている SmartMedia は最初から、先頭の良いブロックに CIS が書いてある。
	// MU2000 の書式化はこれを探してから FAT を書くので、CIS が無いと「Bad Card!」になる。
	// firmware が見るのは CIS の頭の 10 バイトだけ。予備の領域はブロックの番地を 0000 にし、ECC を入れる
	static const u8 cis_head[10] = { 0x01, 0x03, 0xd9, 0x01, 0xff, 0x18, 0x02, 0xdf, 0x01, 0x20 };
	for (u32 pg = 0; pg < 2; pg++) {
		u8 *p = m_data.data() + size_t(pg) * page_bytes();
		std::fill(p, p + PAGE, u8(0xff));
		std::copy(cis_head, cis_head + 10, p);
		u8 *sp = p + PAGE;
		u8 e1[3], e2[3];
		ecc256(p, e1);
		ecc256(p + 256, e2);
		sp[6] = sp[7] = 0x00;
		sp[11] = sp[12] = 0x00;
		sp[8] = e2[0]; sp[9] = e2[1]; sp[10] = e2[2];
		sp[13] = e1[0]; sp[14] = e1[1]; sp[15] = e1[2];
	}
	// CIS のブロックの残りのページも、番地は 0000
	for (u32 pg = 2; pg < PAGES_PER_BLOCK; pg++) {
		u8 *sp = m_data.data() + size_t(pg) * page_bytes() + PAGE;
		sp[6] = sp[7] = sp[11] = sp[12] = 0x00;
	}
	m_dirty = true;
	return true;
}

bool smartmedia::load(const std::string &path, std::string &err)
{
	std::FILE *f = open_file(path, "rb");
	if (!f) {
		err = CLI_T("Cannot open the card file: ", "カードのファイルを開けない: ") + path;
		return false;
	}
	std::fseek(f, 0, SEEK_END);
	const long size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	u32 mb = 0;
	for (u32 m : { 16u, 32u, 64u, 128u })
		if (size_t(size) == size_t(m) * 1024 * 1024 / PAGE * (PAGE + SPARE))
			mb = m;
	if (!mb) {
		std::fclose(f);
		err = CLI_T("The card file is not the size of a 16/32/64/128MB SmartMedia: ", "カードのファイルの大きさが 16/32/64/128MB の SmartMedia と合わない: ") + path;
		return false;
	}
	create(mb);
	const size_t got = std::fread(m_data.data(), 1, m_data.size(), f);
	std::fclose(f);
	if (got != m_data.size()) {
		eject();
		err = CLI_T("Could not read the whole card file: ", "カードのファイルを読み切れない: ") + path;
		return false;
	}
	clear_dirty();
	return true;
}

void smartmedia::mark_dirty(u32 page)
{
	const u32 b = page / PAGES_PER_BLOCK;
	if (b < m_dirty_blocks.size())
		m_dirty_blocks[b] = 1;
	m_dirty = true;
}

void smartmedia::take_dirty_blocks(std::vector<block> &out)
{
	out.clear();
	const size_t bytes = size_t(PAGES_PER_BLOCK) * page_bytes();
	for (u32 b = 0; b < m_dirty_blocks.size(); b++) {
		if (!m_dirty_blocks[b])
			continue;
		const auto from = m_data.begin() + ptrdiff_t(size_t(b) * bytes);
		out.push_back({ b, std::vector<u8>(from, from + ptrdiff_t(bytes)) });
		m_dirty_blocks[b] = 0;
	}
	m_dirty = false;
}

bool smartmedia::write_blocks(const std::string &path, const std::vector<block> &blocks, std::string &err)
{
	if (blocks.empty())
		return true;
	std::FILE *f = open_file(path, "r+b");
	if (!f) {
		err = CLI_T("Cannot write back to the card file: ", "カードのファイルに書き戻せない: ") + path;
		return false;
	}
	bool ok = true;
	for (const block &b : blocks) {
		const long at = long(b.index) * long(PAGES_PER_BLOCK) * long(PAGE + SPARE);
		if (std::fseek(f, at, SEEK_SET) != 0 || std::fwrite(b.bytes.data(), 1, b.bytes.size(), f) != b.bytes.size())
			ok = false;
	}
	if (std::fclose(f) != 0)
		ok = false;
	if (!ok)
		err = CLI_T("Could not write everything back to the card file: ", "カードのファイルに書き戻し切れない: ") + path;
	return ok;
}

bool smartmedia::save(const std::string &path, std::string &err) const
{
	std::FILE *f = open_file(path, "wb");
	if (!f) {
		err = CLI_T("Cannot write the card file: ", "カードのファイルを書けない: ") + path;
		return false;
	}
	const size_t put = std::fwrite(m_data.data(), 1, m_data.size(), f);
	std::fclose(f);
	if (put != m_data.size()) {
		err = CLI_T("Could not write the whole card file: ", "カードのファイルを書き切れない: ") + path;
		return false;
	}
	return true;
}

void smartmedia::control_w(u8 v)
{
	m_ctrl = v;
}

void smartmedia::data_w(u8 v)
{
	if (!inserted() || !(m_ctrl & 0x01))
		return;
	if (m_ctrl & 0x08) {
		command(v);
	} else if (m_ctrl & 0x04) {
		address(v);
	} else if (m_mode == mode::program) {
		if (m_column < page_bytes())
			m_buf[m_column++] = v;
	}
}

u8 smartmedia::data_r()
{
	if (!inserted() || !(m_ctrl & 0x01))
		return 0xff;
	switch (m_mode) {
	case mode::read_id: {
		const u8 id[2] = { MAKER_TOSHIBA, m_device_code };
		return id[m_id_pos++ % 2];
	}
	case mode::status:
		// bit 7 は書き込みを禁じていない、bit 6 は準備ができている、bit 0 は失敗
		return u8((write_protected ? 0x00 : 0x80) | 0x40);
	case mode::read: {
		if (m_page >= m_pages)
			return 0xff;
		const u8 v = m_data[size_t(m_page) * page_bytes() + m_column];
		m_column++;
		if (m_column >= page_bytes()) {
			// 次のページへ続けて読む。予備だけを読む命令（50）なら次の予備から
			m_page++;
			m_column = (m_pointer == 0x50) ? PAGE : 0;
			if (m_pointer == 0x01)
				m_pointer = 0x00;
		}
		return v;
	}
	default:
		return 0xff;
	}
}

void smartmedia::command(u8 c)
{
	m_last_cmd = c;
	switch (c) {
	case 0xff:                                   // リセット
		m_mode = mode::idle;
		m_pointer = 0x00;
		break;
	case 0x00: case 0x01: case 0x50:             // 読む（ページの頭 / 後ろ半分 / 予備）
		m_pointer = c;
		m_mode = mode::read;
		m_addr_count = 0;
		break;
	case 0x80:                                   // 書く中身を受け取り始める
		m_mode = mode::program;
		m_addr_count = 0;
		m_buf.assign(page_bytes(), 0xff);
		m_column = (m_pointer == 0x01) ? 256 : (m_pointer == 0x50) ? PAGE : 0;
		break;
	case 0x10:                                   // 書く。NAND は 1 を 0 にしかできない
		if (m_mode == mode::program && !write_protected && m_page < m_pages) {
			const size_t base = size_t(m_page) * page_bytes();
			for (u32 i = 0; i < page_bytes(); i++)
				m_data[base + i] &= m_buf[i];
			mark_dirty(m_page);
		}
		if (m_pointer == 0x01)
			m_pointer = 0x00;
		m_mode = mode::status;
		break;
	case 0x60:                                   // 消すブロックの番地を受け取り始める
		m_mode = mode::erase;
		m_addr_count = 0;
		break;
	case 0xd0:                                   // 消す
		if (m_mode == mode::erase && !write_protected) {
			const u32 block = m_page / PAGES_PER_BLOCK;
			const size_t base = size_t(block) * PAGES_PER_BLOCK * page_bytes();
			if (base < m_data.size())
				std::fill(m_data.begin() + base, m_data.begin() + std::min(m_data.size(), base + size_t(PAGES_PER_BLOCK) * page_bytes()), u8(0xff));
			mark_dirty(m_page);
		}
		m_mode = mode::status;
		break;
	case 0x70:                                   // 状態
		m_mode = mode::status;
		break;
	case 0x90:                                   // 名乗り
		m_mode = mode::read_id;
		m_id_pos = 0;
		break;
	default:
		break;
	}
}

void smartmedia::address(u8 a)
{
	switch (m_mode) {
	case mode::read:
	case mode::program:
		if (m_addr_count == 0) {
			m_column = a + ((m_pointer == 0x01) ? 256 : (m_pointer == 0x50) ? PAGE : 0);
			if (m_column >= page_bytes())
				m_column = page_bytes() - 1;
			m_page = 0;
		} else {
			const u32 shift = 8 * (m_addr_count - 1);
			m_page = (m_page & ~(u32(0xff) << shift)) | (u32(a) << shift);
		}
		m_addr_count++;
		break;
	case mode::erase: {
		// 消すときは列の番地が無く、ページの番地だけ（32MB までは 2 回、64MB からは 3 回）
		const u32 shift = 8 * m_addr_count;
		if (m_addr_count == 0)
			m_page = 0;
		m_page = (m_page & ~(u32(0xff) << shift)) | (u32(a) << shift);
		m_addr_count++;
		break;
	}
	default:
		break;
	}
}

void smartmedia::state(state_io &s)
{
	s.v(m_ctrl);
	u8 md = u8(m_mode);
	s.v(md);
	m_mode = mode(md);
	s.v(m_pointer); s.v(m_addr_count); s.v(m_column); s.v(m_page); s.v(m_id_pos); s.v(m_last_cmd);
	u32 n = u32(m_buf.size());
	s.v(n);
	if (!s.writing())
		m_buf.resize(std::min<u32>(n, 4096));
	if (!m_buf.empty())
		s.mem(m_buf.data(), m_buf.size());
}

} // namespace smu2000
