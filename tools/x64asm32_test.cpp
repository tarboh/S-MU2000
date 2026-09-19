// license:BSD-3-Clause
//
// x64asm32_test.cpp — x64asm.h の 32bit モード（x86-32）のエンコード・意味の自己検査
// （CPU32_LEDGER.md Phase 2 テスト D）。32bit の実行コードを本当に VirtualAlloc して動かし、
// C++ の参考実装と突き合わせる。全通過で exit 0、不一致があれば名前を出して exit 1。
//
// ビルド（リポジトリのルートで。tools/msvc32_build.ps1 と同じ vcvars amd64_x86 環境を作る）:
//   cmd /c " "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" amd64_x86 && ^
//          cl /nologo /std:c++20 /O2 /MT /utf-8 /EHsc /D_CRT_SECURE_NO_WARNINGS ^
//             /Isrc /Isrc/compat tools\x64asm32_test.cpp /Fe:build32-msvc\x64asm32_test.exe /Febuild32-msvc\"
// 実行: build32-msvc\x64asm32_test.exe
// 機械語は build32-msvc\x64asm32.bin に出る。相互確認（手動）:
//   C:\msys64\mingw32\bin\objdump.exe -D -b binary -mi8086 -Mintel build32-msvc\x64asm32.bin
//   （rex が一切無く、shld/shl・shrd/sar・add/adc・sub/sbb・mov eax,imm32+ff d0 の列になっていること）

#include "x64asm.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace x64asm;

static int g_fail = 0, g_pass = 0;
static const char *g_cur = "?";
static std::vector<u8> g_bin;   // 全部の機械語（objdump 用ダンプ）
static u32 g_in[8];             // 機械語への入力
static u32 g_out[8];            // 機械語が書く出力

typedef u32 (*fn0_t)(void);

static int call0(fn0_t f);
static std::vector<u8> g_last;   // 直前に作った機械語（EXC 時のダンプ用）
static void dump_on_exc(void)
{
	fputs("last blob: ", stderr);
	for (u8 b : g_last)
		fprintf(stderr, "%02x", b);
	fputc('\n', stderr);
	fflush(stderr);
}

static void (*g_fn)(void);
__declspec(naked) static void thunk(void)   // 訳した機械語の前後で callee 保存レジスタを守る
{
	__asm {
		pushad
		mov eax, g_fn
		call eax
		popad
		ret
	}
}

static void fail(const char *fmt, ...)
{
	g_fail++;
	va_list ap;
	va_start(ap, fmt);
	fputs("FAIL ", stderr);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

// 命令の先頭に REX らしいバイト（0x40..0x4F）が来たら失敗。実行可能 page に写して返す
static void *run(const char *name, assembler &a)
{
	g_cur = name;
	if (a.code.empty()) { fail("%s: empty", name); return nullptr; }
	if (a.code[0] >= 0x40 && a.code[0] <= 0x4f)
		fail("%s: rex-like head byte %02x", name, a.code[0]);
	g_bin.insert(g_bin.end(), a.code.begin(), a.code.end());
	g_last = a.code;
	if (getenv("X64ASM32_TRACE")) {
		fprintf(stderr, "run %s (%zu bytes)\n", name, a.code.size());
		fflush(stderr);
		FILE *fp = nullptr;
		if (fopen_s(&fp, "build32-msvc\\x64asm32.dbg.bin", "ab") == 0 && fp) {
			fwrite(a.code.data(), 1, a.code.size(), fp);
			fclose(fp);
		}
	}
	void *p = VirtualAlloc(nullptr, a.code.size(), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!p) { fail("%s: VirtualAlloc", name); return nullptr; }
	std::memcpy(p, a.code.data(), a.code.size());
	return p;
}

// バイト列がコード中に含まれること（-1 は何でも）
static void want_seq(const char *name, const std::vector<u8> &c, std::initializer_list<int> bs)
{
	for (size_t s = 0; s + bs.size() <= c.size(); s++) {
		size_t i = 0;
		for (int b : bs) {
			if (b >= 0 && c[s + i] != u8(b)) break;
			i++;
		}
		if (i == bs.size()) return;
	}
	fail("%s: sequence not found (len %zu)", name, c.size());
}

// g_in ベースの決まり文句
static void head(assembler &a) { a.imm32(RAX, u32(uintptr_t(&g_in[0]))); }
static void inp(assembler &a, int i, u8 r) { a.load32(r, mem{ RAX, NOREG, 1, i * 4 }); }
static void out2(assembler &a, u8 lo, u8 hi)      // lo,hi != RAX。RDX は死んでいること
{
	a.push(lo);
	a.push(hi);
	a.imm32(RAX, u32(uintptr_t(&g_out[0])));
	a.pop(RDX);
	a.store32(mem{ RAX, NOREG, 1, 4 }, RDX);
	a.pop(RDX);
	a.store32(mem{ RAX, NOREG, 1, 0 }, RDX);
	a.ret();
}

static u64 rs = 88172645463325252ull;
static u32 rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return u32(rs >> 16); }
static u64 pair(void) { return u64(rnd()) | (u64(rnd()) << 32); }
static u64 asar64(u64 v, int n) { return u64((s64)v >> (n >= 63 ? 63 : n)); }

// ---------------------------------------------------------------- 32bit 命令の網羅

