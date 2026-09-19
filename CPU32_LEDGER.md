# CPU32_LEDGER.md — handoff: x86-32 JIT backend (session restart dump)

> **STATUS 2026-09-16: COMPLETE.** All Phases 1–8 done — see **Phase 8 result + session summary**
> at the end of this file. The "No source files have been modified yet" note below is the original
> session-start snapshot, kept as historical record.

Task: Win32 (32-bit) VST2 builds are CPU-heavy because both JITs are `#if defined(_WIN32) &&
defined(__x86_64__)`-gated → interpreter only. Plan (approved by user): **dual-mode assembler +
both JITs ported to x86-32, SH-2 first**, and fix the guard so MSVC x64 (`_M_X64`) also gets the JIT.
**No source files have been modified yet.** Only build artifacts: `build/` (x64 objs + verify.exe,
green baseline) and `build32/` (partial, broken toolchain).

## Approved decisions
- Scope: **both JITs, SH-2 first** (SH-2 port is mechanical; MEG is the big one).
- Guard fix: **yes** — `__x86_64__ || _M_X64 || __i386__ || _M_IX86`; MSVC x64 gets JIT too.
- Order: guards → x64asm dual-mode → SH2 port + A/B → MEG port + A/B → MSVC/Win32 + probe → docs.

## Todo list state at interruption
1. [completed] Toolchains + baseline builds — **x64 GREEN** (MINGW64 g++ 16.2, `make -j8
   build/verify.exe` linked + run = golden). **WIN32 GREEN via MSVC x86** (`tools/msvc32_build.ps1`:
   verify bit-exact vs `tests/verify.txt`, render `drums` pcm_sha1 = golden, statetest 50-step
   state-exact; all exes `dumpbin` 14C x86). No source edits were needed.
2. [completed] Dual-mode `src/compat/x64asm.h` (32-bit encodings, pair emitters, guard fix) — see **Phase 2 result** below
3. [completed] Port SH-2 JIT to x86-32 (preamble, reg map, cdecl calls)
4. [completed] Validate SH2 win32 bit-exact (verify/statetest/render A/B) — **Phase 4, all green**
5. [completed] Port MEG JIT to x86-32 (pair accumulator, ALU chains, preamble) — **Phase 5, all green**
6. [completed] Validate MEG win32 bit-exact (selftest + render A/B) — **Phase 6, full matrix green**
7. [completed] MSVC Win32 + MSVC x64 builds + `vst2_host_probe` CPU check — **Phase 7, all green; zero source fixes**
8. [completed] Update AGENTS.md / VST2_LEDGER.md / CMakePresets + mingw_compat comments — **Phase 8, docs synced 2026-09-16**

## 32-bit toolchain problem — RESOLVED (decision: Win32 harness = **MSVC x86**)
- `C:\msys64\mingw32\bin\g++.exe` (16.2.0) is dead: cc1plus loads but silently no-ops (exit 0,
  zero output, no .obj). `C:\msys64\clang32\bin` is empty. **Do NOT try to repair**: never write
  anything under `C:\msys64` (no pacman, no file moves; its `/tmp` is fine for scratch). If gcc
  must ever be invoked via msys2_shell it needs a script file:
  `& "C:\msys64\msys2_shell.cmd" -mingw32 -defterm -no-start -here -c "bash /c/<path>.sh"`.
