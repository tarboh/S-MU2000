// license:BSD-3-Clause
// copyright-holders:David Haywood

#ifndef MAME_CPU_SH_SH_H
#define MAME_CPU_SH_SH_H

#pragma once

// S-MU2000: MAME 本体の代わりに互換層とバスを使う
#include "state.h"
#include "../../compat/mamecompat.h"
#include "../../compat/membus.h"



/***************************************************************************
    DEBUGGING
**************************************************************************/

#define DISABLE_FAST_REGISTERS              (0) // set to 1 to turn off usage of register caching
#define SINGLE_INSTRUCTION_MODE             (0)

#define SET_EA                      (0) // makes slower but "shows work" in the EA fake register like the interpreter

#if SET_EA
#define SETEA(x) UML_MOV(block, mem(&m_sh2_state->ea), ireg(x))
#else
#define SETEA(x)
#endif

/***************************************************************************
    CONSTANTS
***************************************************************************/

/* speed up delay loops, bail out of tight loops (can cause timer issues) */
#define BUSY_LOOP_HACKS 0

#define SH2DRC_STRICT_VERIFY    0x0001          /* verify all instructions */
#define SH2DRC_FLUSH_PC         0x0002          /* flush the PC value before each memory access */
#define SH2DRC_STRICT_PCREL     0x0004          /* do actual loads on MOVLI/MOVWI instead of collapsing to immediates */

#define SH2DRC_COMPATIBLE_OPTIONS   (SH2DRC_STRICT_VERIFY | SH2DRC_FLUSH_PC | SH2DRC_STRICT_PCREL)
#define SH2DRC_FASTEST_OPTIONS  (0)

#define SH2_MAX_FASTRAM       4

/* map variables */
#define MAPVAR_PC                   M0
#define MAPVAR_CYCLES               M1

#define PROBE_ADDRESS               ~0

#define CPU_TYPE_SH1    (0)
#define CPU_TYPE_SH2    (1)
#define CPU_TYPE_SH3    (2)
#define CPU_TYPE_SH4    (3)

#define REG_N  ((opcode >> 8) & 15)
#define REG_M  ((opcode >> 4) & 15)

/* Bits in SR */
#define SH_T   0x00000001
#define SH_S   0x00000002
#define SH_I   0x000000f0
#define SH_Q   0x00000100
#define SH_M   0x00000200

#define SH_FLAGS   (SH_M|SH_Q|SH_I|SH_S|SH_T)

// Special meanings for the delay slot PC value.  Odd numbers were chosen because they're not valid otherwise.
#define SH_OVRPC_NONE       0xffffffff  // We aren't in a delay slot, use desc->pc
#define SH_OVRPC_DYNAMIC    0xfffffffd  // In the delay slot of a register-indirect branch, base is runtime m_sh2_state->target

/* SR shift values */
#define T_SHIFT 0
#define S_SHIFT 1
#define I_SHIFT 4
#define Q_SHIFT 8
#define M_SHIFT 9

/***************************************************************************
    MACROS
***************************************************************************/

#define SH2_CODE_XOR(a)     ((a) ^ NATIVE_ENDIAN_VALUE_LE_BE(2, 0)) // sh2
#define SH34LE_CODE_XOR(a)  ((a) ^ NATIVE_ENDIAN_VALUE_LE_BE(0, 6)) // naomi
#define SH34BE_CODE_XOR(a)  ((a) ^ NATIVE_ENDIAN_VALUE_LE_BE(6, 0)) // cave


enum
{
	SH4_PC = 1, SH_SR, SH4_PR, SH4_GBR, SH4_VBR, SH4_DBR, SH4_MACH, SH4_MACL,
	SH4_R0, SH4_R1, SH4_R2, SH4_R3, SH4_R4, SH4_R5, SH4_R6, SH4_R7,
	SH4_R8, SH4_R9, SH4_R10, SH4_R11, SH4_R12, SH4_R13, SH4_R14, SH4_R15, SH4_EA, SH4_SP
};

// S-MU2000: cpu_device をやめ、互換層の device_t を継承する
class sh_common_execution : public device_t
{

public:
	// 状態の保存と復元（src/state.h）
	void state(state_io &s);