static void t_moves(void)
{
	assembler a;
	head(a);                          // B8 disp32
	a.mov32(RSI, RAX);                // 8B F0（in 表の基準を退避）
	a.loadu16(RDI, mem{ RSI, NOREG, 1, 2 });
	a.loads16(RBP, mem{ RSI, NOREG, 1, 2 });
	a.loads16_32(RBX, mem{ RSI, NOREG, 1, 2 });
	a.loadu8(RCX, mem{ RSI, NOREG, 1, 1 });
	a.load32(RDX, mem{ RSI, NOREG, 1, 4 });
	a.movzx8(RBX, RDX);
	a.movzx16(RBX, RDX);
	a.movsx8(RBX, RDX);
	a.movsx16(RBX, RDX);
	a.bswap32(RDI);
	a.bsr32(RDI, RDX);                // rdx=in[1] は non-zero にする
	a.rol32(RDI, 7);
	a.shl32cl(RDI);
	a.shr32cl(RDI);
	a.neg32(RDI);
	a.not32(RDI);
	a.add32(RDI, RCX);
	a.sub32(RDI, RCX);
	a.or32(RDI, RCX);
	a.xor32(RDI, RCX);
	a.and32(RDI, RCX);
	a.imul32(RDI, RCX);
	a.imul32i(RDI, RCX, 3);
	a.add32ri(RDI, 5);
	a.sub32ri(RDI, 5);
	a.and32i(RDI, 0xffff);
	a.or32ri(RDI, 1);
	a.xor32ri(RDI, 1);
	a.cmp32ri(RDI, 0);
	a.test32ri(RDI, 0xff);
	a.test32(RDI, RDI);
	a.add32rm(RDI, mem{ RSI, NOREG, 1, 0 });
	a.cmp32rm(RDI, mem{ RSI, NOREG, 1, 4 });
	a.test32rm(RDI, mem{ RSI, NOREG, 1, 4 });
	a.cmp32i_mem(mem{ RSI, NOREG, 1, 0 }, 1);
	a.sub32i_mem(mem{ RSI, NOREG, 1, 4 }, 2);
	a.add32i_mem(mem{ RSI, NOREG, 1, 4 }, 2);
	a.or32i_mem(mem{ RSI, NOREG, 1, 4 }, 0);
	a.and32i_mem(mem{ RSI, NOREG, 1, 4 }, 0xffffffffu);
	a.xor32i_mem(mem{ RSI, NOREG, 1, 4 }, 0);
	a.test32i_mem(mem{ RSI, NOREG, 1, 4 }, 1);
	a.shl32i_mem(mem{ RSI, NOREG, 1, 4 }, 1);
	a.shr32i_mem(mem{ RSI, NOREG, 1, 4 }, 1);
	a.shl32(RDI, 3);
	a.shr32(RDI, 3);
	a.sar32(RDI, 3);
	a.setcc(0x94, RAX);
	a.lea32(RAX, mem{ RSI, NOREG, 1, 8 });
	// 基準を捨てて g_out に書く（値はどうでもいい：通ることとエンコードが見たい）
	a.imm32(RDX, u32(uintptr_t(&g_out[0])));
	a.mov32(RAX, RSI);
	a.store32(mem{ RDX, NOREG, 1, 0 }, RDI);
	a.ret();
	want_seq("imm32-head", a.code, { 0xb8 });
	want_seq("loadu16", a.code, { 0x0f, 0xb7, 0xbe });    // be = mod10 reg=7 rm=6 (ESI base)
	want_seq("loads16", a.code, { 0x0f, 0xbf });
	want_seq("bswap-noprefix", a.code, { 0x0f, 0xcf });   // 66 も 4x も無し
	void *p = run("moves", a);
	if (!p) return;
	g_in[1] = 0x11223344;
	if (call0((fn0_t)p)) return;
	VirtualFree(p, 0, MEM_RELEASE);
	g_pass++;
}

static int call0(fn0_t f)   // SEH 付きで呼ぶ（クラッシュ時の名前と EIP を出す）。g_out を消しておく
{
	// 訳した機械語は EBX/EBP/ESI/EDI（callee 保存）を自由に使るので、C++ 側に返すとき守らせる
	std::memset(g_out, 0, sizeof g_out);
	g_fn = (void (*)(void))f;
	__try { thunk(); }
	__except (dump_on_exc(), fprintf(stderr, "EXC %08x eip %p fn %p (in %s)\n", GetExceptionCode(), GetExceptionInformation()->ExceptionRecord->ExceptionAddress, (void *)f, g_cur), fflush(stderr), EXCEPTION_EXECUTE_HANDLER) { return 1; }
	return 0;
}

static void t_stores(void)
{
	assembler a;
	a.imm32(RBX, u32(uintptr_t(&g_out[0])));
	a.imm32(RCX, 0x11223344);
	a.store32(mem{ RBX, NOREG, 1, 0 }, RCX);
	a.store32i(mem{ RBX, NOREG, 1, 4 }, 0xdeadbeef);
	a.store16(mem{ RBX, NOREG, 1, 8 }, RCX);
	a.store8(mem{ RBX, NOREG, 1, 10 }, RCX);
	a.store8i(mem{ RBX, NOREG, 1, 11 }, 0x5a);
	a.subrsp(8);                                       // 83 EC 08
	a.store32(mem{ RSP, NOREG, 1, 4 }, RCX);            // SIB base=ESP
	a.load32(RDX, mem{ RSP, NOREG, 1, 4 });
	a.addrsp(8);
	a.store32(mem{ RBX, NOREG, 1, 12 }, RDX);
	a.ret();
	want_seq("subesp", a.code, { 0x81, 0xec, 0x08, 0x00, 0x00, 0x00 });
	want_seq("sib-esp", a.code, { 0x89, 0x8c, 0x24, 0x04 });
	want_seq("store16-66", a.code, { 0x66, 0x89 });
	void *p = run("stores", a);
	if (!p) return;
	if (call0((fn0_t)p)) return;
	VirtualFree(p, 0, MEM_RELEASE);
	if (g_out[0] != 0x11223344 || g_out[1] != 0xdeadbeef || (g_out[2] & 0xffff) != 0x3344 ||
	    ((g_out[2] >> 16) & 0xff) != 0x44 || ((g_out[2] >> 24) & 0xff) != 0x5a || g_out[3] != 0x11223344) {
		fail("stores: %08x %08x %08x %08x", g_out[0], g_out[1], g_out[2], g_out[3]);
		return;
	}
	g_pass++;
}