- Working Win32 harness: `tools/msvc32_build.ps1` (VS 18 Community, `vcvarsall.bat amd64_x86`
  env capture, then `cl` directly). Engine list = Makefile OBJS; tool mains mirror Makefile link
  lines (verify does NOT link mu2000). Objs in `build32-msvc/obj` (flat), exes `build32-msvc/`.
  Engine compiled clean on MSVC x86 first try (only C4805 noise in sh_adc/sh_sci). Win32 runs
  interpreter-only (JITs `#if`'d out) and is already bit-exact: verify == `tests/verify.txt`,
  render drums pcm_sha1 == `tests/drums.json` (12.5 s wav in 11 s wall), statetest state-exact.

## Baseline commands (working)
```
# x64 (MinGW64 — the only working gcc env; bash directly, NOT msys2_shell):
C:\msys64\usr\bin\bash.exe -lc 'cd /d/Projects/vst/S-MU2000-vst2-iplug && export PATH=/mingw64/bin:/usr/bin:/bin && make -j8 build/verify.exe && ./build/verify.exe'
# Win32 (MSVC x86; add -Tools verify|render|statetest or -Tools a,b for one):
pwsh -NoProfile -File D:\Projects\vst\S-MU2000-vst2-iplug\tools\msvc32_build.ps1
# win32 golden check (from repo root; same invocation shape as tools/run_tests.py):
python tools/make_test_midi.py build32-msvc/tests
build32-msvc\render.exe roms build32-msvc\tests\drums.mid build32-msvc\tests\drums.wav 4.500 --boot 8.000 -v
python -c "import sys,json;sys.path.insert(0,'tools');import fingerprint as fp;f=fp.make('build32-msvc/tests/drums.wav','build32-msvc/tests/drums.log',8*44100,name='drums',seconds=4.5);print(f['pcm_sha1']==json.load(open('tests/drums.json',encoding='utf-8'))['pcm_sha1'])"
```
NOTE: in pwsh→bash calls use **single-quoted** inner scripts (`$?`/`$PATH` get eaten by pwsh in
double quotes). `BUILD=build32` overrides the legacy build dir for win32 attempts.


## Key findings (all verified by reading source)

### Root cause / hot path
- `swp30_jit.cpp:22` and `sh2_jit.cpp:27`: `#if defined(_WIN32) && defined(__x86_64__)` gate;
  x64asm.h never compiled for Win32 or MSVC-x64 (MSVC has no `__x86_64__`; AGENTS.md documents
  "Win32 = interpreter-only", MinGW-x64 the only JIT consumer).
- SWP30 master+slave (`mu2000.h:217` `m_swpm, m_swps`), each one `meg_state`; MEG runs **0x180=384
  instructions per audio sample per chip** → dominant cost. Per-chip timing already instrumented:
  `swp30.h:79` `m_t_sample`/`m_t_meg` (ns), printf around `mu2000.h:200`/`292`.
- Env kill switches (for A/B): `SMU2000_MEG_JIT=0`, `SMU2000_MEG_BAKE=0`, `SMU2000_MEG_EARLY=0`,
  `SMU2000_SH2_JIT=0` (interpreter) / `=1` (native-off), `SH2_JIT_TRACE=cycles,count,file`.

### x64asm.h (`src/compat/x64asm.h`)
- `rr()/rm()` emit REX only when `rex != 0x40` → with reg<8 and `w=false`, emitted bytes are
  ALREADY valid 32-bit encodings. `rm()` always uses ModRM mod=10/disp32 (safe EBP base) and
  SIB idx=4 for "no index" (valid both modes); ESP base already emits SIB.
- 32-bit-invalid members to guard/replace: all 64-bit ops (`mov64/load64/store64/add64/sub64/
  and64/cmp64/test64/imul64/imul64i/shl64/sar64/neg64/cmov*64/cmp64ri/subrsp/addrsp` with w=true),
  `loads32` (0x63 movsxd), `loads16`, `imm64` (encodes `48 B8 id` — in 32-bit this decodes as
  `dec eax; mov eax,id` → HARD HAZARD), `call_abs` (imm64+call → needs imm32 form: `B8+rd id`),
  `push/pop` (0x41 prefix only needed r>=8 — fine), `bswap32`/`setcc` fine.

### SH-2 JIT (`src/mame/cpu/sh2_jit.cpp`, 820 lines) — port = mechanical
- Pins: RBX=cpu, RSI=state, R12=ROM, R13=RAM; scratch RAX/RCX/RDX, R8=store value.
  32-bit map: CPU=ESI(or EBX), state=EDI, ROM=EBP, RAM=EBX + scratch EAX/ECX/EDX — exactly fits 8.
- ALL native data-path ops already 32-bit (MUL/MULS/MULU → `imul32` truncation matches C++;
  pc/regs/icount/ea all u32). No 64-bit needed except pointer-table spots.
- Arch-dependent sites: `enter`/`next_block` prologue `:269-313` (x64 args rcx/rdx/r8/r9 →
  cdecl `[esp+4,8,12,16]`; no shadow space in 32-bit = simpler); `sizeof(pages[0]) != 8` check
  `:263` (→4); page-table loads scale 8 + `test64` `:300-306` (→4/test32); `imm64` at `:277,299,799`;
  `call` lambda `:353` (call_abs) + arg setup `mov64(RCX,RBX)` before helpers at `:383,404,703,736
  (jit_exec + RDX=op imm32),790` → replace with `push args RTL; call; add esp,N`; `mwrite`
  value-reg R8 (`:399-401`, `store8(mw,R8)` → use a 32-bit scratch, e.g. EDI-free slot, `mov [m],cl`).
- Helpers (all static, cdecl on both toolchains): `jit_exec/rb/rw/rl/wb/ww/wl/irq/trace` `:121-165`.

### MEG JIT (`src/mame/sound/swp30_jit.cpp`, 990 lines) — the real work
- Entry `fn_t(void(*)(meg_state*,swp30_device*,u16* ram))`; x64 prologue `:437` pushes
  RBX,R12,R13,R14,R15,RSI,RDI,RBP + `subrsp(56)`; epilogue `:964-969` (`store64 P` at `:965`).
- Pins `:428`: MS=RBX, SWP=R12, **P(64-bit s64 acc)=R13**, SC=R14(sample ctr), RAM=R15, SEED=RSI,
  K_MAX=RDI(0x7fffff), K_MIN=RBP(-0x800000); P_MAX=R9(0x3fffffffff)/P_MIN=R10(-2^38) reloaded
  after every call (`load_p_limits` `:430`).
- **Planned 32-bit map**: EBX=MS, EDI=SWP, EBP=RAM; scratch EAX/ECX/EDX/ESI; P hi/lo + SEED + SC
  copy in stack frame (`imul eax,[esp+s],imm` / `sub eax,[esp+s]` direct); K/P limits as immediates
  (drops load_p_limits entirely); `call_lfo` `:774-776` → cdecl push(lfo,MS)+`add esp,8` (pins are
  callee-saved, P/SEED survive in frame).
- 64-bit chains to port (the complete list): lambdas `pack24 :452`, `rnd :468`(already 32-bit),
  `p_packed :475`; ALU `:618-705` (m1 `loads16`→pair, `emit_m1_expand`, `shl64(RAX,8+15)`,
  `imul64` `:642`/`imul64i` `:615`, asel `loads32+shl64/sar64 :650-656`, rop add/sub/`neg64+cmovs64`
  `/`and64 :657-667`, `shift shl64 :668`, clamp0 `shl64/sar64 22`, clamp1/2/dflt cmp64+cmovl/g64
  :670-697`, `mov64(P,RAX) :698`, latch `test64+setl/sete_mem :699-704`); `memw sar64 15 :823-825`;
  `index`/`index2` `sar64 23 :832-841`; need_tval pair clamp `±0x8000` via cmp64/cmov `:856-870`
  + branchy copy `:948-957`; R8-address/LFO-table consts `:726,742,912` (→ scratch reg, imm32 ptrs);
  table-read path `:875-892` and everything else (load32/store32/loadu16/jcc/bswap-free) stays.
- Emitted helpers: `emit_revram_encode` clobbers RCX,RDX,**R11**→remap to scratch (ESI);
  `emit_revram_decode` clobbers RCX,RDX,R8; `emit_m1_expand` is pure 32-bit (output ≤0x7ffc ≥0) —
  only its `shl64` caller needs pair work.
- Pair macro designs: sar64(n)=`shrd lo,hi,n; sar hi,n`; shl64(n)=`shld lo,hi,n; shl hi,n`;
  add/sub=`add/adc`; neg64=`neg lo; not hi; sbb hi,0` (verify); imul64 low64 = imul(al*bl full)
  + `add hi, a_lo*b_hi` + `add hi, a_hi*b_lo` (a_hi small for m1<<23, b_hi∈{0,-1} for s32 operand)
  — 3 imul + 2 add; clamp vs ±2^38 = cmp hi vs ±0x40 (±0x3f boundary care) + cmov pair (2 cmov).
- Selftest `meg_jit_selftest :253` (VirtualAlloc RWX; sweeps encode 27-bit, decode 0..0xffff,
  m1 via `fn64(s64)` — 32-bit variant: `u32 fn(u32 lo, s32 hi)` pair-in, EDX:EAX out per MS cdecl).
  Called from `src/verify.cpp:36` → ROM-free gate works on win32 automatically once JIT on.
- gen/spec bake machinery (`:129-249`) and branchy (`skip` var) path are arch-neutral once the
  above emitters are parameterized.

### Build/test harness
- Legacy `make` (MSYS2 g++) = native tools. `make test` → TEST_EXES verify/statetest/render/
  xgtest/samptest + `tools/run_tests.py`; golden `tests/verify.txt`; ROMs present in `roms/`.
  A/B gate: JIT=0 vs JIT=1 renders bit-identical (compare via `tools/compare_wav.py`).
- CMake plugin path: presets vs-win32/vs-x64/ci-win32/ci-win64/mingw-x64/mingw-win32;
  outputs `build-cmake/<api>/<arch>/<Config>/`. `/MT` everywhere. No `/guard:cf` (verified absent)
  → indirect call `c->fn(...)` into JIT buffer is fine. No SSE emitted by either JIT → no
  16-byte-stack-alignment constraint in 32-bit. DEP handled: W then VirtualProtect→EXECUTE_READ
  (MEG `:971-986`); SH2 uses one big PAGE_EXECUTE_READWRITE buf (16MB) — fine on Win32 (<4GB).
- Hard repo rules (AGENTS.md): never touch `iPlug2/` submodule (`git status` shows `m iPlug2` —
  preexisting, leave it); `roms/` never committed/deleted; nothing committed this session.

## Phase 2 result (2026-09-16)

Dual-mode `src/compat/x64asm.h` + guard fix **done and validated**. Previous session's code
compiled and passed **first try** — this session made **no source fixes** (validation only).

### x64-32 API cheat-sheet (`x64asm.h` `#else` branch, `SMU_X64ASM_MODE==32`, lines 198-525)
Mode switch `:25-31` (`__x86_64__||_M_X64`→64, `__i386__||_M_IX86`→32, else `#error`).
- **Regs**: `enum RAX=0..RDI=7, NOREG=0xff` (no R8..R15; `chk(r)` aborts r≥8). Aliases
  `EAX..EDI`. `mem{base,index=NOREG,scale,disp}` + `off(m,d)` (bump disp).
- **`rr`/`rm` lost the `w` param** — shared-mode call sites must `#if` per arch. Never emits REX
  (objdump of full test blob: 0 REX bytes).
- **Plain 32-bit, same shape as x64**: mov32/load32/store32/loadu16/loadu8/loads16(=movsx r32,m16,
  NOT 64-bit)/store16/store8/store8i/store32i/imm32/add32/sub32/and32/or32/xor32/**adc32/sbb32**/
  test32/imul32/imul32i/add32ri/sub32ri/and32i/or32ri/xor32ri/cmp32ri/test32ri/shl32/shr32/sar32/
  rol32/shl32cl/shr32cl/neg32/not32/bswap32/bsr32/movzx8/16/movsx8/16/loads8_32/loads16_32/lea32/
  setcc/setl_mem/sete_mem/add32rm/cmp32rm/test32rm/{add,sub,and,or,xor}32mr/{add,or,and,xor,test,
  shl,shr,cmp,sub}32i_(ri|_mem)/call_reg/push/pop/subrsp/addrsp(cdecl,no shadow)/ret/jcc_fwd/
  jz_fwd/jmp_fwd/patch/patch_to. Dropped on 32-bit: `imm64`, `loads32`(movsxd), all w=1 encoders.
- **New**: `shld(d,s,n)` 0F A4 / `shrd(d,s,n)` 0F AC (imm8).
- **Pair emitters (lo,hi)**: `mov64(d,dhi,s,shi)`; `load64(d,dhi,m)`=[m],[m+4]; `store64(m,s,shi)`;
  `add64`=add/adc, `sub64`=sub/sbb (CF after = hi-side carry, don't rely); `and64` flags after
  undefined; `shl64(d,dhi,n)`=shld+shl (n<32) / mov+shl+xor (n≥32); `sar64`=shrd+sar / mov+sar+sar31.
- **`mov_imm64(d,dhi,u64 v)` bounds**: only `v ≤ 0x7FFFFFFF` or `v ≥ 0xFFFFFFFF80000000`
  (imm32-sign-extend range) else `abort()`; hi = sign of lo. All MEG constants + addresses fit.
- **`neg64(d,dhi)`**: neg lo; not hi; `sbb hi,-1` → SF = value sign → `cmovs64` works after it.
- **cmp64 flags convention (critical)**: `cmp64(a,ahi,b,bhi)` = cmp ahi,bhi / jne / cmp alo,blo.
  At fall-through label: **ZF,CF always correct** (je/jne/ja/jb/jae/jbe fine); **SF/OF only if hi
  differed** → `jl/jg/jle/jge` and `cmovl64/cmovg64` FORBIDDEN after cmp64. Signed branches:
  template `jlt64/jgt64(a,ahi,b,bhi,body)` (emit cmp+jg+jl+cmp+jae branch form, MSVC-style) and
  `jlt64i/jgt64i(a,ahi,imm32-signed,body)`; `cmp64i(a,ahi,bi)` same sign-extend form (test32 if
  bi≥0). `cmovcc64(cc,d,dhi,s,shi)`=two cmovs: `cmovl64/cmovg64` after plain cmp32 only,
  `cmovs64` after neg64/cmp32/test32, `cmove64` after cmp64 (ZF valid).
- **`test64(a,ahi,b,bhi)`**: test hi / jne / test lo — ZF always right; SF only if hi≠0;
  for sign use `test32(ahi,ahi)`+setcc (bit31 of hi IS the sign).
- **`imul64(d,dhi,s,shi)`** (low64): d must be RAX; **clobbers RAX+RDX** always (plus d,dhi);
  s,shi,dhi ∉ {RAX,RDX,RSP}; uses unsigned `mul` for lo·lo (signed would corrupt bit31-set m<<23).
  **`imul64i(d,dhi,v32-signed)`**: d=RAX; additionally **clobbers ECX** — caller pushes ECX if live.
- **`call_abs(fn)`** = `B8+rd imm32; FF D0` — clobbers EAX (same contract as the x64 form).

### Enable macros for Phase 3 / Phase 5 (define in build, per file)
- SH-2 (Phase 3): `/DSMU_JIT32_PORT_SH2` → sets `SMU2000_SH2_JIT`+`SMU2000_SH2_JIT32`.
  Guards: x64 `sh2_jit.cpp:31` (`_M_X64` added → **MSVC x64 now gets the JIT**), 32-bit elif `:33`,
  codegen fork site `:255` (`#if !SMU2000_SH2_JIT || defined(SMU2000_SH2_JIT32)` — split into
  `#if/#elif/#else` when the x86-32 emitters land).
- MEG (Phase 5): `/DSMU_JIT32_PORT_MEG` → `SMU2000_MEG_JIT`+`SMU2000_MEG_JIT32`.
  Guards: x64 `swp30_jit.cpp:26`, 32-bit elif `:28`, x64-only asm helpers `:47`, selftest `:264`,
  build() fork site `:305`.
- Without the macros win32 stays interpreter-only (verified unchanged below).

### Gate results (all green, no fixes)
- **A) MSVC x64** (VS18 `vcvarsall x64`, `/c /std:c++20 /O2 /MT /utf-8 /bigobj /EHsc` + defines,
  scratch objs): sh2_jit.cpp + swp30_jit.cpp + x64asm.h — `EXITCODE=0`, /W3 silent.
- **B) tools/x64asm32_test.cpp** (cl x86 amd64_x86 → `build32-msvc\x64asm32_test.exe`; runs emitted
  code from VirtualAlloc via naked thunk): `pass 16  fail 0`, `EXITCODE=0`. objdump cross-check of
  `build32-msvc\x64asm32.bin` (56879 B, `-b binary -mi8086 -Mintel`): rex **0**, shld 5, shrd 6,
  adc 2, sbb 3, cmov 5, mul 7.