	// Data that needs to be stored close to the generated DRC code
	struct internal_sh2_state
	{
		uint32_t  pc = 0;
		uint32_t  pr = 0;
		uint32_t  sr = 0;
		uint32_t  mach = 0;
		uint32_t  macl = 0;
		uint32_t  r[16] = {};
		uint32_t  ea = 0;

		uint32_t  pending_irq = 0;
		uint32_t  pending_nmi = 0;
		int32_t   irqline = 0;
		uint32_t  evec = 0;               // exception vector for DRC
		uint32_t  irqsr = 0;              // IRQ-time old SR for DRC
		uint32_t  target = 0;             // target for jmp/jsr/etc so the delay slot can't kill it
		int     internal_irq_level = 0;
		int     icount = 0;
		uint8_t   sleep_mode = 0;
		uint32_t  arg0 = 0;              /* print_debug argument 1 */
		uint32_t  arg1 = 0;
		uint32_t  gbr = 0;
		uint32_t  vbr = 0;

		uint32_t  m_delay = 0;

		// SH3/4 additional DRC "near" state
		uint32_t  m_ppc = 0;
		uint32_t  m_spc = 0;
		uint32_t  m_ssr = 0;
		uint32_t  m_rbnk[2][8] = {};
		uint32_t  m_sgr = 0;
		uint32_t  m_fr[16] = {};
		uint32_t  m_xf[16] = {};
		uint32_t  m_cpu_off = 0;
		uint32_t  m_pending_irq = 0;
		uint32_t  m_test_irq = 0;
		uint32_t  m_fpscr = 0;
		uint32_t  m_fpul = 0;
		uint32_t  m_dbr = 0;

		// SH3/4 floating point constants the generated code refers to by address
		double  m_ftrc_dmin = 0;         // FTRC double-precision range check
		double  m_ftrc_dmax = 0;
		float   m_ftrc_smin = 0;         // FTRC single-precision range check
		float   m_ftrc_smax = 0;
		float   m_fzero = 0;             // FTRV accumulator initialiser
		float   m_fone = 0;              // FSRRA reciprocal numerator
		uint8_t m_fpmode[4] = {};         // FPSCR.RM -> UML rounding mode

		int     m_frt_input = 0;
		int     m_fpu_sz = 0;
		int     m_fpu_pr = 0;
	};

	internal_sh2_state *m_sh2_state;

	// MAME では device_state_interface が持っていた。周辺がログに出すのに使う
	u32 pc() const { return m_sh2_state->pc; }
	// 呼び出し元を辿るため（調べもの用）
	u32 pr() const { return m_sh2_state->pr; }

	// 移植の突き合わせ用。レジスタの状態を 1 つの値に畳む（安い方）
	u64 regs_hash() const
	{
		u64 h = 0;
		for (int i = 0; i < 16; i++) h = h * 1000003 ^ m_sh2_state->r[i];
		h = h * 1000003 ^ m_sh2_state->sr;
		h = h * 1000003 ^ m_sh2_state->pr;
		h = h * 1000003 ^ m_sh2_state->gbr;
		h = h * 1000003 ^ m_sh2_state->mach;
		h = h * 1000003 ^ m_sh2_state->macl;
		return h;
	}

	// 移植の突き合わせ用。レジスタの状態を文字にする。
	// MAME 側にも同じものを入れてあるので、最初に食い違う命令が分かる
	const char *regs_text() const
	{
	        static char buf[256];
	        int n = 0;
	        for (int i = 0; i < 16; i++) {
	                if (n >= static_cast<int>(sizeof(buf)))
	                        break;
	                int written = std::snprintf(buf + n, sizeof(buf) - n,
	                                            " %08X", m_sh2_state->r[i]);
	                if (written < 0 || written >= static_cast<int>(sizeof(buf) - n))
	                        break;
	                n += written;
	        }
	        if (n < static_cast<int>(sizeof(buf))) {
	                int written = std::snprintf(buf + n, sizeof(buf) - n,
	                                            " SR=%08X PR=%08X MACH=%08X MACL=%08X",
	                                            m_sh2_state->sr, m_sh2_state->pr,
	                                            m_sh2_state->mach, m_sh2_state->macl);
	                if (written >= 0 && written < static_cast<int>(sizeof(buf) - n))
	                        n += written;
	        }
	        return buf;
	}