static void t_imm64(void)
{
	assembler a;
	a.imm32(RBX, u32(uintptr_t(&g_out[0])));
	a.mov_imm64(RCX, RBP, 0x7fffff);
	a.store32(mem{ RBX, NOREG, 1, 0 }, RCX);
	a.store32(mem{ RBX, NOREG, 1, 4 }, RBP);
	a.mov_imm64(RCX, RBP, u64(s64(-0x8000)));
	a.store32(mem{ RBX, NOREG, 1, 8 }, RCX);
	a.store32(mem{ RBX, NOREG, 1, 12 }, RBP);
	a.ret();
	want_seq("mov-imm64-pos", a.code, { 0xb9, 0xff, 0xff, 0x7f, 0x00, 0xbd, 0x00, 0x00, 0x00, 0x00 });
	want_seq("mov-imm64-neg", a.code, { 0xb9, 0x00, 0x80, 0xff, 0xff, 0xbd, 0xff, 0xff, 0xff, 0xff });
	void *p = run("imm64", a);
	if (!p) return;
	if (call0((fn0_t)p)) return;
	VirtualFree(p, 0, MEM_RELEASE);
	if (g_out[0] != 0x7fffff || g_out[1] != 0 || g_out[2] != 0xffff8000u || g_out[3] != 0xffffffffu) {
		fail("imm64: %08x %08x %08x %08x", g_out[0], g_out[1], g_out[2], g_out[3]);
		return;
	}
	g_pass++;
}

static void t_arith64(void)
{
	assembler a;
	head(a);
	inp(a, 0, RCX); inp(a, 1, RDX);
	inp(a, 2, RBX); inp(a, 3, RBP);
	a.add64(RCX, RDX, RBX, RBP);
	out2(a, RCX, RDX);
	want_seq("add64-add-adc", a.code, { 0x01, 0xd9, 0x11, 0xea });   // add ecx,ebx / adc edx,ebp
	void *p = run("add64", a);
	if (!p) return;
	const fn0_t f = (fn0_t)p;
	for (int i = 0; i < 60000; i++) {
		u64 x = i < 10 ? u64(i) : (i == 12 ? ~u64(0) : pair());
		u64 y = pair();
		if (i == 13) y = u64(1);
		g_in[0] = u32(x); g_in[1] = u32(x >> 32);
		g_in[2] = u32(y); g_in[3] = u32(y >> 32);
		if (call0(f)) return;
		u64 r = u64(g_out[0]) | (u64(g_out[1]) << 32);
		if (r != x + y) { fail("add64 %llx+%llx", (unsigned long long)x, (unsigned long long)y); return; }
	}
	VirtualFree(p, 0, MEM_RELEASE);

	assembler b;
	head(b);
	inp(b, 0, RCX); inp(b, 1, RDX);
	inp(b, 2, RBX); inp(b, 3, RBP);
	b.sub64(RCX, RDX, RBX, RBP);
	out2(b, RCX, RDX);
	want_seq("sub64-sub-sbb", b.code, { 0x29, 0xd9, 0x19, 0xea });   // sub ecx,ebx / sbb edx,ebp
	p = run("sub64", b);
	if (!p) return;
	const fn0_t g = (fn0_t)p;
	for (int i = 0; i < 60000; i++) {
		u64 x = i < 10 ? u64(i) : pair();
		u64 y = pair();
		if (i == 15) { x = 0; y = 1; }
		if (i == 16) { x = 0x100000000ull; y = 1; }
		g_in[0] = u32(x); g_in[1] = u32(x >> 32);
		g_in[2] = u32(y); g_in[3] = u32(y >> 32);
		if (call0(g)) return;
		u64 r = u64(g_out[0]) | (u64(g_out[1]) << 32);
		if (r != x - y) { fail("sub64 %llx-%llx", (unsigned long long)x, (unsigned long long)y); return; }
	}
	VirtualFree(p, 0, MEM_RELEASE);

	assembler c;
	head(c);
	inp(c, 0, RCX); inp(c, 1, RDX);
	inp(c, 2, RBX); inp(c, 3, RBP);
	c.and64(RCX, RDX, RBX, RBP);
	out2(c, RCX, RDX);
	p = run("and64", c);
	if (!p) return;
	const fn0_t h = (fn0_t)p;
	for (int i = 0; i < 20000; i++) {
		u64 x = pair(), y = pair();
		g_in[0] = u32(x); g_in[1] = u32(x >> 32);
		g_in[2] = u32(y); g_in[3] = u32(y >> 32);
		if (call0(h)) return;
		u64 r = u64(g_out[0]) | (u64(g_out[1]) << 32);
		if (r != (x & y)) { fail("and64"); return; }
	}
	VirtualFree(p, 0, MEM_RELEASE);
	g_pass++;
}

static void t_shift64(void)
{
	static const int ns[] = { 1, 5, 15, 22, 31, 32, 33, 40, 55, 63 };
	for (int n : ns) {
		char nm[32];
		assembler a;
		head(a);
		inp(a, 0, RCX); inp(a, 1, RDX);
		a.shl64(RCX, RDX, u8(n));
		out2(a, RCX, RDX);
		if (n < 32) {                                   // shld edx,ecx,n / shl ecx,n
			sprintf(nm, "shl64-%d", n);
			want_seq(nm, a.code, { 0x0f, 0xa4, 0xca, u8(n), 0xc1, 0xe1, u8(n) });
		} else if (n > 32) {                            // mov edx,ecx / shl edx,n-32 / xor ecx,ecx
			sprintf(nm, "shl64-%d", n);
			want_seq(nm, a.code, { 0x8b, 0xd1, 0xc1, 0xe2, u8(n - 32), 0x31, 0xc9 });
		} else {
			sprintf(nm, "shl64-%d", n);
			want_seq(nm, a.code, { 0x8b, 0xd1, 0x31, 0xc9 });
		}
		void *p = run(nm, a);
		if (!p) return;
		const fn0_t f = (fn0_t)p;
		for (int i = 0; i < 2000; i++) {
			u64 x = i < 8 ? (u64(1) << (i * 7 % 64)) | (i & 1 ? 0x55555555u : 0) : pair();
			g_in[0] = u32(x); g_in[1] = u32(x >> 32);
			if (call0(f)) return;
			u64 r = u64(g_out[0]) | (u64(g_out[1]) << 32);
			if (r != (x << n)) { fail("SHLCMP n=%d i=%d x=%llx -> %llx want %llx", n, i, (unsigned long long)x, (unsigned long long)r, (unsigned long long)(x << n)); return; }
		}
		VirtualFree(p, 0, MEM_RELEASE);

		assembler b;
		head(b);
		inp(b, 0, RCX); inp(b, 1, RDX);
		b.sar64(RCX, RDX, u8(n));
		out2(b, RCX, RDX);
		sprintf(nm, "sar64-%d", n);
		if (n < 32)
			want_seq(nm, b.code, { 0x0f, 0xac, 0xd1, u8(n), 0xc1, 0xfa, u8(n) });   // shrd ecx,edx,n / sar edx,n
		else if (n > 32)
			want_seq(nm, b.code, { 0x8b, 0xca, 0xc1, 0xf9, u8(n - 32), 0xc1, 0xfa, 0x1f });   // mov ecx,edx; sar ecx,n-32; sar edx,31
		else
			want_seq(nm, b.code, { 0x8b, 0xca, 0xc1, 0xfa, 0x1f });                            // mov ecx,edx; sar edx,31
		p = run(nm, b);
		if (!p) return;
		const fn0_t g = (fn0_t)p;
		for (int i = 0; i < 2000; i++) {
			u64 x = i < 4 ? ~u64(0) : (i < 8 ? u64(1) << (i * 7 % 64) : pair());
			g_in[0] = u32(x); g_in[1] = u32(x >> 32);
			if (call0(g)) return;
			u64 r = u64(g_out[0]) | (u64(g_out[1]) << 32);
			if (r != asar64(x, n)) { fail("SARCMP n=%d i=%d x=%llx -> %llx want %llx", n, i, (unsigned long long)x, (unsigned long long)r, (unsigned long long)asar64(x, n)); return; }
		}
		VirtualFree(p, 0, MEM_RELEASE);
	}
	g_pass++;
}