- **x64 golden** (MinGW64 make; JIT objs 02:14 newer than edited sources — binary is fresh):
  tail `MEG の JIT のリバーブ RAM の詰め方と戻し方・係数の広げ方: 食い違い 0`.
- **Win32 golden** (`tools\msvc32_build.ps1 -Tools verify`, 14C x86 re-linked): verify tail
  identical to golden (`ピッチレジスタ … 一致` / `食い違い 0`); JIT still off (no port macro).

### Fixes this session
None — previous session's x64asm.h/JIT guards/tests passed every gate on first compile/run.

## Phase 3 result (2026-09-16) — SH-2 JIT x86-32 port **done, all gates green**

Edited **only `src/mame/cpu/sh2_jit.cpp`** (x64asm.h untouched this phase). Single shared codegen;
x64 output byte-for-byte unchanged (all edits are guard macros / same-value token swaps /
`#if SMU_X64ASM_MODE == 32` additions).

### Register map (32-bit)
EBX=cpu, ESI=state, **EBP=ROM, EDI=RAM**; scratch EAX/ECX/EDX. **ECX doubles as JIT_VAL**
(the x64 R8 store-value role). Alias macros `JIT_ROM/JIT_RAM/JIT_VAL` defined at `:47-58`
(R12/R13/R8 in 64-bit mode → identical x64 emission). Fork site `:271` is now
`#if !SMU2000_SH2_JIT` (stub) `#else` (real codegen, both arches).