	// S-MU2000: MAME は device_memory_interface でバスを持っていた。
	// こちらは組み立て側が作った mem_bus を差し込む
	void set_program_bus(mem_bus *b) { m_program = b; m_decrypted_program = b; }

	// S-MU2000: 経過サイクル。
	// MAME は machine().time() から逆算していたが、こちらはホストの時計を持たない。
	// CPU が自分で数え、周辺のタイマ（MTU / CMT）はこの値を基準に動く。
	virtual void execute_run() = 0;

	// 呼び出し側はこれで走らせる。実際に進んだサイクル数を返す。
	// 途中で周辺が予定を入れると、要求より早く戻ってくる
	int run_cycles(int cycles)
	{
		m_sh2_state->icount = cycles;
		m_cycles_this_run   = cycles;
		execute_run();
		const int done = m_cycles_this_run - m_sh2_state->icount;
		m_total_cycles     += done;
		m_cycles_this_run   = 0;
		m_sh2_state->icount = 0;
		return done;
	}

	// MAME の abort_timeslice。周辺が新しい予定を入れたとき、スケジューラが
	// 組み直せるよう CPU をその場で止める。使わなかったぶんは経過に数えない
	void abort_timeslice()
	{
		m_cycles_this_run  -= m_sh2_state->icount;
		m_sh2_state->icount = 0;
	}

	// S-MU2000: 命令を進めずに時間だけ進める（バスの WAIT で CPU が止まっている間）。走行中には呼ばない
	void skip_cycles(u64 n) { m_total_cycles += n; }

	// 走行中に呼ばれても正しい値になる（MAME の total_cycles と同じ勘定）
	u64 total_cycles() const
	{
		return m_total_cycles + (m_cycles_this_run - m_sh2_state->icount);
	}

	u64 m_total_cycles    = 0;
	int m_cycles_this_run = 0;

	virtual uint8_t read_byte(offs_t offset) = 0;
	virtual uint16_t read_word(offs_t offset) = 0;
	virtual uint32_t read_long(offs_t offset) = 0;
	virtual uint16_t decrypted_read_word(offs_t offset) = 0;
	virtual void write_byte(offs_t offset, uint8_t data) = 0;
	virtual void write_word(offs_t offset, uint16_t data) = 0;
	virtual void write_long(offs_t offset, uint32_t data) = 0;

	virtual void set_frt_input(int state) = 0;
	void pulse_frt_input() { set_frt_input(ASSERT_LINE); set_frt_input(CLEAR_LINE); }

protected:
	// compilation boundaries -- how far back/forward does the analysis extend?
	enum : u32
	{
		COMPILE_BACKWARDS_BYTES     = 64,
		COMPILE_FORWARDS_BYTES      = 256,
		COMPILE_MAX_INSTRUCTIONS    = (COMPILE_BACKWARDS_BYTES / 2) + (COMPILE_FORWARDS_BYTES / 2),
		COMPILE_MAX_SEQUENCE        = 64
	};

	// size of the execution code cache
	enum : size_t
	{
		CACHE_SIZE                  = 32 * 1024 * 1024
	};

	// exit codes
	enum : int
	{
		EXECUTE_OUT_OF_CYCLES       = 0,
		EXECUTE_MISSING_CODE        = 1,
		EXECUTE_UNMAPPED_CODE       = 2,
		EXECUTE_RESET_CACHE         = 3
	};

	class frontend;
	class opcode_desc;

	// S-MU2000: MAME のデバイス生成の引数を落とした
	sh_common_execution() = default;

	// MAME は DRC が生成したコードの近くに置くため drc_cache から取っていた。
	// DRC を使わないので、ただのメンバでよい
	internal_sh2_state m_sh2_state_storage{};