static void t_neg64(void)
{
	// neg lo / not hi / sbb hi,0 → 下 64bit と SF（符号）を確かめる
	assembler a;
	head(a);
	inp(a, 0, RCX); inp(a, 1, RDX);
	a.neg64(RCX, RDX);
	a.push(RCX);
	a.push(RDX);
	a.setcc(0x98, RCX);              // js を CL へ（結果はもうスタックにある。AL だと次で消える）
	a.imm32(RAX, u32(uintptr_t(&g_out[0])));
	a.store8(mem{ RAX, NOREG, 1, 8 }, RCX);
	a.pop(RDX);
	a.store32(mem{ RAX, NOREG, 1, 4 }, RDX);
	a.pop(RCX);
	a.store32(mem{ RAX, NOREG, 1, 0 }, RCX);
	a.ret();
	want_seq("neg64-seq", a.code, { 0xf7, 0xd9, 0xf7, 0xd2, 0x83, 0xda, 0xff });
	void *p = run("neg64", a);
	if (!p) return;
	const fn0_t f = (fn0_t)p;
	static const u64 vs[] = { 0, 1, ~u64(0), u64(0x80000000), u64(s64(-0x8000)),
		                      u64(0x4000000000ull), u64(0x3fffffffffull), u64(1) << 63, u64(0x7fffffffffffffffull), 1ull << 31 };
	for (u64 x : vs) {
		g_in[0] = u32(x); g_in[1] = u32(x >> 32);
		if (call0(f)) return;
		s64 want = -(s64)x;
		u64 r = u64(g_out[0]) | (u64(g_out[1]) << 32);
		if (s64(r) != want || bool(g_out[2] & 0xff) != (want < 0)) {
			fail("neg64 %llx -> %llx sf=%d", (unsigned long long)x, (unsigned long long)r, int(g_out[2] & 0xff));
			VirtualFree(p, 0, MEM_RELEASE);
			return;
		}
	}
	for (int i = 0; i < 60000; i++) {
		u64 x = pair();
		if (i % 7 == 0) x = u64(s32(rnd()));
		g_in[0] = u32(x); g_in[1] = u32(x >> 32);
		if (call0(f)) return;
		s64 want = -(s64)x;
		u64 r = u64(g_out[0]) | (u64(g_out[1]) << 32);
		if (s64(r) != want || bool(g_out[2] & 0xff) != (want < 0)) { fail("neg64 rnd"); return; }
	}
	VirtualFree(p, 0, MEM_RELEASE);
	g_pass++;
}

static void t_cmov64(void)
{
	// cmovs64（MEG の rop2 の形：-x を作って SF が立ったら元の組へ戻る）
	assembler a;
	head(a);
	inp(a, 0, RCX); inp(a, 1, RBX);
	a.mov32(RDX, RCX);
	a.mov32(RBP, RBX);
	a.neg64(RDX, RBP);
	a.cmovs64(RDX, RBP, RCX, RBX);
	a.mov64(RCX, RBX, RDX, RBP);
	out2(a, RCX, RBX);
	want_seq("cmovs64-pair", a.code, { 0x0f, 0x48, 0xd1, 0x0f, 0x48, 0xeb });
	void *p = run("cmovs64", a);
	if (!p) return;
	const fn0_t f = (fn0_t)p;
	for (int i = 0; i < 60000; i++) {
		u64 x = i < 8 ? u64(s64(s32(i - 4))) : pair();
		g_in[0] = u32(x); g_in[1] = u32(x >> 32);
		if (call0(f)) return;
		u64 nx = 0 - x;
		u64 want = (s64)nx < 0 ? x : nx;
		u64 r = u64(g_out[0]) | (u64(g_out[1]) << 32);
		if (r != want) { fail("cmovs64 %llx -> %llx want %llx", (unsigned long long)x, (unsigned long long)r, (unsigned long long)want); return; }
	}
	VirtualFree(p, 0, MEM_RELEASE);

	// cmove64（cmp64i の後で ZF がラベルで正しいこと。hi が同じ帯も見る）
	assembler c;
	head(c);
	inp(c, 0, RCX); inp(c, 1, RDX);          // x
	inp(c, 2, RSI); inp(c, 3, RDI);          // 目印の組
	c.xor32(RBX, RBX);
	c.xor32(RBP, RBP);
	c.cmp64i(RCX, RDX, 0x800000);
	c.cmove64(RBX, RBP, RSI, RDI);           // x == 0x800000 なら目印を拾う
	out2(c, RBX, RBP);
	want_seq("cmove64-pair", c.code, { 0x0f, 0x44 });
	p = run("cmove64", c);
	if (!p) return;
	const fn0_t g = (fn0_t)p;
	static const u64 vs[] = { 0x800000, 0x800000 | (u64(1) << 32), 0x7fffff, 0x800001,
		                      u64(0xffffffff), 0, ~u64(0), u64(s64(-0x800001)), u64(0x100800000ull) };
	for (u64 x : vs) {
		g_in[0] = u32(x); g_in[1] = u32(x >> 32);
		g_in[2] = 0x31313131; g_in[3] = 0x32323232;
		if (call0(g)) return;
		u64 r = u64(g_out[0]) | (u64(g_out[1]) << 32);
		const bool eq = s64(x) == s64(0x800000);
		if (eq && r != 0x3232323231313131ull) { fail("cmove64 eq %llx -> %llx", (unsigned long long)x, (unsigned long long)r); return; }
		if (!eq && r != 0) { fail("cmove64 ne %llx -> %llx", (unsigned long long)x, (unsigned long long)r); return; }
	}
	for (int i = 0; i < 50000; i++) {
		u64 x = pair() >> (i % 12);
		if (i % 5 == 0) x = 0x800000 | (u64(rnd() & 1) << 40);
		g_in[0] = u32(x); g_in[1] = u32(x >> 32);
		g_in[2] = 0x31313131; g_in[3] = 0x32323232;
		if (call0(g)) return;
		u64 r = u64(g_out[0]) | (u64(g_out[1]) << 32);
		const bool eq = s64(x) == s64(0x800000);
		if (eq != (r != 0)) { fail("cmove64 rnd %llx", (unsigned long long)x); return; }
	}
	VirtualFree(p, 0, MEM_RELEASE);
	g_pass++;
}