### enter/next_block prologue/epilogue (`:304-313` / `:344-374`)
`push ebx,esi,ebp,edi` → args at `[esp+20,24,28,32]` (cdecl [esp+4..16] + 16-byte push shift);
`imm32` entry addr (no imm64, Win32 fits); page-table loads scale 8→4, `test64`→`test32`;
sizeof(pages[0]) check 8→4 (`:292-296`); exit `pop edi,ebp,esi,ebx; ret` — no shadow, no
addrsp, caller cleans args. Stack alignment irrelevant (no SSE emitted).

### Helper call convention (all static members = cdecl both toolchains, **no 64-bit params —
no dword-pair pushes needed anywhere**)
| site | 32-bit sequence |
|---|---|
| mread slow `jit_rb/rw/rl(c,a)` `:450-454` | `push edx; push ebx; call_abs; add esp,8` (result EAX) |
| mwrite slow `jit_wb/ww/wl(c,a,v)` `:466-480` | `push ecx(v); push edx(a); push ebx; call_abs; add esp,12` |
| `jit_exec(c,op)` `:833-842` | `imm32 edx,op; push edx; push ebx; call_abs; add esp,8` |
| `jit_trace(c)` `:794-801` / `jit_irq(c)` `:894-901` | `push ebx; call_abs; add esp,4` |

`call_abs` clobbers EAX (B8 id / FF D0) — never pass args via EAX. mwrite fast path sz2/4 moves
value ECX→EDX (address is dead after the RAM range check; to_slow branches branch before that,
so a,v stay intact in ECX/EDX). `store8(mw, JIT_VAL)` = legacy `mov [edi+eax], cl` (no REX).
The `store_pc_at` byte-insert trick (lazy-pc) is arch-neutral (rel32 jcc/jmp, positions recorded
post-insert) — verified via bit-exact run; MEG branchy path (Phase 5) can reuse it.

### Enable
`:29-38`: 32-bit builds **self-define `SMU_JIT32_PORT_SH2`** (JIT auto-ON in msvc32 harness;
opt-out `SMU_JIT32_NO_SH2`). Runtime kill-switch `SMU2000_SH2_JIT=0` unchanged in
`jit_enabled()`; `=1` (native-off, all-execute_one) also works 32-bit — useful Phase 4 A/B lever.
JIT-live proof hook: `SMU2000_SH2_JIT32_LOG=1` → one stderr line from `jit::init()` `:377-381`
(init is only reachable via jit_run→compile, so it proves real JIT execution).

### Gate results
1. **Win32 build clean** — cl x86 silent, `dumpbin` 14C, all 3 exes relinked.
2. **Win32 JIT-on golden** — verify tail `食い違い 0`; render drums `pcm_sha1` == `tests/drums.json`
   **True** with JIT on; `sh2-jit32: x86-32 prologue emitted` printed (JIT ran through the full
   8 s boot + 4.5 s render). statetest rc=0 `状態は完全に一致`, log identical JIT vs interpreter.
3. **x64 green** — make -j8 + verify = `食い違い 0`, `-Wall` silent; diff audit: the 13 deleted
   lines are comments/guard lines or `R12→JIT_ROM`,`R13→JIT_RAM`,`R8→JIT_VAL` same-token swaps.
4. **A/B smoke** — `SMU2000_SH2_JIT=0` vs unset on win32 verify: identical; stronger: drums.wav
   render A/B byte-identical (Get-FileHash equal).

### Phase-5 warnings
- MSVC x86 C++ unwinding walks the EBP chain; JIT frames keep EBP=ROM and install no frame
  record. MAME helpers here never throw, but any **throwing** helper called from JIT'd code
  (MEG call_lfo path) will break unwinding → keep helpers noexcept or wrap.
- No `push imm32` emitter — spill via `imm32`+`push reg` (jit_exec pattern).
- `-v` does NOT disable the JIT (g_pc_hash/g_pc_trace default null; only boot.cpp --pc-* flags
  set them) — earlier "JIT didn't run" scare was just stderr swallowed by a pwsh pipe; capture
  render stderr to a file when checking logs.
- 16 MB PAGE_EXECUTE_READWRITE buffer fine on Win32 (<<4 GB); `emit_*` helpers untouched.

## Phase 4 result (2026-09-16) — SH-2 win32 JIT **validated bit-exact, no source fixes needed**

Clean rebuild (`Remove-Item build32-msvc\obj` + `msvc32_build.ps1 -Tools verify,render,statetest`,
14C x86, only known C4805 noise). Driver: scratch python per run_tests.py command shapes
(`--boot 8.000 -v`, statetest `--warm 2.0 --steps 50` piano.mid = run_tests first case).

### PASS/FAIL matrix (every comparison bit-identical)
| test | JIT-on sha | JIT-off sha | golden | verdict |
|---|---|---|---|---|
| piano 5.5 s | 7bbb16f75980… | same | 7bbb16f75980… | PASS (wav bytes same) |
| chord 5.0 s | 076dbcdbf84e… | same | 076dbcdbf84e… | PASS |
| drums 4.5 s | 51e409edf4da… | same | 51e409edf4da… | PASS |
| effects 6.0 s | d27e5115a29e… | same | d27e5115a29e… | PASS |
| dense 5.0 s | 5f45eeddd50d… | same | 5f45eeddd50d… | PASS |
| port_b 4.5 s | 154e7f3de7d0… | same | 154e7f3de7d0… | PASS |
| bend 5.5 s | 857d81ebc251… | same | 857d81ebc251… | PASS |
| statetest (50 steps, on/off) | rc=0 状態は完全に一致, logs byte-identical | | | PASS |
| verify (stdout, on/off) | fc.exe /b vs tests\verify.txt: no differences (255 B) | | | PASS |

JIT speed vs interpreter on renders: ~5.6 s vs ~8.5 s wall (≈1.5x). 7/7 + statetest + verify all green
first pass — **zero fixes this phase**.