	void ADD(uint32_t m, uint32_t n);
	void ADDI(uint32_t i, uint32_t n);
	void ADDC(uint32_t m, uint32_t n);
	void ADDV(uint32_t m, uint32_t n);
	void AND(uint32_t m, uint32_t n);
	void ANDI(uint32_t i);
	void ANDM(uint32_t i);
	void BF(uint32_t d);
	void BFS(uint32_t d);
	void BRA(uint32_t d);
	void BRAF(uint32_t m);
	void BSR(uint32_t d);
	void BSRF(uint32_t m);
	void BT(uint32_t d);
	void BTS(uint32_t d);
	void CLRMAC();
	void CLRT();
	void CMPEQ(uint32_t m, uint32_t n);
	void CMPGE(uint32_t m, uint32_t n);
	void CMPGT(uint32_t m, uint32_t n);
	void CMPHI(uint32_t m, uint32_t n);
	void CMPHS(uint32_t m, uint32_t n);
	void CMPPL(uint32_t n);
	void CMPPZ(uint32_t n);
	void CMPSTR(uint32_t m, uint32_t n);
	void CMPIM(uint32_t i);
	void DIV0S(uint32_t m, uint32_t n);
	void DIV0U();
	void DIV1(uint32_t m, uint32_t n);
	void DMULS(uint32_t m, uint32_t n);
	void DMULU(uint32_t m, uint32_t n);
	void DT(uint32_t n);
	void EXTSB(uint32_t m, uint32_t n);
	void EXTSW(uint32_t m, uint32_t n);
	void EXTUB(uint32_t m, uint32_t n);
	void EXTUW(uint32_t m, uint32_t n);
	void JMP(uint32_t m);
	void JSR(uint32_t m);
	void LDCGBR(uint32_t m);
	void LDCVBR(uint32_t m);
	void LDCMGBR(uint32_t m);
	void LDCMVBR(uint32_t m);
	void LDSMACH(uint32_t m);
	void LDSMACL(uint32_t m);
	void LDSPR(uint32_t m);
	void LDSMMACH(uint32_t m);
	void LDSMMACL(uint32_t m);
	void LDSMPR(uint32_t m);
	void MAC_L(uint32_t m, uint32_t n);
	void MAC_W(uint32_t m, uint32_t n);
	void MOV(uint32_t m, uint32_t n);
	void MOVBS(uint32_t m, uint32_t n);
	void MOVWS(uint32_t m, uint32_t n);
	void MOVLS(uint32_t m, uint32_t n);
	void MOVBL(uint32_t m, uint32_t n);
	void MOVWL(uint32_t m, uint32_t n);
	void MOVLL(uint32_t m, uint32_t n);
	void MOVBM(uint32_t m, uint32_t n);
	void MOVWM(uint32_t m, uint32_t n);
	void MOVLM(uint32_t m, uint32_t n);
	void MOVBP(uint32_t m, uint32_t n);
	void MOVWP(uint32_t m, uint32_t n);
	void MOVLP(uint32_t m, uint32_t n);
	void MOVBS0(uint32_t m, uint32_t n);
	void MOVWS0(uint32_t m, uint32_t n);
	void MOVLS0(uint32_t m, uint32_t n);
	void MOVBL0(uint32_t m, uint32_t n);
	void MOVWL0(uint32_t m, uint32_t n);
	void MOVLL0(uint32_t m, uint32_t n);
	void MOVI(uint32_t i, uint32_t n);
	void MOVWI(uint32_t d, uint32_t n);
	void MOVLI(uint32_t d, uint32_t n);
	void MOVBLG(uint32_t d);
	void MOVWLG(uint32_t d);
	void MOVLLG(uint32_t d);
	void MOVBSG(uint32_t d);
	void MOVWSG(uint32_t d);
	void MOVLSG(uint32_t d);
	void MOVBS4(uint32_t d, uint32_t n);
	void MOVWS4(uint32_t d, uint32_t n);
	void MOVLS4(uint32_t m, uint32_t d, uint32_t n);
	void MOVBL4(uint32_t m, uint32_t d);
	void MOVWL4(uint32_t m, uint32_t d);
	void MOVLL4(uint32_t m, uint32_t d, uint32_t n);
	void MOVA(uint32_t d);
	void MOVT(uint32_t n);
	void MULL(uint32_t m, uint32_t n);
	void MULS(uint32_t m, uint32_t n);
	void MULU(uint32_t m, uint32_t n);
	void NEG(uint32_t m, uint32_t n);
	void NEGC(uint32_t m, uint32_t n);
	void NOP(void);
	void NOT(uint32_t m, uint32_t n);
	void OR(uint32_t m, uint32_t n);
	void ORI(uint32_t i);
	void ORM(uint32_t i);
	void ROTCL(uint32_t n);
	void ROTCR(uint32_t n);
	void ROTL(uint32_t n);
	void ROTR(uint32_t n);
	void RTS();
	void SETT();
	void SHAL(uint32_t n);
	void SHAR(uint32_t n);
	void SHLL(uint32_t n);
	void SHLL2(uint32_t n);
	void SHLL8(uint32_t n);
	void SHLL16(uint32_t n);
	void SHLR(uint32_t n);
	void SHLR2(uint32_t n);
	void SHLR8(uint32_t n);
	void SHLR16(uint32_t n);
	void SLEEP();
	void STCSR(uint32_t n);
	void STCGBR(uint32_t n);
	void STCVBR(uint32_t n);
	void STCMSR(uint32_t n);
	void STCMGBR(uint32_t n);
	void STCMVBR(uint32_t n);
	void STSMACH(uint32_t n);
	void STSMACL(uint32_t n);
	void STSPR(uint32_t n);
	void STSMMACH(uint32_t n);
	void STSMMACL(uint32_t n);
	void STSMPR(uint32_t n);
	void SUB(uint32_t m, uint32_t n);
	void SUBC(uint32_t m, uint32_t n);
	void SUBV(uint32_t m, uint32_t n);
	void SWAPB(uint32_t m, uint32_t n);
	void SWAPW(uint32_t m, uint32_t n);
	void TAS(uint32_t n);
	void TST(uint32_t m, uint32_t n);
	void TSTI(uint32_t i);
	void TSTM(uint32_t i);
	void XOR(uint32_t m, uint32_t n);
	void XORI(uint32_t i);
	void XORM(uint32_t i);
	void XTRCT(uint32_t m, uint32_t n);