static void t_cmp64i(void)
{
	// cmp64i + sete（== の判定。hi 跨ぎ）
	assembler a;
	head(a);
	inp(a, 0, RCX); inp(a, 1, RDX);
	a.cmp64i(RCX, RDX, 0x800000);
	a.imm32(RAX, u32(uintptr_t(&g_out[0])));
	a.sete_mem(mem{ RAX, NOREG, 1, 0 });
	a.ret();
	void *p = run("cmp64i-eq", a);
	if (!p) return;
	const fn0_t f = (fn0_t)p;
	static const u64 vs[] = { 0x800000, 0x10000800000ull, 0xffffffffull, u64(s64(-0x800000)), u64(s64(-0x800001)), 0, ~u64(0) };
	for (u64 x : vs) {
		g_in[0] = u32(x); g_in[1] = u32(x >> 32);
		if (call0(f)) return;
		if ((g_out[0] != 0) != (s64(x) == s64(0x800000))) { fail("cmp64i-eq %llx", (unsigned long long)x); return; }
	}
	VirtualFree(p, 0, MEM_RELEASE);

	// cmp64（組）+ jae：符号なし >= が hi 跨ぎで正しいこと
	assembler c;
	head(c);
	inp(c, 0, RCX); inp(c, 1, RDX);
	inp(c, 2, RBX); inp(c, 3, RBP);
	c.cmp64(RCX, RDX, RBX, RBP);
	c.imm32(RDI, 1);
	const size_t jae = c.jcc_fwd(0x83);       // jae: 1 を残す
	c.xor32(RDI, RDI);                        // でなければ 0
	c.patch(jae);
	c.imm32(RAX, u32(uintptr_t(&g_out[0])));
	c.store32(mem{ RAX, NOREG, 1, 0 }, RDI);
	c.ret();
	p = run("cmp64-jae", c);
	if (!p) return;
	const fn0_t g = (fn0_t)p;
	static const u64 es[] = { 0, 1, ~u64(0), u64(1) << 32, (u64(1) << 32) - 1, 0x80000000, 0xffffffff, u64(1) << 63 };
	for (u64 x : es) for (u64 y : es) {
		g_in[0] = u32(x); g_in[1] = u32(x >> 32);
		g_in[2] = u32(y); g_in[3] = u32(y >> 32);
		if (call0(g)) return;
		if (bool(g_out[0]) != (x >= y)) { fail("cmp64-jae %llx %llx", (unsigned long long)x, (unsigned long long)y); return; }
	}
	for (int i = 0; i < 50000; i++) {
		u64 x = pair() >> (i % 20), y = pair() >> (rnd() % 20);
		g_in[0] = u32(x); g_in[1] = u32(x >> 32);
		g_in[2] = u32(y); g_in[3] = u32(y >> 32);
		if (call0(g)) return;
		if (bool(g_out[0]) != (x >= y)) { fail("cmp64-jae rnd"); return; }
	}
	VirtualFree(p, 0, MEM_RELEASE);
	g_pass++;
}