### Coverage (effects render, 8 s boot + 6 s, temporary env-gated instrumentation in `compile()`, since REVERTED)
- **16,595 blocks compiled, 11,469,991 bytes emitted**, peak `used` 11.47 MB of 16 MB buf.
- **Fallback/compile-failure count = 0**: `compile()` returns null only via bus-check (`:397-403`),
  buf-full flush (`:929`), or init-fail — instrumented counts: skip-bus **0**, buf-full **0** → every
  compile() call produced a block. Remaining interpreter visits are only the *designed* jit_run
  pre-check exits (delay slot / m_test_irq / pc≥ROM_END−0x100 / odd pc / pc-hash), which the
  A/B trace proves are harmless (identical state stream).
- **Trace A/B**: `SH2_JIT_TRACE=240000000,150000,file` on effects, JIT-on vs JIT-off: 150,000
  instructions, cycles 240,000,000→240,179,352 (1.195 cyc/instr, music section ≈8.57 s emulated),
  37,907,503 B traces **byte-identical** (PC+regs+icount+irq state per instruction — no mid-stream
  divergence). Trace emitters are compile-time (`:797`), env set at process start → every block traced.

### Soak + determinism
effects.mid at 3x golden duration (18 s + 8 s boot = 26 s emulated), JIT on, twice: rc=0 both,
12.4 s wall each (≈2.1x faster than realtime), wav sha256 identical (`a376295267e0da8d…`). No crashes.

### x64 regression
`make -j8 build/verify.exe` + run: `食い違い 0`. `build32-msvc\x64asm32_test.exe`: `pass 16 fail 0`.

### EBP/exception audit verdict: **LOW risk for SH-2 JIT**
Helpers `jit_exec/rb/rw/rl/wb/ww/wl/irq/trace` (`sh2_jit.cpp:147-191`) contain no throw/new/vector;
their transitive callees (execute_one interpreter, mem-map device handlers in sh/sci4/hd44780/
port/intc/mtu/dmac/cmt/adc/bsc .cpp) grep **clean** of throw/push_back/resize/new; `fatalerror` →
`abort()` (no unwind); `jit_trace` uses snprintf-to-fixed-buffer + fprintf. All
`std::vector`/`new` in sh2_jit.cpp are in `compile()/init()` — normal C++ caller frames, not under
JIT. **No try/catch anywhere in the non-GUI engine**, so no catch frame above the JIT can unwind
through the EBP=ROM frames; a hypothetical bad_alloc under JIT would terminate (clean kill) rather
than mis-unwind. **Phase-5 caveat stands**: if MEG call_lfo or any future helper ever catches/throws,
add `__declspec(nothrow)` discipline or noexcept wrappers — the EBP chain is still unrecoverable.

### Harness quirks (Phase 5 must-know)
- pwsh `Start-Process -RedirectStandardOutput` gives cp1252 stdout → Japanese prints crash python;
  `sys.stdout.reconfigure(encoding='utf-8')` in drivers (p4_sv.py pattern). Native console OK.
- Never round-trip engine sources through `Get-Content/Set-Content` — adds UTF-8 BOM (git shows
  line 1 churn; `git show HEAD:<f> | head -c 6 | od` to check). Reverted byte-clean this time.
- `msvc32_build.ps1` rebuilds stale objs by timestamp; `-Tools render` alone suffices after
  sh2_jit.cpp edits. Instrumented rebuild must be reverted+rebuilt before declaring bit-exactness.
- Call-convention notes already proven here transfer to MEG: cdecl helpers push-RTL+`add esp,N`,
  `call_abs` clobbers EAX, no push-imm emitter (imm32+push reg), SSE-free ⇒ stack alignment free.

## Resume hint (step 1 decision) — historical, kept for reference
Try `msys2_shell.cmd -mingw32` g++ once and `clang32` once (5 min). Otherwise pivot win32
validation to **MSVC x86**: same source list as `engine/CMakeLists.txt:12-33` + `src/verify.cpp`,
`cl /std:c++20 /O2 /MT /utf-8 /bigobj`, run verify.exe (it prints selftest + golden compare).
Then start step 2 (x64asm dual-mode) regardless of harness choice — assembler work is harness-free.

## Phase 5 result (2026-09-16) — MEG JIT x86-32 port **green; 2 systemic emitter bugs found+fixed**

Previous agent's +379-line port compiled and ran; selftest failed 65535/65535. Two ROOT CAUSES, both 32-bit only:

1. **setcc-to-ESI is AH-silently-wrong** (emit_revram_encode, swp30_jit.cpp:82 area): 32-bit mode without
   REX cannot address SIL/DIL (8-bit rm=4..7 = AH/CH/DH/BH). `setne ESI` emitted `0F 95 C6` = set AH,
   clobbering the ACC mantissa's high byte -> every v with e!=0 wrong + selftest-loop corruption.
   Fix: 32-bit #if branch computes e!=0 branchless (mov e;sub 1;shr 31;xor 1;sub ecx,ne;add RENC,ecx);
   x64 byte path untouched. x64asm.h 32-bit `setcc` now abort()s for r>=4 (footgun guard, :303).
   Selftest stub also gains push/pop ESI (RENC/RDEC are callee-saved under cdecl; stub clobbering ESI
   broke the C++ loops — earlier 65535 was partly this). m1_expand/decode: no change needed.
2. **imul64i missing the b_hi cross term** (x64asm.h:513): hi omitted `-a_lo` when the imm c<0, so
   a>0,c<0 gave huge +hi -> clamp to +0x7fffff. gen never hits it (m1_expand >= 0); SPEC bake bakes raw
   m_const (drums pc=05c c=-896, mmode2, r116) -> first audio divergence at S396958. Fix: `if (v<0)
   load ecx,[esp+4]; sub dhi,ecx` before addrsp. (imul64 pair form already had both cross terms.)

Found via a temporary per-chip JIT-vs-interpreter compare harness in meg_jit_run (snapshot meg_state+ram+
swp fields, run fn, restore, run_program, diff p/m/r/ram/swp + ops dump of writers to the bad reg). NOTE:
**master+slave SWP30 run on separate threads** — harness needed per-chip slots (InterlockedCompareExchange
claim); the old SMU_DBG_MEG swp30.cpp dumps interleave both chips and are unreliable for A/B. Harness now
REMOVED; SMU_DBG_MEG hook reverted via git checkout (swp30.cpp clean).

### Reg/frame map (as built)
EBX=MS, EDI=SWP, EBP=RAM; scratch EAX(:EDX pair)/ECX/ESI; P(lo,hi)+SEED+SC in stack frame (push ebx,esi,
edi,ebp; sub esp,0x18; args [esp+2c/30/34]); K/P limits immediate; call_lfo = cdecl push(lfo,MS)+add esp,8.

### Gates (win32 MSVC x86, post-cleanup full rebuild)
- selftest: 食い違い **0** (encode 134M, decode 64K, m1 64K sweep) — verify stdout `fc /b tests\verify.txt`: **no differences**.
- drums 4.5 s: JIT-on pcm_sha1 == golden 51e409edf4da…; JIT-off == golden; on/off wav **byte-identical**;
  SMU2000_MEG_BAKE=0 and MEG_EARLY=0 renders == interpreter wav too (kill-switch matrix).