	void op0010(uint16_t opcode);
	void op0011(uint16_t opcode);
	void op0110(uint16_t opcode);
	void op1000(uint16_t opcode);
	void op1100(uint16_t opcode);

	void execute_one(const uint16_t opcode);

	virtual void execute_one_0000(uint16_t opcode);
	virtual void execute_one_4000(uint16_t opcode);
	virtual void execute_one_f000(uint16_t opcode) = 0;

	virtual void RTE() = 0;
	virtual void LDCSR(const uint16_t opcode) = 0;
	virtual void LDCMSR(const uint16_t opcode) = 0;
	virtual void TRAPA(uint32_t i) = 0;
	virtual void ILLEGAL() = 0;


public:
	/* fast RAM */
	uint32_t              m_fastram_select = 0;
	struct
	{
		offs_t              start = 0;                      /* start of the RAM block */
		offs_t              end = 0;                        /* end of the RAM block */
		bool                readonly = false;                   /* true if read-only */
		void *              base;                       /* base in memory where the RAM lives */
	} m_fastram[SH2_MAX_FASTRAM];

	int m_pcfsel = 0;                 // last pcflush entry set
	uint32_t m_pcflushes[16] = {};           // pcflush entries




	// S-MU2000: m_pr16 / m_prptr は DRC フロントエンド専用だったので削除した
	// S-MU2000: address_space の代わりに mem_bus を指す。
	// 命令フェッチ用の別空間（m_decrypted_program）は MU2000 では同じもの。
	mem_bus *m_program = nullptr;
	mem_bus *m_decrypted_program = nullptr;


	/* internal stuff */
	uint8_t               m_cache_dirty = 0;                /* true if we need to flush the cache */

	/* register mappings */



	/* internal compiler state */

	virtual void sh2_exception(const char *message, int irqline) { fatalerror("sh2_exception in base classs\n"); }






	int m_cpu_type = 0;
	uint32_t m_am = 0;

	void sh2drc_set_options(uint32_t options);
	void sh2drc_add_pcflush(offs_t address);

	// S-MU2000: get_desclist は DRC フロントエンド専用だったので削除した

	uint32_t epc(const opcode_desc *desc);


protected:
	// device_t implementation
	virtual void device_start();
};

#endif // MAME_CPU_SH_SH_H