static void t_jlt64(void)
{
	// 境界の全組み合わせ（jlt64/jgt64）
	static const u64 edges[] = {
		0, 1, 2, ~u64(0), ~u64(1), u64(0x7fffffff), u64(0x80000000), u64(0xffffffff),
		u64(0x100000000ull), u64(0x8000000000000000ull), u64(0x7fffffffffffffffull),
		u64(s64(-0x8000)), u64(s64(-0x7fff)), u64(0x8000), u64(0x4000000000ull),
		u64(s64(-0x4000000001ll)), u64(0x3fffffffffull), u64(s64(-0x4000000000ll)),
		u64(0xffff8000), u64(0x1ffffffffull),
	};
	for (u64 x : edges) for (u64 y : edges) {
		g_in[0] = u32(x); g_in[1] = u32(x >> 32);
		g_in[2] = u32(y); g_in[3] = u32(y >> 32);
		{
			assembler a;
			head(a);
			inp(a, 0, RCX); inp(a, 1, RDX);
			inp(a, 2, RBX); inp(a, 3, RBP);
			g_out[0] = 0;
			a.jlt64(RCX, RDX, RBX, RBP, [&](void) {
				a.imm32(RDI, u32(uintptr_t(&g_out[0])));
				a.store32i(mem{ RDI, NOREG, 1, 0 }, 1);
			});
			a.ret();
			void *p = run("jlt64", a);
			if (!p) return;
			if (call0((fn0_t)p)) return;
			VirtualFree(p, 0, MEM_RELEASE);
			if (bool(g_out[0]) != ((s64)x < (s64)y)) { fail("jlt64 %llx %llx", (unsigned long long)x, (unsigned long long)y); return; }
		}
		{
			assembler a;
			head(a);
			inp(a, 0, RCX); inp(a, 1, RDX);
			inp(a, 2, RBX); inp(a, 3, RBP);
			g_out[0] = 0;
			a.jgt64(RCX, RDX, RBX, RBP, [&](void) {
				a.imm32(RDI, u32(uintptr_t(&g_out[0])));
				a.store32i(mem{ RDI, NOREG, 1, 0 }, 2);
			});
			a.ret();
			void *p = run("jgt64", a);
			if (!p) return;
			if (call0((fn0_t)p)) return;
			VirtualFree(p, 0, MEM_RELEASE);
			if (g_out[0] != ((s64)x > (s64)y ? 2u : 0u)) { fail("jgt64 %llx %llx", (unsigned long long)x, (unsigned long long)y); return; }
		}
	}
	// ランダム（i 形：符号拡張 imm32 比。MEG の ±0x8000 / ±0x800000 系）
	static const u32 consts[] = { 0, 1, u32(-1), 0x8000, u32(s32(-0x8000)), 0x800000, u32(s32(-0x800001)), 0x7ffffff };
	fn0_t fl[8], fg[8];
	for (int k = 0; k < 8; k++) {
		assembler a;
		head(a);
		inp(a, 0, RCX); inp(a, 1, RDX);
		g_out[0] = 0;
		a.jlt64i(RCX, RDX, consts[k], [&](void) {
			a.imm32(RDI, u32(uintptr_t(&g_out[0])));
			a.store32i(mem{ RDI, NOREG, 1, 0 }, 1);
		});
		a.ret();
		void *p = run("jlt64i", a);
		if (!p) return;
		fl[k] = (fn0_t)p;

		assembler b;
		head(b);
		inp(b, 0, RCX); inp(b, 1, RDX);
		g_out[0] = 0;
		b.jgt64i(RCX, RDX, consts[k], [&](void) {
			b.imm32(RDI, u32(uintptr_t(&g_out[0])));
			b.store32i(mem{ RDI, NOREG, 1, 0 }, 1);
		});
		b.ret();
		p = run("jgt64i", b);
		if (!p) return;
		fg[k] = (fn0_t)p;
	}
	for (int i = 0; i < 80000; i++) {
		u64 x;
		switch (i % 4) {
		case 0: x = pair(); break;
		case 1: x = u64(s64(s32(rnd()))); break;
		case 2: x = u64(rnd() % 0x1000000) * (i % 3 ? 1 : u64(1) << 23); break;
		default: x = u64(s64(rnd() % 0x40001 - 0x20000)) << (i % 30); break;
		}
		g_in[0] = u32(x); g_in[1] = u32(x >> 32);
		const int k = i % 8;
		g_out[0] = 0;
		if (call0(fl[k])) return;
		if (bool(g_out[0]) != ((s64)x < s64(s32(consts[k])))) { fail("jlt64i %llx %x", (unsigned long long)x, consts[k]); return; }
		g_out[0] = 0;
		if (call0(fg[k])) return;
		if (bool(g_out[0]) != ((s64)x > s64(s32(consts[k])))) { fail("jgt64i %llx %x", (unsigned long long)x, consts[k]); return; }
	}
	for (int k = 0; k < 8; k++) {
		VirtualFree((void *)fl[k], 0, MEM_RELEASE);
		VirtualFree((void *)fg[k], 0, MEM_RELEASE);
	}
	g_pass++;
}

static void t_test64(void)
{
	// ZF は hi 跨ぎの 0 判定に常に正しい。符号は test32(hi)+setl（MEG の latch の形）
	assembler a;
	head(a);
	inp(a, 0, RCX); inp(a, 1, RDX);
	a.test64(RCX, RDX, RCX, RDX);
	a.imm32(RAX, u32(uintptr_t(&g_out[0])));
	a.sete_mem(mem{ RAX, NOREG, 1, 0 });        // flag_z の形
	a.test32(RDX, RDX);
	a.setcc(0x9c, RCX);                         // setl＝SF＝bit31 hi＝値の符号（flag_n の形）
	a.store8(mem{ RAX, NOREG, 1, 1 }, RCX);
	a.ret();
	void *p = run("test64", a);
	if (!p) return;
	const fn0_t f = (fn0_t)p;
	static const u64 vs[] = { 0, 1, ~u64(0), u64(0x80000000), u64(0xffffffff), u64(s64(-0x8000)),
		                      u64(0x100000000ull), u64(0x7fffffff), u64(1) << 63, u64(0x80000001) };
	for (u64 x : vs) {
		g_in[0] = u32(x); g_in[1] = u32(x >> 32);
		if (call0(f)) return;
		if (((g_out[0] & 0xff) != 0) != (x == 0) || bool(g_out[0] & 0x100) != ((s64)x < 0)) { fail("test64 %llx", (unsigned long long)x); return; }
	}
	for (int i = 0; i < 80000; i++) {
		u64 x = pair();
		if (i % 4 == 0) x = pair() & 0xffffffff;        // hi が 0 で lo の bit31 が立つ帯（旧形が崩れる所）
		g_in[0] = u32(x); g_in[1] = u32(x >> 32);
		if (call0(f)) return;
		if (((g_out[0] & 0xff) != 0) != (x == 0) || bool(g_out[0] & 0x100) != ((s64)x < 0)) { fail("test64 rnd %llx", (unsigned long long)x); return; }
	}
	VirtualFree(p, 0, MEM_RELEASE);
	g_pass++;
}