- JIT proof: `SMU2000_MEG_JIT32_LOG=1` -> `meg-jit32: x86-32 build() emitted 913 bytes` (and more).
- x64 re-gate (MinGW64 make): 食い違い **0**. x64asm32_test.exe (rebuilt): **pass 16 fail 0**.
- TIMING drums render wall: JIT-on **2.08 s** vs off **5.22 s = 2.51x** (earlier cold pair 4.23/7.72=1.83x).

### Phase 6 must add
Full A/B matrix across all tests/*.mid (piano/chord/effects/dense/port_b/bend) x {JIT on/off} x {BAKE,EARLY
matrix}; soak (>=18 s renders x2, sha-equal); statetest win32 state-exact with MEG JIT on; multi-program
coverage (effects has branchy programs - verify branchy path exercised, not just drums linear); vst2_host_probe
CPU check; consider deleting or fixing SMU_DBG_MEG interleave if kept as debug aid.

## Phase 6 result (2026-09-16) — Win32 MEG JIT **FULL matrix green; zero source fixes; zero source drift**

All gates on fresh from-scratch build32-msvc (obj deleted; msvc32_build.ps1 -Tools verify,render,statetest;
exit 0, 3x dumpbin 14C x86). Warnings = **only the known C4805 set** (4x sh_sci.cpp + 1x sh_adc.cpp) — none new.
Harness per run_tests.py shapes via scratch drivers (Temp\opencode p6_*.py; wav compare = full-byte sha256 +
fingerprint pcm_sha1; pwsh OutputEncoding=UTF8; all runs redirected to logs).

### 1-2. Render matrix — 7 MIDI x 5 configs = **35/35 PASS** (pcm_sha1 AND wav sha256 identical across all 5, == golden json; bootlen 352800 everywhere)
| test | default | MEG_JIT=0 | MEG_BAKE=0 | MEG_EARLY=0 | SH2_JIT=0 | golden |
|---|---|---|---|---|---|---|
| piano 7bbb16f7.. | = | = | = | = | = | PASS |
| chord 076dbcdb.. | = | = | = | = | = | PASS |
| drums 51e409ed.. | = | = | = | = | = | PASS |
| effects d27e5115.. | = | = | = | = | = | PASS |
| dense 5f45eedd.. | = | = | = | = | = | PASS |
| port_b 154e7f3d.. | = | = | = | = | = | PASS |
| bend 857d81eb.. | = | = | = | = | = | PASS |
(= means bit-identical to golden. Bake/EARLY are optimizations: confirmed bit-identical, not just close.)

### 3. statetest — 7/7 PASS (every midi, --warm 2.0 --steps 50): rc=0, 詰めると..戻し: 一致; default vs MEG_JIT=0 **logs byte-identical** per test.

### 4. verify — 3 configs (default / MEG_JIT=0 / ALL switches off): stdout **byte-identical** to tests/verify.txt; 食い違い 0 on every line. x64 oracle re-verified: fresh make build/verify.exe stdout == tests/verify.txt (255 B, unchanged — no regeneration needed).

### 5. Soak + timing (effects = longest track)
- Soak 18 s + 8 s boot (26 s emulated), JIT default x2: rc=0, wav sha256 **identical** a376295267e0da8d… (same hash as Phase-4 soak), wall 3.8 s / 4.0 s = 6.8x realtime.
- Timing, effects 6 s + 8 s boot, best-of-2 per config (win32 MSVC x86):

| config | wall | factor vs full-interp |
|---|---|---|
| default (both JITs) | 1.98 s | **4.74x** |
| MEG_BAKE=0 | 2.09 s | 4.49x (bake worth ~5%) |
| MEG_EARLY=0 | 1.99 s | 4.72x |
| SH2_JIT=0 (MEG only) | 5.43 s | 1.73x |
| MEG_JIT=0 (SH2 only) | 5.95 s | 1.58x |
| all JIT off | 9.39 s | 1.00x |

MEG-JIT contributes **3.0x** on top of SH2-JIT; SH2-JIT **2.7x** on top of MEG-JIT; combined **4.7x**.
Cross-arch: x64 MinGW render of same track ~1 s wall; its wav is **byte-identical** to the win32 JIT wav
(30ff53fef814…). Phase-5 drums pair (2.08/5.22=2.5x) was MEG-only; win32-with-both-JITs is now far ahead.

### 6. Coverage (temporary env-gated counters SMU2000_P6_COVER in both JITs, effects 18 s run; sources RESTORED byte-clean after: sha256 87EA7C4B…/3DD12C33… = pre-instrument, P6COVER grep empty, rebuild re-gated golden)
- SH2: compiles=**17392 emitted=17392** (100%), bus-check rejects 0, init-fail 0, **buf-full 0**, peak 13.69 MB / 16 MB buf.
  Designed pre-check interpreter exits: delay-slot 90,950, irq-flag 16,024, pc-hash 0 — never compile failures.
- MEG: gen builds **18 OK / 0 bad**, spec-bakes **7 OK / 0 bad**, JIT program calls 2,291,296 vs interpreter 1,904
  (0.083%; all in program-change/rebuild windows — rej_delay=0). **No MEG-JIT compile fallback exists anywhere**;
  build() failure (VirtualAlloc only, silent at swp30_jit.cpp:224-225) never triggered. **Verdict: nothing runs
  interpreted except by-design handoffs; multi-program + branchy programs (effects) fully JIT-compiled.**
- Trace A/B: SH2_JIT_TRACE=300000000,200000 on effects, JIT-on vs off: 200,000 instr (cyc 300.000M→300.239M),
  50,565,626 B traces **byte-identical**. JIT32 proofs printed: sh2-jit32 prologue 175 B; meg-jit32 build() 913 B x2 (chips).

### 7. Hygiene
- git status: only preexisting ' M iPlug2'; M = the 3 port sources; untracked = CPU32_LEDGER.md, build32-msvc/,
  tools/msvc32_build.ps1, tools/x64asm32_test.cpp, and a stray root x64asm32_test.obj (Phase-2 leftover artifact).
- Grep 3 sources: no SMU_DBG_MEG, TODO/FIXME, or rogue env-reads (only the intentional kill-switches +
  SMU2000_{MEG,SH2}_JIT32_LOG/DUMP/DUMPSAMPLE + SH2_JIT_TRACE + SH2_LAZYPC/SLOTNATIVE). swp30.cpp clean.
- git diff audit (856+/42-): every deleted line is a comment, a guard, or a same-value alias swap
  (R12/R13→JIT_ROM/JIT_RAM, R8→JIT_VAL, R11/R8→RENC/RDEC — #define'd to the identical reg on x64);
  all new code sits inside `#if SMU_X64ASM_MODE == 32` branches with the x64 sequence preserved in `#else`.
- x64 re-gate: make -j8 build/verify.exe rc=0, verify 食い違い 0, stdout == tests/verify.txt. tools x64asm32_test.exe
  rebuilt (fresh x64asm32.bin 56879 B): **pass 16 fail 0**.

### 8. DEP / stack-alignment invariant
- MEG win32 path confirmed in source AND exercised (2.29M JIT invocations prove it): VirtualAlloc PAGE_READWRITE
  (:1292) → memcpy → VirtualProtect **PAGE_EXECUTE_READ** (:1301) → FlushInstructionCache (:1302). No RWX.
  SH2 keeps one big PAGE_EXECUTE_READWRITE 16 MB buf (:289, +selftest stub :302) — accepted pre-existing choice.
- **SSE-free invariant holds**: x64asm.h contains NO SSE emitter/API at all (grep movaps/movdqa/pxor/… = 0 hits,
  both modes); 32-bit mode emits only 0F{b6,b7,be,bf,af,bd,c8+,94,9c,a4,ac,8x,4ccmov,jcc}. Binary proof: live MEG
  JIT dump (SMU2000_MEG_JIT32_DUMP, 81,942 B real program incl. ALU+branchy path) objdump i386 → **0 SSE mnemonics**
  (the 8 raw `0F 29` pairs live inside imm32 absolute-pointer immediates); fresh x64asm32.bin → 0 REX, 0 SSE.
  ⇒ win32 JIT frames need no 16-byte stack alignment (documented here, per instruction no code comment added).

### Files changed since Phase 5: **none** (instrumentation round-trip hash-verified; final binaries re-gated golden).

### Phase-7 watch-outs
1. CMake win32 plugin must get the same effect as the msvc32 harness: JITs auto-enable via source self-defines
   (SMU_JIT32_PORT_SH2/MEG); ensure no SMU_JIT32_NO_SH2/MEG leaks into presets and /guard:cf stays absent.
2. vst2_host_probe CPU check: expect ~2.0 s per 6 s render / 14C dll; prove both chips JIT live via stderr
   SMU2000_MEG_JIT32_LOG lines under the host (2 printed = master+slave) — note hosts may swallow stderr.
3. SH2 16 MB PAGE_EXECUTE_READWRITE buf in a plugin: fine so far (no AV/CET issue on this box), but MEG-style
   RW→RX would be the hardened upgrade if a host/AV ever objects.
4. The stack-alignment-free property rests on the SSE-free invariant — any future SSE emitter in x64asm.h
   32-bit mode requires an align-16 preamble in BOTH jit prologues first.

## Phase 7 result (2026-09-16) — CMake plugin builds **win32+x64 GREEN; first MSVC-x64 JIT host run clean; zero source fixes**

Zero repo drift: `git status` after the phase == before ( M iPlug2 preexisting + the 3 port sources +
untracked artifacts only). All build/scratch files in `C:\Users\djtub\AppData\Local\Temp\opencode`
(p7_*.log/bat, p7_cpu_probe.cpp). One stray `nul` file a bash-misdirect created in the repo root was
deleted (`[System.IO.File]::Delete('\\?\...')`); it was this session's artifact, not preexisting.

### 1. Plugin builds (VS 2026, GUI OFF, /MT; wipe of stale GUI=ON cache dirs required first)
- **Watch-out found:** `build-cmake/_bld/vs-win32/CMakeCache.txt` carried `SMU2000_ENABLE_GUI:BOOL=ON`
  from a previous (GUI-phase) session — a preset re-configure silently keeps it. Fix: deleted
  `_bld/vs-win32` + `_bld/vs-x64` (disposable build dirs only), re-configured → `gui=OFF` in both summaries.
  Docs phase: tell users a GUI toggle change needs a fresh cache (`-DSMU2000_ENABLE_GUI=…` or wipe).
- `cmake --preset vs-win32` rc=0 (SDK auto-found at D:/opt/vst/vstsdk2.4 via iplug2_paths.cmake fallback;
  `VST2_SDK_DIR` env also set, harmless). Confirmed pre-build: grep `SMU_JIT32` = **0 hits** in cmake/,
  CMakeLists.txt, CMakePresets.json, engine/, SMU2000_VST2/ — JIT self-defines are source-side only, as designed.
- `cmake --build --preset vs-win32-release *> p7_bld_win32.log` rc=0 → `build-cmake/vst2/Win32/Release/SMU2000_VST2.dll`
  (663,040 B) + `build-cmake/clap/Win32/Release/SMU2000_VST2.clap` (717,824 B), roms staged next to both.
  Warnings = **only the known 5×C4805** (4× sh_sci.cpp + 1× sh_adc.cpp) — none new from the JITs.
- `cmake --build --preset vs-x64-release` rc=0 → vst2/x64/Release .dll 752,128 B + clap/x64/Release .clap 815,104 B.
  **First-ever MSVC x64 compile of the JIT path: clean, zero fixes**, same 5×C4805 only. No native re-gate needed
  (no source touched) but re-ran it anyway: fresh `make -B build/verify.exe` + run → rc=0, stdout
  **byte-identical to tests/verify.txt** (hash-compared), 食い違い 0.
- `dumpbin /headers`: Win32 dll+clap = `14C machine (x86)`, x64 pair = `8664 machine (x64)`. All four correct.
- Task-4 GUI-free check: `dumpbin /dependents` on BOTH win32 dll and clap = KERNEL32, USER32,
  api-ms-win-core-synch-l1-2-0 **only** — no opengl/nanovg/IGraphics DLLs, no VCRUNTIME/MSVCRT (/MT). GUI-ON not built.

### 2. Host-probe CPU matrix — new scratch probe `p7_cpu_probe32/64.exe` (Temp; compiled via p7_probe32/64.bat
  pattern from build_probe32.bat, cl /O2, headers from D:/opt/vst/vstsdk2.4 == the exact headers the dll
  was compiled with, hash-verified vs iPlug2 stub copies). 44100/512, `effOpen`+`effSetSampleRate(opt)`+
  `effSetBlockSize`+**`effMainsChanged(1)`**, XG-on sysex, bank/prog/vol on 8 ch, then boot phase RT-paced
  while churning notes until sound, then **free-run 6 s** (engine runs its own threads; process CPU =
  `GetProcessTimes` = SH2+MEG threads included):

| config (win32 dll unless noted) | wall 6 s | ms/blk | xRT | proc CPU ms | CPU ratio |
|---|---|---|---|---|---|
| default (both JITs) | **3.741 s** | 7.25 | **1.60** | 7,453 | **2.06x** vs full-interp |
| SMU2000_MEG_JIT=0 (SH2 only) | 6.385 s | 12.37 | 0.94 | 12,688 | 1.21x |
| all 4 kill-switches=0 (full interp) | 7.680 s | 14.88 | **0.78 (<RT!)** | 15,328 | 1.00x |
| **x64 dll** default (ref row) | 2.883 s | 5.59 | 2.08 | 5,766 | 2.66x |

  Win32 JIT-in-host = **2.06x** process-CPU win (wall 2.05x, agreeing); full interpreter cannot sustain
  1.0xRT on this 8-ch churn in a host. win32-JIT is only **1.30x slower than x64** — the x86-32 port
  transfers nearly all of the x64 JIT's win into a real 32-bit host path. Boot sound appeared at 875 ms
  (default) vs 3031 ms (interpreter) — JIT speeds firmware boot through the host pull path too.
- Probe gotchas learned (for the docs phase): (a) without `effMainsChanged(1)` iPlug2 stays idle and
  `processReplacing` memsets — 0.001 ms/block of fake "speed"; (b) `effSetSampleRate` passes the rate in
  **`opt`** (float), not `value` (iPlugVST2.cpp:429 `SetSampleRate(opt)`); (c) `engine::fill()` returns
  pure silence until boot completes (engine.cpp:463) and boot runs on the engine's own thread at ~wall
  speed — a probe MUST real-time-pace the boot phase or it measures an empty memset path (first attempt
  read 0.0009 ms/blk, peak=0); (d) mu2000 worker/chip threads hold ~2.0 cores busy-spin in threaded mode,
  so cores-used is ~flat across configs — compare wall-ms and CPU-ms ratios, not "cores free".

### 3. Smoke + JIT proofs under the host
- All 4 configs rc=0, **no crash anywhere** — incl. first MSVC-x64-JIT load of the dll by a host process.
  Audio **non-silent**: rms 0.312–0.314, peak 0.62091 — *identical peak across all four configs* (engine
  behaviour unchanged by JIT/kill-switches, matching Phase-6 bit-exactness).
- stderr proof under host (win32 default): `sh2-jit32: x86-32 prologue emitted (175 bytes…)` +
  `meg-jit32: x86-32 build() emitted 913 bytes` — **same 175/913 B as the native msvc32 harness**.
  NOTE: only **1** `meg-jit32` line is printed, not 2 — by design: `static bool done32` (swp30_jit.cpp:1306-1308)
  logs once per *process* ("環境変数で 1 回だけ"), both chips share it. The Phase-6 stderr note "2 printed =
  master+slave" was wrong for the LOG line; the per-chip ×2 evidence is the coverage counter (Phase 6 §6), not this.
- **CLAP (task 3, nice-to-have):** p7_clap32.exe (win32 compile of tools/clap_host_probe.cpp, reuse of
  build_clap.bat pattern) on the Win32 .clap: `clap_entry v1.2.10`, `plugin[0] id=com.tarboh.SMU2000_VST2`,
  create_plugin+init pass, then `no clap.gui extension` → probe RESULT "FAIL" is **expected GUI-OFF
  behaviour** (probe is GUI-centric; no audio-path in that probe). Audio CPU not measured for CLAP
  (would need a CLAP audio/notes host — out of timebox; plugin code path is shared with VST2 via SMU2000_VST2.cpp).

### Files changed since Phase 6: **none** (build harness + probes all in Temp; repo untouched).

### Phase-8 (docs) watch-outs
1. AGENTS.md/README: document effMainsChanged + rate-in-opt + boot-RT-pace for any future host probe;
   document GUI-cache-staleness (reconfigure after GUI toggle).
2. CI: vs-win32 CI builds now exercise the win32 JITs — the 16 MB PAGE_EXECUTE_READWRITE SH2 buffer
   under AV in CI runners is the residual (untested) risk; MEG RW→RX hardening remains the known upgrade.
3. Full interpreter <1xRT (0.78x) in a host: users on the kill-switches WILL glitch — worth a README line.
4. CLAP host probe covers GUI only; a CLAP audio probe is the one untested host path (shared engine, low risk).

## Phase 8 result + session summary (2026-09-16) — docs synced; **ALL TODOS COMPLETE, SESSION DONE**

Docs-only phase (zero code touched; sources final/green from Phase 7). Synced: `AGENTS.md` Toolchain
notes (old "Win32 = interpreter-only" bullet rewritten to dual-mode truth + broken-mingw32 warning),
`VST2_LEDGER.md` (dated SUPERSEDED notes at Findings §32-bit-JIT / P1 plan / P5 plan + new dated
Status entry pointing here), `CMakePresets.json` mingw-win32 description (toolchain-broken note;
`cmake --list-presets` re-validated rc=0), `cmake/mingw_compat.cmake` header comment (dual-mode JIT
reality + dev-box i686 caveat; comment-only, logic untouched — file contains no win32 JIT/SSE gate,
consistent with arch-clean design). `README.md` untouched (Phase-7 watch-out #3 kill-switch glitch
line remains optional-to-do for the user).

### What changed (code, whole port — `git diff --stat`)
```
 src/compat/x64asm.h          | 358 ++++++++++++++++++++++++-  (dual-mode 32-bit encoder + pair emitters)
 src/mame/cpu/sh2_jit.cpp     | 146 +++++++++++--            (x86-32 codegen path + guard fix)
 src/mame/sound/swp30_jit.cpp | 394 +++++++++++++++++++++++++++--- (x86-32 MEG path + guard fix)
 3 files changed, 856 insertions(+), 42 deletions(-)   (x64 output byte-for-byte preserved)
```
Plus new untracked tooling: `tools/msvc32_build.ps1`, `tools/x64asm32_test.cpp`.

### Headline numbers (all bit-exact; x64/x86/interp streams identical)
- JIT CPU win: **2.06x in win32 VST host** (Phase 7 probe), **4.74x native** (Phase 6 effects render).
- Host throughput: win32-JIT **1.60xRT** vs full-interpreter **0.78xRT** (<RT = glitches on kill-switches).
- win32-JIT only **1.30x behind x64** — port transfers nearly the whole x64 JIT win to 32-bit.
- MSVC x64 got the JIT for the first time (guard `_M_X64`), clean first compile, host-probed OK.

### Tooling added
- `tools/msvc32_build.ps1` — Win32 native harness (MSVC `vcvarsall amd64_x86` + `cl`): verify/render/
  statetest, golden-checked; the supported win32 build path.
- `tools/x64asm32_test.cpp` — x86-32 encoding gate (emits/executes from VirtualAlloc; pass 16 fail 0;
  objdump cross-check 0 REX / 0 SSE).

### Residual risks (from Phase 7 — unmitigated, documented)
1. **GUI cache staleness:** a stale `_bld/vs-win32` (or vs-x64) CMakeCache silently keeps
   `SMU2000_ENABLE_GUI=ON` across preset re-configures — wipe the `_bld/<preset>` dir whenever GUI
   state creeps in unexpectedly.
2. **SH2 16 MB PAGE_EXECUTE_READWRITE buffer** in a plugin DLL: fine on this box, but AV products in
   the wild (or CI runners) may flag RWX; MEG-style W→RX flip is the known hardening upgrade.
3. **CLAP audio path unprobed** under win32 JIT (clap_host_probe is GUI-centric; engine path shared
   with VST2, low risk).

### STANDING RULE (never break)
**No MSYS2 modification is ever attempted.** `C:\msys64` is read-only territory: no pacman, no file
moves, no repairs. The mingw-w64-i686 (mingw32) toolchain **stays broken** (cc1plus silent no-op) —
Win32 builds go through `vs-win32` / `tools/msvc32_build.ps1`, full stop. Nothing committed this
session; `iPlug2` submodule dirty-mark and `roms/` are preexisting/untouched.