static void t_imul64(void)
{
	// (EAX,ECX) * (EBX,EBP) → 下 64bit（EAX,ECX）
	assembler a;
	head(a);
	inp(a, 1, RCX); inp(a, 2, RBX); inp(a, 3, RBP);
	inp(a, 0, RAX);
	a.imul64(RAX, RCX, RBX, RBP);
	a.push(RAX);
	a.imm32(RAX, u32(uintptr_t(&g_out[0])));
	a.pop(RDX);
	a.store32(mem{ RAX, NOREG, 1, 0 }, RDX);
	a.store32(mem{ RAX, NOREG, 1, 4 }, RCX);
	a.ret();
	want_seq("mul-full", a.code, { 0xf7, 0xe3 });       // mul ebx（F7 /4 符号無し）
	void *p = run("imul64", a);
	if (!p) return;
	const fn0_t f = (fn0_t)p;
	static const u64 e[] = { 0, 1, ~u64(0), u64(1) << 31, u64(0x7fffffff), u64(1) << 32, u64(1) << 63, u64(0x8000) };
	for (int i = 0; i < 200000; i++) {
		u64 x, y;
		if (i < 8) { x = e[i]; y = e[7 - i]; }
		else switch (i % 4) {
		case 0: x = pair(); y = pair(); break;
		case 1: x = u64(rnd() & 0x7ffc) << 23; y = u64(u32(s32(rnd()))); break;   // MEG: m<<23 × s32（lo の bit31 が立つ）
		case 2: x = u64(s64(s32(rnd()))); y = i % 8 ? 0x1fffff : ~u64(0); break;
		default: x = u64(rnd() & 0x1ff) << 38; y = (u64(rnd() & 0x7fffffff) | (u64(rnd() & 1) << 32)) * (i % 16 ? 1 : 3); break;
		}
		g_in[0] = u32(x); g_in[1] = u32(x >> 32);
		g_in[2] = u32(y); g_in[3] = u32(y >> 32);
		if (call0(f)) return;
		u64 r = u64(g_out[0]) | (u64(g_out[1]) << 32);
		if (r != x * y) { fail("imul64 %llx*%llx=%llx want %llx", (unsigned long long)x, (unsigned long long)y, (unsigned long long)r, (unsigned long long)(x * y)); return; }
	}
	VirtualFree(p, 0, MEM_RELEASE);
	g_pass++;
}

static void t_imul64i(void)
{
	assembler a;
	head(a);
	inp(a, 1, RCX);
	inp(a, 0, RAX);
	a.imul64i(RAX, RCX, 0x1fffff);
	a.push(RAX);
	a.imm32(RAX, u32(uintptr_t(&g_out[0])));
	a.pop(RDX);
	a.store32(mem{ RAX, NOREG, 1, 0 }, RDX);
	a.store32(mem{ RAX, NOREG, 1, 4 }, RCX);
	a.ret();
	want_seq("mul-full-ecx", a.code, { 0xf7, 0xe1 });   // mul ecx
	void *p = run("imul64i", a);
	if (!p) return;
	const fn0_t f = (fn0_t)p;
	for (int i = 0; i < 100000; i++) {
		u64 x;
		switch (i % 3) {
		case 0: x = u64(rnd() & 0x7ffc) << 23; break;    // MEG の m1<<23（lo の bit31 が立つ）
		case 1: x = u64(s64(s32(rnd()))); break;
		default: x = pair(); break;
		}
		if (i < 4) x = (i == 0 ? 0 : i == 1 ? u64(1) << 31 : i == 2 ? u64(0x7ffc) << 23 : ~u64(0));
		g_in[0] = u32(x); g_in[1] = u32(x >> 32);
		if (call0(f)) return;
		u64 r = u64(g_out[0]) | (u64(g_out[1]) << 32);
		u64 want = x * u64(s64(s32(0x1fffff)));
		if (r != want) { fail("imul64i %llx -> %llx want %llx", (unsigned long long)x, (unsigned long long)r, (unsigned long long)want); return; }
	}
	VirtualFree(p, 0, MEM_RELEASE);
	g_pass++;
}

static u32 __cdecl add3(u32 a, u32 b)
{
	return a + b + 7;
}

static void t_call_abs(void)
{
	// cdecl：右から左へ push → call_abs（B8 id / FF D0）→ add esp,8
	assembler a;
	head(a);
	inp(a, 0, RDX);
	inp(a, 1, RBP);
	a.push(RBP);
	a.push(RDX);
	a.call_abs((void *)&add3);
	a.addrsp(8);
	a.imm32(RBX, u32(uintptr_t(&g_out[0])));
	a.store32(mem{ RBX, NOREG, 1, 0 }, RAX);
	a.ret();
	want_seq("call-abs", a.code, { 0xb8, -1, -1, -1, -1, 0xff, 0xd0 });
	void *p = run("call_abs", a);
	if (!p) return;
	g_in[0] = 10; g_in[1] = 25;
	if (call0((fn0_t)p)) return;
	VirtualFree(p, 0, MEM_RELEASE);
	if (g_out[0] != 42) { fail("call_abs got %u", g_out[0]); return; }
	g_pass++;
}

static void t_pushpop(void)
{
	assembler b;
	head(b);
	inp(b, 0, RDI);
	b.push(RAX); b.push(RCX); b.push(RDX); b.push(RBX);
	b.push(RBP); b.push(RSI); b.push(RDI);
	b.pop(RDI); b.pop(RSI); b.pop(RBP); b.pop(RBX);
	b.pop(RDX); b.pop(RCX); b.pop(RAX);
	b.imm32(RBX, u32(uintptr_t(&g_out[0])));
	b.store32(mem{ RBX, NOREG, 1, 0 }, RDI);
	b.ret();
	want_seq("push-edi", b.code, { 0x57 });
	want_seq("pop-edi", b.code, { 0x5f });
	void *p = run("pushpop", b);
	if (!p) return;
	g_in[0] = 0x7777;
	if (call0((fn0_t)p)) return;
	VirtualFree(p, 0, MEM_RELEASE);
	if (g_out[0] != 0x7777) { fail("pushpop %08x", g_out[0]); return; }
	g_pass++;
}

static void t_branches(void)
{
	// 手で打つ rel8 の je（ZF=1 で飛ぶ）
	assembler a;
	head(a);
	inp(a, 0, RCX);
	a.xor32(RDX, RDX);                    // ZF=1
	a.byte(0x74);                         // je rel8
	const size_t rel8 = a.code.size();
	a.byte(0);                            // 後で埋める
	a.imm32(RCX, 0xffff);                 // 飛ばされる
	a.code[rel8] = 5;                     // imm32 の分だけ進む
	a.imm32(RBX, u32(uintptr_t(&g_out[0])));
	a.store32(mem{ RBX, NOREG, 1, 0 }, RCX);
	a.ret();
	void *p = run("je-rel8", a);
	if (!p) return;
	g_in[0] = 0x1234;
	if (call0((fn0_t)p)) return;
	VirtualFree(p, 0, MEM_RELEASE);
	if (g_out[0] != 0x1234) { fail("je-rel8 %08x", g_out[0]); return; }

	// jz_fwd + patch（後ろへ飛ぶ）
	assembler c;
	head(c);
	inp(c, 0, RCX);
	c.xor32(RDX, RDX);                    // ZF=1
	const size_t jz = c.jz_fwd();
	c.imm32(RCX, 0xdead);                 // 飛ばされる
	c.patch(jz);
	c.imm32(RBX, u32(uintptr_t(&g_out[0])));
	c.store32(mem{ RBX, NOREG, 1, 0 }, RCX);
	c.ret();
	p = run("jz_fwd", c);
	if (!p) return;
	g_in[0] = 0x2222;
	if (call0((fn0_t)p)) return;
	VirtualFree(p, 0, MEM_RELEASE);
	if (g_out[0] != 0x2222) { fail("jz_fwd %08x", g_out[0]); return; }

	// jmp_fwd + patch_to
	assembler d;
	head(d);
	inp(d, 0, RCX);
	const size_t j = d.jmp_fwd();
	d.imm32(RCX, 0xdead);                 // とばされる
	d.patch_to(j, d.code.size());
	d.imm32(RBX, u32(uintptr_t(&g_out[0])));
	d.store32(mem{ RBX, NOREG, 1, 0 }, RCX);
	d.ret();
	p = run("jmp_fwd", d);
	if (!p) return;
	g_in[0] = 0x3333;
	if (call0((fn0_t)p)) return;
	VirtualFree(p, 0, MEM_RELEASE);
	if (g_out[0] != 0x3333) { fail("jmp_fwd %08x", g_out[0]); return; }
	g_pass++;
}

static void t_pack24(void)
{
	// MEG の pack24 ラムダの 32bit 版の写し。入力 (EAX,ECX)、出力 lo=EAX（hi は符号）
	//  - x64 の and32i は上で 0 に広がるので、lo を mask して hi を 0 にする形で同じにする
	assembler a;
	head(a);
	inp(a, 1, RCX);
	inp(a, 0, RAX);
	a.mov64(RBX, RBP, RAX, RCX);
	a.sar64(RBX, RBP, 63);
	a.and32i(RBX, 0x7fff);
	a.xor32(RBP, RBP);                    // x64 の「上で消える」ところと同じにする
	a.add64(RAX, RCX, RBX, RBP);
	a.sar64(RAX, RCX, 15);
	a.cmp64i(RAX, RCX, 0x800000);
	a.imm32(RBX, 0x7fffff);
	a.imm32(RBP, 0);                      // 注: cmp64 の後の cmove までフラグを守る（xor は駄目。mov は可）
	a.cmove64(RAX, RCX, RBX, RBP);
	a.cmp64i(RAX, RCX, u32(s32(-0x800001)));
	a.imm32(RBX, u32(s32(-0x800000)));
	a.imm32(RBP, 0xffffffffu);
	a.cmove64(RAX, RCX, RBX, RBP);
	a.shl32(RAX, 8);
	a.sar32(RAX, 8);                      // lo だけ（sext24）。x64 では上で 0 に広がるので hi も 0 にする
	a.xor32(RCX, RCX);
	a.push(RAX);
	a.imm32(RAX, u32(uintptr_t(&g_out[0])));
	a.pop(RDX);
	a.store32(mem{ RAX, NOREG, 1, 0 }, RDX);
	a.store32(mem{ RAX, NOREG, 1, 4 }, RCX);
	a.ret();
	void *p = run("pack24", a);
	if (!p) return;
	const fn0_t f = (fn0_t)p;
	auto ref = [](s64 p) -> s64 {
		s64 r = p + (p < 0 ? 0x7fff : 0);
		s64 s = r >> 15;
		if (s == s64(0x800000)) s = 0x7fffff;
		if (s == s64(-0x800001)) s = -0x800000;
		return u64(u32(s32(u32(s) << 8) >> 8));       // meg_pack24 の u32 返り（上 32bit は 0）
	};
	static const s64 ee[] = { 0, 1, -1, 0x800000, -0x800001, 0x7fffff, -0x800000, s64(1) << 38, -(s64(1) << 38), s64(0x8000) };
	for (int i = 0; i < 200000; i++) {
		s64 x;
		if (i < 10) x = ee[i];
		else switch (i % 4) {
		case 0: x = s64(s32(rnd())); break;
		case 1: x = s64(rnd() % 0x20000) * (i % 8 ? 1 : -1); break;
		case 2: x = s64((u64(rnd() & 0x7ffc) << 38) >> 23); break;
		default: x = s64(pair() >> 20); break;
		}
		g_in[0] = u32(u64(x)); g_in[1] = u32(u64(x) >> 32);
		if (call0(f)) return;
		s64 r = s64(u64(g_out[0]) | (u64(g_out[1]) << 32));
		if (r != ref(x)) { fail("pack24 %llx -> %llx want %llx", (unsigned long long)x, (unsigned long long)r, (unsigned long long)ref(x)); return; }
	}
	VirtualFree(p, 0, MEM_RELEASE);
	g_pass++;
}

int main(void)
{
	printf("x64asm32_test (x86-32 mode)\n");
	t_moves();
	t_stores();
	t_imm64();
	t_arith64();
	t_shift64();
	t_neg64();
	t_cmov64();
	t_cmp64i();
	t_jlt64();
	t_test64();
	t_imul64();
	t_imul64i();
	t_call_abs();
	t_pushpop();
	t_branches();
	t_pack24();

	FILE *fp = nullptr;
	if (fopen_s(&fp, "build32-msvc\\x64asm32.bin", "wb") == 0 && fp) {
		fwrite(g_bin.data(), 1, g_bin.size(), fp);
		fclose(fp);
		printf("dump: build32-msvc\\x64asm32.bin (%zu bytes)\n", g_bin.size());
	} else
		printf("dump: skipped (run from repo root)\n");

	printf("pass %d  fail %d\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
