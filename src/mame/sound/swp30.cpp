// license:BSD-3-Clause
// copyright-holders:Olivier Galibert

// Yamaha SWP30/30B, ROMpler/DSP combo

// S-MU2000: MAME 本体の取り込みをやめ、swp30.h（互換層を取り込む）だけにした
#include "swp30.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <sstream>
#include <cmath>

/*
TODOs:
  - 7 bits are still not understood in the MEG instructions
  - in the lfo, the top of slot 9 is not understood
  - slot b is not understood but used (and even read at times)
  - some instruments from the demo don't work well (not sure which)
  - there seems to be some saturation at times in the demo
  - lots of control registers are not understood, in particular
    the 5x ones, one of which is used in the talk mod effect
  - the timing of communication between the meg registers and the
    the environment is known but not implemented


  The SWP30 is the combination of a rompler called AWM2 (Advanced
  Wave Memory 2) and an effects DSP called MEG (Multiple Effects
  Generator).  It also includes some routing/mixing capabilities,
  moving data between AWM2, MEG and serial inputs (MELI) and
  outputs (MELO) with volume management capabilities everywhere.
  Its clock is 33.9MHz and the output is at 44100Hz stereo (768
  cycles per sample pair) per dac output.

  I/O wise, the chip has 8 generic audio serial inputs and 8
  outputs for external plugins, and two dac outputs, all
  stereo. The MU100 connects a stereo ADC to the first input, and
  routes the third input and output to the plugin board.


    Registers:

  The chip interface presents 4096 16-bits registers in a 64x64 grid.
  They are mostly read/write.  Some of this grid is for per-channel
  values for AWM2, but parts are isolated and renumbered for MEG
  registers or for general control functions.


    AWM2:

  The AWM2 is in charge of handling the individual channels.  It
  manages reading the rom, decoding the samples, applying volume and
  pitch envelopes and lfos and filtering the result.  Each channel is
  then sent as a mono signal to the mixer for further processing.

  It is composed of a number of blocks, please refer to the individual
  documentations further in the file:
  - streaming
  - dual special filters
  - dual iir1 filters
  - envelope control
  - lfo

  The sound data can be four formats (8 bits, 12 bits, 16 bits, and
  a 8-bits kinda-apdcm format).  The rom bus is 25 bits address and
  32 bits data wide.  It applies four filters to the sample data in
  two dual filter blocks.  The first block has two filters
  configurable between iir1 and chamberlain, lpf, hpf, band or
  notch, with our without configurable resonance.  The second block
  is two free iir1 filters.  Envelopes are handled automatically,
  and the final result is sent to the mixer for panning, volume
  control and routing.  In addition lfo acts on the pitch and the
  volume.


    MEG:

  The MEG is a DSP with 384 program steps connected to a reverb
  samples ram.  It computes all the effects and sends to result to
  the adcs and the serial outputs.


    Mixer:

  The mixer gets the outputs of the AWM2, the MEG (for the previous
  sample) and the external inputs, attenuates and sums them
  according to its mapping instructions, and pushes the results to
  the MEG and the external outputs.
*/


/*--------------------------------------------------------------------------------

   Memory map in rough numerical order with block indications

  cccccc 000000  AMW2/Filters     mmmm .aaa aaaa aaaa                      Filter 1 mode and main parameter
  cccccc 000001  AMW2/Filters     .rrr rrrr uuuu uuuu                      Filter 1 glide rate / bypass level
  cccccc 000010  AMW2/Filters     .... .ccc cccc cccc                      Filter 2 mode and main parameter
  cccccc 000011  AMW2/Filters     .... .... vvvv vvvv                      Post-filter level
  cccccc 000100  AMW2/Filters     bbbb b... .... ....                      Filters second parameter

  cccccc 000101  AWM2/LFO         .... .... .aaa aaaa                      LFO amplitude depth
  cccccc 000110  AWM2/Envelope    ssss ssss iiii iiii                      Attack speed and start volume
  cccccc 000111  AWM2/Envelope    ssss ssss tttt tttt                      Decay 1 speed and target
  cccccc 001000  AWM2/Envelope    ssss ssss tttt tttt                      Decay 2 speed and target
  cccccc 001001  AWM2/Envelope    ssss ssss gggg gggg                      Release speed & global volume
  cccccc 001010  AWM2/LFO         tt.s ssss mppp pppp                      LFO type, step, pitch mode, pitch depth
  cccccc 001011   ?
  cccccc 001100   ?
  cccccc 001101   ?

  000000 001110                   9100 at startup
  000000 001111                   c002 at startup then c003
  000001 001110  AWM2/Control     internal register address
  000001 001111  AWM2/Control     (read) internal register value
  000010 00111*  AWM2/Control     wave direct access address
  000011 00111*  AWM2/Control     wave direct access size
  000100 001110  AWM2/Control     wave direct access trigger (8000 = read sample from rom, 9000 = read sample from ram, 5000 = write sample to ram)
  000100 001111  AWM2/Control     wave direct access status
  000101 00111*  AWM2/Control     wave direct access data
  00011* 00111*  AWM2/Control     keyon mask
  001000 001110  AWM2/Control     keyon trigger
  001101 001110                   1100 at startup then 0040
  010000 001110  MEG/Control      .... .... .... ....    commit LFO increments on write
  010000 001111  MEG/Control      .... ...a aaaa aaaa    program address
  010001 00111*  MEG/Control      dddd dddd dddd dddd    program data 1/2
  010010 00111*  MEG/Control      dddd dddd dddd dddd    program data 2/2
  010011 001110   ? sy26
  010011 001111   ? sy27
  010100 001111                   00ff after part 1
  010101 001110                   00ff after part 1
  010101 001111                   00ff after part 1
  011mmm 001110  MEG/Reverb       memory map
  100000 001110  MEG/Reverb       ram memory map tlb enable (0=on, 1=off, bits 0-7)
  100000 001111  MEG/Reverb       ram memory bank clear (1=trigger a clear)
  100001 001110  MEG/Reverb       ram direct access status
  100101 00111*  MEG/Reverb       ram direct access address
  100110 00111*  MEG/Reverb       ram direct access data

  101*** 00111*   ? sy5x

  cccccc 010000   ?

  cccccc 010001  AMW2/Streaming   ?-pp pppp pppp pppp                      Pitch
  cccccc 01001*  AMW2/Streaming   ?Lll llll ssss ssss ssss ssss ssss ssss  Loop disable. Loop size adjust. Number of samples before the loop point
  cccccc 01010*  AMW2/Streaming   bfff ffff ssss ssss ssss ssss ssss ssss  Backwards. Finetune. Number of samples in the loop
  cccccc 01011*  AMW2/Streaming   ffSS Smma aaaa aaaa aaaa aaaa aaaa aaaa  Format, Scaling, Compressor mode, Sample address

  cccccc 100000  AWM2/IIR         vvvv vvvv vvvv vvvv                      IIR1 a1
  aaaaaa 100001  MEG/Data         cccc cccc cccc cccc                      constant index 6*a + 0
  cccccc 100010  AWM2/IIR         vvvv vvvv vvvv vvvv                      IIR1 b1
  aaaaaa 100011  MEG/Data         cccc cccc cccc cccc                      constant index 6*a + 1
  cccccc 100100  AWM2/IIR         vvvv vvvv vvvv vvvv                      IIR1 a0
  aaaaaa 100101  MEG/Data         cccc cccc cccc cccc                      constant index 6*a + 2
  cccccc 100110  AWM2/IIR         vvvv vvvv vvvv vvvv                      IIR2 b1
  aaaaaa 100111  MEG/Data         cccc cccc cccc cccc                      constant index 6*a + 3
  cccccc 101000  AWM2/IIR         vvvv vvvv vvvv vvvv                      IIR2 a1
  aaaaaa 101001  MEG/Data         cccc cccc cccc cccc                      constant index 6*a + 4
  cccccc 101010  AWM2/IIR         vvvv vvvv vvvv vvvv                      IIR2 a0
  aaaaaa 101011  MEG/Data         cccc cccc cccc cccc                      constant index 6*a + 5


  aaaaaa 11000a  MEG/Data         oooo oooo oooo oooo                      offset index a
  ssssss 110010  Mixer            llll llll rrrr rrrr                      Route attenuation left/right input s
  ssssss 110011  Mixer            0000 0000 1111 1111                      Route attenuation slot 0/1   input s
  ssssss 110100  Mixer            2222 2222 3333 3333                      Route attenuation slot 2/3   input s
  ssssss 110101  Mixer            fedc ba98 7654 3210                      Route mode bit 2 input s output 0-f
  ssssss 110110  Mixer            fedc ba98 7654 3210                      Route mode bit 1 input s output 0-f
  ssssss 110111  Mixer            fedc ba98 7654 3210                      Route mode bit 0 input s output 0-f
  ssssss 111000  Mixer            llll llll rrrr rrrr                      Route attenuation left/right input s+40
  ssssss 111001  Mixer            0000 0000 1111 1111                      Route attenuation slot 0/1   input s+40
  ssssss 111010  Mixer            2222 2222 3333 3333                      Route attenuation slot 2/3   input s+40
  ssssss 111011  Mixer            fedc ba98 7654 3210                      Route mode bit 2 input s+40 output 0-f
  ssssss 111100  Mixer            fedc ba98 7654 3210                      Route mode bit 1 input s+40 output 0-f
  ssssss 111101  Mixer            fedc ba98 7654 3210                      Route mode bit 0 input s+40 output 0-f
  aaaaaa 11111a  MEG/LFO          pppp ttss iiii iiii                      LFO index a, phase, type, shift, increment
*/

/*======================= AWM2 blocks ============================================

Streaming block

  cccccc 010001   ?-pp pppp pppp pppp                      Pitch
  cccccc 01001*   ?Lll llll ssss ssss ssss ssss ssss ssss  Loop disable. Loop size adjust. Number of samples before the loop point
  cccccc 01010*   bfff ffff ssss ssss ssss ssss ssss ssss  Backwards. Finetune. Number of samples in the loop
  cccccc 01011*   ffSS Smma aaaa aaaa aaaa aaaa aaaa aaaa  Format, Scaling, Compressor mode, Sample address

The streaming block manages reading and decoding samples from rom
and/or dram at a given pitch and format.  The samples are
interpolated for a better quality.

Addresses 000000-ffffff are in rom (and maybe sram?),
1000000-1ffffff are in dram.

The unknown bit in the 010001 slot tends to be set when reading a
compressed sample and unset otherwise.  It does not seem to impact
the result though.  The unknown bit in the 010010 slot doesn't seem
to ever been set in the mu100 and does not seem to impact the
result.

  Sample formats

Samples can be in one of four formats, 8 bits, 12
bits, 16 bits and adaptive-delta-compression with 8 bits per
sample.  Non-compressed samples are zero-extended on the right to
get a almost-full-range 16-bits value.

The compressed format uses a running delta and an accumulator.  The
input byte is expanded into a 10-bit signed value through a fixed
table, which is added to the current delta.  The delta is then
added to the accumulator, which gives the current sample value.
Then the current delta is, depending on the mode bits, multiplied
by either 0.875 (7/8), 0.75 (3/4), 0.5 (1/2) or 0 (e.g. cleared).

The multiplier on the delta is buggy and bias towards negative
numbers, but it's not entirely clear how exactly.  Even worse, the
multiplier results change depending on whether the scaling is zero
or non-zero, and also has some kind of context or extra state bits
hidden somewhere.


  Sample scaling

Samples just read are then shifted left by Scaling bits (0-7).
While the scaling is in practice only used for compressed samples,
the hardware applies it to any format.  The result is clamped
between -0x8000 and a value depending on the amount of scaling
(0x7fff for 0, 0x7ffe for 1, ..., 0x7f80 for 7).


  Sample addressing, pitching and looping

The chip has two 25-bits address, 16-bits data buses to the sample
roms and drams.  The samples are interleaved between the two buses,
looking as if the data bus was 32 bits wide.  It directly manages
dram signals, so there must be a way somewhere for it to tell
whether a sample address is in dram or not.  When in dual-chip
configuration, the address and data lines of the buses are directly
connected, so they have a way to arbitrate their accesses.  The
amount of data needed at a given time varying depending on pitch
and sample format, the design of the memory access controller must
have been interesting.

The current sample position is in signed 25.15 format.  The initial
value of the sample position is minus the number of samples before
the loop point (unsigned 24 bits, slots 010010 and 010011).  It is
incremented by the unsigned 7.15 step value for each sample, until
it reaches a positive value more or equal to the loop size
(unsigned 24 bits, slots 010100 and 010101).  Then if looping is
enabled the position is decreased by the loop size and incremented
by the loop size adjust, otherwise the last sample value output is
held and a maximum speed envelope release is triggered.

The loop size adjust is a 0.6 unsigned value (e.g. between 0 and
0.984375).

Looping is enabled when L=0 (slot 010010) and b=0 (slot 010110).

The unsigned 7.15 step is computed from the pitch and the finetune
values.  The base step value is 2**pitch with pitch encoded as a
signed 4.10 value, e.g. giving a result between 1/256 and almost
but not quite 256.  The exponentiation table has 13 bits of
precision including the left 1 bit.  The finetune is a signed value
between -64 and +63 that is added to the pitch once position 0 is
reached.

Backwards sample reading negates the sample position value before
fetching.  Note that backwards reading disable looping.

Samples are read with sample pos 0 corresponding to the bottom
sample at the 25-bits address. It is important to note that four
consecutive sample values are required for the interpolation block,
and the hardware manages to provide the correct values even for
compressed samples with large steps, requiring to compute and
accumulate the deltas for all the bytes on the way.  The memory
controller must be REALLY interesting.

A pitch skip bigger than the loop size ends up with results
somewhere between weird and utterly insane.  Don't do that.


  Sample interpolation

Samples go through an interpolator which uses two past samples and
two future samples to compute the final value for a non-integer
position, with a weight for each history sample.  The weights are
computed from two polynoms:
  f0(t) = (t - t**3)/6
  f1(t) = t + (t**2 - t**3)/2

The polynoms are used with the decimal part 'p' (as in phase) of
the sample position.  The computation from the four samples s0..s3
is:
  s = - s0 * f0(1-p) + s1 * f1(1-p) + s2 * f1(p) - s3 * f0(p)

f0(0) = f0(1) = f1(0) = 0 and f1(1) = 1, so when phase is 0 (sample
streaming with no frequency shifting) the sample s1 is output.

The implementation of the weights uses two tables with apparently
2048 entries and 10 bits precision (e.g. between 0 and 0.999), but
are in reality 1023 entries.  Each entry of the 1023-entries tables
goes to slots 2n-1 and 2n (n=1..1023), and slots 0 and 2047 are
hardcoded to both 0 for f0 and 0/1.0 for f1. That way the tables
can be used in both directions and the computation of 1-p consists
of inverting all the bits.

The two-past sample for the first position, the first pointed at by
the streamer, is forced to zero.

Post-interpolation, the output is a 16-bits signed value with no
decimals.
*/

// DPCM delta expansion table
const std::array<s16, 256> swp30_device::streaming_block::dpcm_expand = []() {
	std::array<s16, 256> deltas;
	constexpr s16 offset[4] = { 0, 0x20, 0x60, 0xe0 };
	for(u32 i=0; i != 128; i++) {
		u32 e = i >> 5;
		s16 base = ((i & 0x1f) << e) + offset[e];
		deltas[i] = base;
		deltas[i+128] = -base;
	}
	deltas[0x80] = 0x88; // Not actually used by samples, but tested on hardware
	return deltas;
}();

// Pitch conversion table, 2**(i/1024) as 1.12
const std::array<u16, 0x400> swp30_device::streaming_block::pitch_base = []() {
	std::array<u16, 0x400> base;
	for(u32 i=0; i != 0x400; i++)
		base[i] = pow(2, i/1024.0) * 4096;
	return base;
}();

// Sample interpolation functions f0 and f1.  The second half of f1 is adjusted so that the combination is 1.0 (e.g. 0x400)
const std::array<std::array<s16, 0x800>, 2> swp30_device::streaming_block::interpolation_table = []() {
	std::array<std::array<s16, 0x800>, 2> result;

	// The exact way of doing the computations replicate the values
	// actually used by the chip (which are very probably a rom, of
	// course).

	for(u32 i=1; i != 1024; i++) {
		s16 f0 = (((i << 20) - i*i*i) / 6) >> 20;
		result[0][2*i-1] = f0;
		result[0][2*i  ] = f0;
	}
	for(u32 i=1; i != 513; i++) {
		s16 f1 = i + ((((i*i) << 10) - i*i*i) >> 21);
		result[1][2*i-1] = f1;
		result[1][2*i  ] = f1;
	}
	for(u32 i=513; i != 1024; i++) {
		u32 i1 = 2*i;
		u32 i2 = 2047 ^ i1;
		// When interpolating, f1 is added and f0 is subtracted, and the total must be 0x400
		s16 f1 = 0x400 + result[0][i1] + result[0][i2] - result[1][i2];
		result[1][2*i-1] = f1;
		result[1][2*i  ] = f1;
	}
	result[0][    0] = 0x000;
	result[0][0x7ff] = 0x000;
	result[1][    0] = 0x000;
	result[1][0x7ff] = 0x400;
	return result;
}();

const std::array<s32, 8> swp30_device::streaming_block::max_value = {
	0x7fff, 0x7ffe, 0x7ffc, 0x7ff8, 0x7ff0, 0x7fe0, 0x7fc0, 0x7f80
};

void swp30_device::streaming_block::clear()
{
	m_start = 0;
	m_loop = 0;
	m_address = 0;
	m_pitch = 0;
	m_loop_size = 0x400;
	m_pos = 0;
	m_pos_dec = 0;
	m_dpcm_s0 = m_dpcm_s1 = m_dpcm_s2 = m_dpcm_s3 = 0;
	m_dpcm_pos = 0;
	m_dpcm_delta = 0;
	m_first = false;
	m_done = false;
	m_last = 0;
}

void swp30_device::streaming_block::keyon()
{
	m_pos = -(m_start & 0xffffff) - 1;
	m_pos_dec = 0;
	m_dpcm_s0 = m_dpcm_s1 = m_dpcm_s2 = m_dpcm_s3 = 0;
	m_dpcm_pos = m_pos+1;
	m_dpcm_delta = 0;
	m_first = true;
	m_finetune_active = false;
	m_done = false;
}

void swp30_device::streaming_block::scale_and_clamp_one(s16 &val, u32 scale, s32 limit)
{
	s32 sval = val << scale;
	if(sval < -0x8000)
		sval = -0x8000;
	else if(sval > limit)
		sval = limit;
	val = sval;
}

void swp30_device::streaming_block::scale_and_clamp(s16 &val0, s16 &val1, s16 &val2, s16 &val3)
{
	u32 scale = (m_address >> 27) & 7;
	if(!scale)
		return;
	s32 limit = max_value[scale];
	scale_and_clamp_one(val0, scale, limit);
	scale_and_clamp_one(val1, scale, limit);
	scale_and_clamp_one(val2, scale, limit);
	scale_and_clamp_one(val3, scale, limit);
}

void swp30_device::streaming_block::read_16(memory_access<25, 2, -2, ENDIANNESS_LITTLE>::cache &wave, s16 &val0, s16 &val1, s16 &val2, s16 &val3)
{
	// S-MU2000: 逆向きのときは 1 つ手前から読み、step() で並びを裏返す（doc/upstream.md の 35）
	s32 spos = m_loop & 0x80000000 ? -m_pos - 1 : m_pos;
	offs_t base_address = m_address & 0x1ffffff;
	offs_t adr = base_address + (spos >> 1);
	switch(spos & 1) {
	case 0: {
		// 32103210 32103210 32103210
		// bbbbaaaa ddddcccc ........
		u32 l0 = wave.read_dword(adr);
		// S-MU2000: MAME は l1 も adr から読んでいて、後ろの 2 つが前の 2 つの写しになっていた
		// （doc/upstream.md の 10）
		u32 l1 = wave.read_dword(adr+1);
		val0 = l0;
		val1 = l0 >> 16;
		val2 = l1;
		val3 = l1 >> 16;
		break;
	}
	case 1: {
		// 32103210 32103210 32103210
		// aaaa.... ccccbbbb ....dddd
		u32 l0 = wave.read_dword(adr);
		u32 l1 = wave.read_dword(adr+1);
		u32 l2 = wave.read_dword(adr+2);
		val0 = l0 >> 16;
		val1 = l1;
		val2 = l1 >> 16;
		val3 = l2;
		break;
	}
	}
	scale_and_clamp(val0, val1, val2, val3);
}

void swp30_device::streaming_block::read_12(memory_access<25, 2, -2, ENDIANNESS_LITTLE>::cache &wave, s16 &val0, s16 &val1, s16 &val2, s16 &val3)
{
	// S-MU2000: 逆向きのときは 1 つ手前から読み、step() で並びを裏返す（doc/upstream.md の 35）
	s32 spos = m_loop & 0x80000000 ? -m_pos - 1 : m_pos;
	offs_t base_address = m_address & 0x1ffffff;
	offs_t adr = base_address + (spos >> 3)*3;
	switch(spos & 7) {
	case 0: {
		// 10210210 02102102 21021021 10210210 10210210
		// ccbbbaaa ....dddc ........ ........ ........
		u32 l0 = wave.read_dword(adr);
		u32 l1 = wave.read_dword(adr+1);
		val0 =  (l0 & 0x00000fff) << 4;
		val1 =  (l0 & 0x00fff000) >> 8;
		val2 = ((l0 & 0xff000000) >> 20) | ((l1 & 0x0000000f) << 12);
		val3 =   l1 & 0x0000fff0;
		break;
	}
	case 1: {
		// 10210210 02102102 21021021 10210210 10210210
		// bbaaa... .dddcccb ........ ........ ........
		u32 l0 = wave.read_dword(adr);
		u32 l1 = wave.read_dword(adr+1);
		val0 =  (l0 & 0x00fff000) >> 8;
		val1 = ((l0 & 0xff000000) >> 20) | ((l1 & 0x0000000f) << 12);
		val2 =   l1 & 0x0000fff0;
		val3 =  (l1 & 0x0fff0000) >> 12;
		break;
	}
	case 2: {
		// 10210210 02102102 21021021 10210210 10210210
		// aa...... dcccbbba ......dd ........ ........
		u32 l0 = wave.read_dword(adr);
		u32 l1 = wave.read_dword(adr+1);
		u32 l2 = wave.read_dword(adr+2);
		val0 = ((l0 & 0xff000000) >> 20) | ((l1 & 0x0000000f) << 12);
		val1 =   l1 & 0x0000fff0;
		val2 =  (l1 & 0x0fff0000) >> 12;
		val3 = ((l1 & 0xf0000000) >> 24) | ((l2 & 0x000000ff) << 8);
		break;
	}
	case 3: {
		// 10210210 02102102 21021021 10210210 10210210
		// ........ cbbbaaa. ...dddcc ........ ........
		u32 l1 = wave.read_dword(adr+1);
		u32 l2 = wave.read_dword(adr+2);
		val0 =   l1 & 0x0000fff0;
		val1 =  (l1 & 0x0fff0000) >> 12;
		val2 = ((l1 & 0xf0000000) >> 24) | ((l2 & 0x000000ff) << 8);
		val3 =  (l2 & 0x000fff00) >> 4;
		break;
	}
	case 4: {
		// 10210210 02102102 21021021 10210210 10210210
		// ........ baaa.... dddcccbb ........ ........
		u32 l1 = wave.read_dword(adr+1);
		u32 l2 = wave.read_dword(adr+2);
		val0 =  (l1 & 0x0fff0000) >> 12;
		val1 = ((l1 & 0xf0000000) >> 24) | ((l2 & 0x000000ff) << 8);
		val2 =  (l2 & 0x000fff00) >> 4;
		val3 =  (l2 & 0xfff00000) >> 16;
		break;
	}
	case 5: {
		// 10210210 02102102 21021021 10210210 10210210
		// ........ a....... cccbbbaa .....ddd ........
		u32 l1 = wave.read_dword(adr+1);
		u32 l2 = wave.read_dword(adr+2);
		u32 l3 = wave.read_dword(adr+3);
		val0 = ((l1 & 0xf0000000) >> 24) | ((l2 & 0x000000ff) << 8);
		val1 =  (l2 & 0x000fff00) >> 4;
		val2 =  (l2 & 0xfff00000) >> 16;
		val3 =  (l3 & 0x00000fff) << 4;
		break;
	}
	case 6: {
		// 10210210 02102102 21021021 10210210 10210210
		// ........ ........ bbbaaa.. ..dddccc ........
		u32 l2 = wave.read_dword(adr+2);
		u32 l3 = wave.read_dword(adr+3);
		val0 =  (l2 & 0x000fff00) >> 4;
		val1 =  (l2 & 0xfff00000) >> 16;
		val2 =  (l3 & 0x00000fff) << 4;
		val3 =  (l3 & 0x00fff000) >> 8;
		break;
	}
	case 7: {
		// 10210210 02102102 21021021 10210210 10210210
		// ........ ........ aaa..... ddcccbbb .......d
		u32 l2 = wave.read_dword(adr+2);
		u32 l3 = wave.read_dword(adr+3);
		u32 l4 = wave.read_dword(adr+4);
		val0 =  (l2 & 0xfff00000) >> 16;
		val1 =  (l3 & 0x00000fff) << 4;
		val2 =  (l3 & 0x00fff000) >> 8;
		val3 = ((l3 & 0xff000000) >> 20) | ((l4 & 0x0000000f) << 12);
		break;
	}
	}
	scale_and_clamp(val0, val1, val2, val3);
}

void swp30_device::streaming_block::read_8(memory_access<25, 2, -2, ENDIANNESS_LITTLE>::cache &wave, s16 &val0, s16 &val1, s16 &val2, s16 &val3)
{
	// S-MU2000: 逆向きのときは 1 つ手前から読み、step() で並びを裏返す（doc/upstream.md の 35）
	s32 spos = m_loop & 0x80000000 ? -m_pos - 1 : m_pos;
	offs_t base_address = m_address & 0x1ffffff;
	offs_t adr = base_address + (spos >> 2);
	switch(spos & 3) {
	case 0: {
		// 10101010 10101010
		// ddccbbaa ........
		u32 l0 = wave.read_dword(adr);
		val0 = (l0 & 0x000000ff) << 8;
		val1 =  l0 & 0x0000ff00;
		val2 = (l0 & 0x00ff0000) >> 8;
		val3 = (l0 & 0xff000000) >> 16;
		break;
	}
	case 1: {
		// 10101010 10101010
		// ccbbaa.. ......dd
		u32 l0 = wave.read_dword(adr);
		u32 l1 = wave.read_dword(adr+1);
		val0 =  l0 & 0x0000ff00;
		val1 = (l0 & 0x00ff0000) >> 8;
		val2 = (l0 & 0xff000000) >> 16;
		val3 = (l1 & 0x000000ff) << 8;
		break;
	}
	case 2: {
		// 10101010 10101010
		// bbaa.... ....ddcc
		u32 l0 = wave.read_dword(adr);
		u32 l1 = wave.read_dword(adr+1);
		val0 = (l0 & 0x00ff0000) >> 8;
		val1 = (l0 & 0xff000000) >> 16;
		val2 = (l1 & 0x000000ff) << 8;
		val3 =  l1 & 0x0000ff00;
		break;
	}
	case 3: {
		// 10101010 10101010
		// aa...... ..ddccbb
		u32 l0 = wave.read_dword(adr);
		u32 l1 = wave.read_dword(adr+1);
		val0 = (l0 & 0xff000000) >> 16;
		val1 = (l1 & 0x000000ff) << 8;
		val2 =  l1 & 0x0000ff00;
		val3 = (l1 & 0x00ff0000) >> 8;
		break;
	}
	}
	scale_and_clamp(val0, val1, val2, val3);
}

// S-MU2000: mode・scale・limit は m_address から決まり、展開の輪の中では変わらない。
// 呼ぶ側（read_8c）が輪の外で 1 回だけ作って渡す。中身の計算は変えていない
void swp30_device::streaming_block::dpcm_step(u8 input, u32 mode, u32 scale, s32 limit)
{
	m_dpcm_s0 = m_dpcm_s1;
	m_dpcm_s1 = m_dpcm_s2;
	m_dpcm_s2 = m_dpcm_s3;

	// S-MU2000: 展開のしかたを実機の出力そのものに合わせた（doc/upstream.md の 19）。
	//
	//   D   = 差分 + 展開表[入力]
	//   積算器 = (積算器 + D) を上限と下限で切り詰めたもの。出力は 積算器 << scale
	//   Deff = 実際に積算器に足せた量（切り詰めたときは D より小さい）
	//   次の差分 = floor((k·Deff + r) / d)、r = 余りを負の側に取ったもの（-d < r <= 0）。k/d = 7/8, 3/4, 1/2
	//   モード 3 は次の差分が 0
	//
	// 積算器は漏れず、差分の減り方の端数は捨てずに次のサンプルへ持ち越す。上限に当たったときは
	// 差分を 0 にするのでも（MAME）そのままにするのでもなく、足せた量から次の差分を作る。
	//
	// 形は galibert さんが SWP00（MU50）と SWP20（MU80）の実機から取り出した出力で決めた
	// （MAME のコミット b75872cb の議論。モード 2 の形は TaleTN さんが先に見つけていた）。
	// SWP00 の出力はモード 2・スケール 4 の 7 万サンプルがすべて一致し、SWP20 は同じサンプルを
	// 32 通りの形式で鳴らしたもののうちスケール 1〜7 の 28 通り、各 8 万サンプルが（上限に張り付くものも含めて）一致した。
	//
	// 前は k(差分 - 1)/d を 8bit の端数で丸め、積算器を 1/128 漏らしていた。倍音は合うが、
	// 基音より下に実機に無い成分が +10〜30dB 出ていた（丸めの食い違いが積算器で積もったもの）。
	//
	// 余り r は、状態の並びを変えないよう m_dpcm_delta の下 8bit に -r として入れる（上は差分）
	const s32 acc = m_dpcm_s3 >> scale;
	const s32 rem = -(m_dpcm_delta & 7);
	s32 delta = (m_dpcm_delta >> 8) + dpcm_expand[input];

	s32 sample = (acc + delta) << scale;
	if(sample < -0x8000)
		sample = -0x8000;
	else if(sample > limit)
		sample = limit;
	m_dpcm_s3 = sample;
	delta = (sample >> scale) - acc;

	s32 y;
	switch(mode) {
	case 0: y = delta * 7 + rem; m_dpcm_delta = (y >> 3) * 256 + ((y & 7) ? 8 - (y & 7) : 0); break;
	case 1: y = delta * 3 + rem; m_dpcm_delta = (y >> 2) * 256 + ((y & 3) ? 4 - (y & 3) : 0); break;
	case 2: y = delta     + rem; m_dpcm_delta = (y >> 1) * 256 + ((y & 1) ? 2 - (y & 1) : 0); break;
	default: m_dpcm_delta = 0; break;
	}
}

void swp30_device::streaming_block::read_8c(memory_access<25, 2, -2, ENDIANNESS_LITTLE>::cache &wave, s16 &val0, s16 &val1, s16 &val2, s16 &val3)
{
	offs_t base_address = m_address & 0x1ffffff;
	if(m_loop & 0x80000000) {
		// S-MU2000: MAME はここで abort() して落ちる（MU100 でも同じ。doc/upstream.md の 7）。
		//
		// 圧縮したサンプルを逆向きに鳴らす声は ROM に無い。ここに来るのは、発音数がいっぱいのときに
		// firmware が鳴っている声を取り上げて、次の音のレジスタを 1 つずつ書いている途中の 1 サンプル。
		// スタンダードキットのタム（逆向きの 16bit サンプル）の start と loop を書いたあと、
		// address をまだ書いていないと、前の音（圧縮）の address と逆向きの印が混ざる。
		// 次の書き込みとキーオンで正しい状態に戻るので、その 1 サンプルは直前の値を保つ
		val0 = m_dpcm_s0;
		val1 = m_dpcm_s1;
		val2 = m_dpcm_s2;
		val3 = m_dpcm_s3;
		return;
	} else {
		const u32 mode  = (m_address >> 25) & 3;
		const u32 scale = (m_address >> 27) & 7;
		const s32 limit = max_value[scale];
		s32 spos =  m_dpcm_pos;
		base_address += spos >> 2;
		u32 cv = wave.read_dword(base_address);
		while(spos != m_pos + 4) {
			u8 input = cv >> ((spos & 3) << 3);
			dpcm_step(input, mode, scale, limit);
			spos++;
			if((spos & 3) == 0) {
				base_address ++;
				cv = wave.read_dword(base_address);
			}
		}
		m_dpcm_pos = spos;
	}

	val0 = m_dpcm_s0;
	val1 = m_dpcm_s1;
	val2 = m_dpcm_s2;
	val3 = m_dpcm_s3;
}

// S-MU2000: 波形を鳴る順に取り出す。位置を 1 つずつ進めて、補間の前の値（4 つのうち先頭）を並べる。
// 圧縮の波形は read_8c が前から順に解くので、位置を飛ばさない
std::vector<s16> swp30_device::decode_wave(u32 start, u32 loop, u32 address, size_t max)
{
	std::vector<s16> out;
	streaming_block b;
	b.clear();
	b.m_start = s32(start);
	b.m_loop = s32(loop);
	b.m_address = address;
	b.keyon();
	const s32 pre = s32(start & 0xffffff), len = s32(loop & 0xffffff);
	out.reserve(std::min<size_t>(size_t(pre) + size_t(len), max));
	for(s32 pos = -pre; pos < len && out.size() < max; pos++) {
		s16 v0 = 0, v1 = 0, v2 = 0, v3 = 0;
		b.m_pos = pos;
		switch(address >> 30) {
		case 0: b.read_16(m_wave_cache, v0, v1, v2, v3); break;
		case 1: b.read_12(m_wave_cache, v0, v1, v2, v3); break;
		case 2: b.read_8 (m_wave_cache, v0, v1, v2, v3); break;
		case 3: b.read_8c(m_wave_cache, v0, v1, v2, v3); break;
		}
		out.push_back(v0);
	}
	return out;
}

std::pair<s16, bool> swp30_device::streaming_block::step(memory_access<25, 2, -2, ENDIANNESS_LITTLE>::cache &wave, s32 pitch_lfo, u16 pitch_offset)
{
	if(m_done)
		return std::make_pair(m_last, false);

	s16 val0, val1, val2, val3;

	switch(m_address >> 30) {
	case 0: read_16(wave, val0, val1, val2, val3); break;
	case 1: read_12(wave, val0, val1, val2, val3); break;
	case 2: read_8 (wave, val0, val1, val2, val3); break;
	case 3: read_8c(wave, val0, val1, val2, val3); break;
	}
	// S-MU2000: 逆向きに鳴らすサンプルは、読んだ 4 つが番地の順（再生の順とは逆）に並ぶ。
	// MAME はそのまま補間していたので、端数が増えるほど 1 つ前の値へ寄っていき、ぎざぎざの雑音が
	// 高い帯に出ていた（Electro Kit の 28 番で 10〜20kHz が実機より +14〜25dB）。1 つ手前から読んで
	// 裏返すと、前・今・次・その次の順になる（doc/upstream.md の 35）
	if((m_loop & 0x80000000) && (m_address >> 30) != 3) {
		std::swap(val0, val3);
		std::swap(val1, val2);
	}
	if(m_first)
		val0 = 0;

	// Not perfectly exact, there are some rounding-like issues from
	// time to time
	s32 index = (m_pos_dec >> 4) & 2047;
	// The 4-tap cubic interpolation can overshoot past full scale near loud
	// peaks; saturate to s16 instead of letting the store wrap (a full-scale
	// sign flip = an audible click on loud notes).
	s32 racc = (
				  - interpolation_table[0][index ^ 2047] * val0
				  + interpolation_table[1][index ^ 2047] * val1
				  + interpolation_table[1][index       ] * val2
				  - interpolation_table[0][index       ] * val3
				  ) >> 10;
	s16 result = std::clamp<s32>(racc, -0x8000, 0x7fff);

	// Bit 14 is set by the firmware on compressed samples but is not part
	// of the pitch.  Left in, it trips the 0x4000 clamp below as soon as
	// finetune becomes active (i.e. once the loop point is crossed), and
	// the note jumps to the maximum pitch for the rest of its life.
	// S-MU2000: ピッチ EG の今の値を、14bit で回り込ませて足す（doc/upstream.md の 13）
	// S-MU2000: ピッチは 14bit の符号付き（1 オクターブ 1024、0x3eef = -0x111）。下の e の計算は
	// 符号付きのまま成り立つ。MAME はループ点を越えたあとの範囲の制限だけ符号なし（0〜0x3fff）で
	// かけていたので、0 に近いピッチ（元の鍵のまま鳴らす音）に LFO を掛けると、下へ振った分が 0 に、
	// 0 のすぐ下の値を上へ振った分が 0x3fff に張り付き、ビブラートが片側だけになっていた（doc/upstream.md の 14）
	s32 sp = s32(util::sext(u32((m_pitch + pitch_offset) & 0x3fff), 14)) + pitch_lfo;
	if(m_finetune_active) {
		s32 ft = (m_loop >> 24) & 0x7f;
		if(ft & 0x40)
			ft -= 0x80;
		sp = std::clamp(sp + ft, -0x2000, 0x1fff);
	}
	const u32 pitch = u32(sp);

	u32 e = ((pitch >> 10) + 8) & 15;
	u32 m = pitch & 0x3ff;
	u32 step = (pitch_base[m] << 10) >> (15-e);

	m_pos_dec += step;
	if(m_pos_dec >= 0x8000) {
		m_first = false;
		m_pos += m_pos_dec >> 15;
		if(!m_finetune_active && m_pos >= 0)
			m_finetune_active = true;

		m_pos_dec &= 0x7fff;
		if(m_pos >= m_loop_size) {
			if(!((m_loop & 0x80000000) || (m_start & 0x40000000))) {
				m_pos -= m_loop_size;
				m_pos_dec += (m_start >> 15) & 0x7e00;
				if(m_pos_dec >= 0x8000)
					m_pos ++;
				m_pos_dec &= 0x7fff;
				// S-MU2000: MAME は 3 に決め打ちしていた（doc/upstream.md の 12）。
				// 展開は m_pos+3 まで済んでいて、ループの後ろには先頭の 3 つの差分の写しがある。
				// 1 回で 2 つ進んでループを跨ぐと、済んでいるのは先頭の 1 つ目までなので、
				// 3 から始めると 2 つ目の差分を落として積算器がずれる。済んだ所の続きから展開する
				m_dpcm_pos -= m_loop_size;
			} else {
				m_done = true;
				m_last = result;
				return std::make_pair(m_last, true);
			}
		}
	}
	return std::make_pair(result, false);
}

void swp30_device::streaming_block::update_loop_size()
{
	// The loop size is 24 bits; bits 24-30 are the finetune, which is read
	// back as (m_loop >> 24) & 0x7f just below in step().  Taking 26 bits
	// here folds the bottom two finetune bits into the size, so any sample
	// with a finetune >= 4 gets a loop size tens of millions of samples
	// long and never loops.
	m_loop_size = m_loop & 0xffffff;
	if(!m_loop_size && !((m_loop & 0x80000000) || (m_start & 0x40000000)))
		m_loop_size = 0x400;
}

void swp30_device::streaming_block::start_h_w(u16 data)
{
	m_start = (m_start & 0x0000ffff) | (data << 16);
	update_loop_size();
}

void swp30_device::streaming_block::start_l_w(u16 data)
{
	m_start = (m_start & 0xffff0000) | data;
}

void swp30_device::streaming_block::loop_h_w(u16 data)
{
	m_loop = (m_loop & 0x0000ffff) | (data << 16);
	update_loop_size();
}

void swp30_device::streaming_block::loop_l_w(u16 data)
{
	m_loop = (m_loop & 0xffff0000) | data;
	update_loop_size();
}

void swp30_device::streaming_block::address_h_w(u16 data)
{
	m_address = (m_address & 0x0000ffff) | (data << 16);
}

void swp30_device::streaming_block::address_l_w(u16 data)
{
	m_address = (m_address & 0xffff0000) | data;
}

void swp30_device::streaming_block::pitch_w(u16 data)
{
	m_pitch = data;
}

u16 swp30_device::streaming_block::start_h_r() const
{
	return m_start >> 16;
}

u16 swp30_device::streaming_block::start_l_r() const
{
	return m_start;
}

u16 swp30_device::streaming_block::loop_h_r() const
{
	return m_loop >> 16;
}

u16 swp30_device::streaming_block::loop_l_r() const
{
	return m_loop;
}

u16 swp30_device::streaming_block::address_h_r() const
{
	return m_address >> 16;
}

u16 swp30_device::streaming_block::address_l_r() const
{
	return m_address;
}

u16 swp30_device::streaming_block::pitch_r() const
{
	return m_pitch;
}

std::string swp30_device::streaming_block::describe() const
{
	std::ostringstream desc;
	util::stream_format(desc, "[%04x %08x %08x %08x] ", m_pitch, m_start, m_loop, m_address);
	util::stream_format(desc, "sample %06x-%06x @ %07x ", m_start & 0xffffff, m_loop & 0xffffff, m_address & 0x1ffffff);
	switch(m_address >> 30) {
	case 0: desc << "16"; break;
	case 1: desc << "12"; break;
	case 2: desc << "8 "; break;
	case 3: util::stream_format(desc, "c%x", (m_address >> 25) & 3); break;
	}
	if(m_address & 0x38000000)
		util::stream_format(desc, " scale %x", (m_address >> 27) & 7);
	if(m_loop & 0x80000000)
		desc << " back";
	else if(m_start & 0x40000000)
		desc << " fwd ";
	else
		desc << " loop";
	if(m_start & 0x3f000000)
		util::stream_format(desc, " loop-adjust %02x", (m_start >> 24) & 0x3f);
	if(m_loop & 0x7f000000) {
		if(m_loop & 0x40000000)
			util::stream_format(desc, " loop-tune -%02x", 0x40 - ((m_loop >> 24) & 0x3f));
		else
			util::stream_format(desc, " loop-tune +%02x", (m_loop >> 24) & 0x3f);
	}
	if(m_pitch & 0x2000) {
		u32 p = 0x4000 - (m_pitch & 0x3fff);
		util::stream_format(desc, " pitch -%x.%03x", p >> 10, p & 0x3ff);
	} else if(m_pitch & 0x3fff)
		util::stream_format(desc, " pitch +%x.%03x", (m_pitch >> 10) & 7, m_pitch & 0x3ff);

	return std::move(desc).str();
}


/*--------------------------------------------------------------------------------

Special filters block

  cccccc 000000   mmmm .aaa aaaa aaaa                      Filter 1 mode and main parameter
  cccccc 000001   .rrr rrrr uuuu uuuu                      Filter 1 glide rate / bypass level
  cccccc 000010   mmmm .aaa aaaa aaaa                      Filter 2 mode and main parameter
  cccccc 000011   .... .... vvvv vvvv                      Post-filter level
  cccccc 000100   bbbb b... .... ....                      Filters second parameter

This block takes samples from the streaming block and applies two
recursive filters to them.  The type of filter and its coefficient
encoding depends on the 4-bits mode.  Filter 1 has encoded
parameters a and b, filter 2 has c and d.  First parameter is 11
bits and the second 5.  A first paramter of 0 disables the
associated filter.



  Volumes

Three attenuations are used, in 4.4 format.

Attenuation u in slot 000001 is the bypass/dry level, adding an
attenuated version of the input directly the output.

S-MU2000: r in slot 000001 is the glide rate of filter 1's main
parameter (a, 11 bits).  A write to slot 000000 does not take effect
at once: a moves towards the written value by (8+r.m) << (r.e+2)
per sample, in units of 1/65536 (r is fp 4.3).  The firmware writes
both every 10ms, with r picked from |new a - old a| so that a arrives
within the tick (table 0x1e58d6 in v2.01).  Checked against
recordings of the hardware (doc/upstream.md, 40).

Attenuation v in slot 000011 is the filter 1 level, attenuating its
output before summing to the block output.

  Filter types
0: Chamberlin configuration low pass filter with fixed q=1

   a is fp 3.8
   k = ((0x101 + a.m) << a.e) / 65536

   B(0) = L(0) = 0
   H' = x0 - L - B
   B' = B + k * H'
   L' = L + k * B'
   y0 = L'

1: Chamberlin configuration low pass filter

   a is fp 3.8, (b+4) is fp 3.3
   k = ((0x101 + a.m) << a.e) / 65536
   q = ((0x10 - (b+4).m) << (4 - (b+4).e)) / 128

   B(0) = L(0) = 0
   H' = x0 - L - q*B
   B' = B + k * H'
   L' = L + k * B'
   y0 = L'

2: order-1 lowpass IIR

   a is fp 3.8, b is unused

   a0 = ((0x101 + a.m) << a.e) / 65536
   b1 = 1-a0
   y0 = x0 * a0 + y1 * b1 = y1 + (x0 - y1) * a0

3: order-2 lowpass IIR

   a is fp3.8, (b+4) is fp 2.3

   dt = ((0x10 - (b+4).m) << (4 - (b+4).e)) / 128

   a0 = ((0x101 + a.m) << a.e) / 65536
   b1 = (2 - dt - a0)
   b2 = dt - 1

   y0 = x0 * a0 + y1 * (2 - dt - a0) + y2 * (dt - 1)
      = (x0 - y1) * a0 + (y2 - y1) * dt + 2*y1 - y2

4: Chamberlin configuration band pass filter with fixed q=1

   a is fp 3.8
   k = ((0x101 + a.m) << a.e) / 65536

   B(0) = L(0) = 0
   B' = B + k * (x0 - L - B)
   L' = L + k * B'
   y0 = B'

5: Chamberlin configuration band pass filter

   a is fp 3.8, (b+4) is fp 3.3
   k = ((0x101 + a.m) << a.e) / 65536
   q = ((0x10 - (b+4).m) << (4 - (b+4).e)) / 128

   B(0) = L(0) = 0
   B' = B + k * (x0 - L - q*B)
   L' = L + k * B'
   y0 = B'

6: order-1 ?pass IIR

   a is fp 3.8, b is unused

   a0 = ((0x101 + a.m) << a.e) / 65536
   b1 = 1-a0
   y0 = x0 - x1 + y1 * b1 = x0 - x1 + y1 - y1 * a0

8: Chamberlin configuration high pass filter with fixed q=1

   a is fp 3.8
   k = ((0x101 + a.m) << a.e) / 65536

   B(0) = L(0) = 0
   H' = x0 - L - B
   B' = B + k * H'
   L' = L + k * B'
   y0 = B'

9: Chamberlin configuration high pass filter

   a is fp 3.8, (b+4) is fp 3.3
   k = ((0x101 + a.m) << a.e) / 65536
   q = ((0x10 - (b+4).m) << (4 - (b+4).e)) / 128

   B(0) = L(0) = 0
   H' = x0 - L - q*B
   B' = B + k * H'
   L' = L + k * B'
   y0 = H'

c: Chamberlin configuration notch filter with fixed q=1

   a is fp 3.8
   k = ((0x101 + a.m) << a.e) / 65536

   B(0) = L(0) = 0
   H' = x0 - L - B
   B' = B + k * H'
   L' = L + k * B'
   y0 = H' + L

d: Chamberlin configuration notch filter

   a is fp 3.8, (b+4) is fp 3.3
   k = ((0x101 + a.m) << a.e) / 65536
   q = ((0x10 - (b+4).m) << (4 - (b+4).e)) / 128

   B(0) = L(0) = 0
   H' = x0 - L - q*B
   B' = B + k * H'
   L' = L + k * B'
   y0 = H' + L
*/


void swp30_device::filter_block::clear()
{
	m_filter_1_a = 0;
	m_level_1 = 0;
	m_filter_2_a = 0;
	m_level_2 = 0;
	m_filter_b = 0;

	m_filter_1_p1 = 0;
	m_filter_2_p1 = 0;
	m_filter_p2 = 0;

	m_filter_1_y0 = 0;
	m_filter_1_y1 = 0;
	m_filter_1_x1 = 0;
	m_filter_1_x2 = 0;

	m_filter_1_h = 0;
	m_filter_1_b = 0;
	m_filter_1_l = 0;
	m_filter_1_n = 0;

	m_filter_2_y0 = 0;
	m_filter_2_y1 = 0;
	m_filter_2_x1 = 0;
	m_filter_2_x2 = 0;

	m_filter_2_h = 0;
	m_filter_2_b = 0;
	m_filter_2_l = 0;
	m_filter_2_n = 0;
}

void swp30_device::filter_block::keyon(u32 &cur, s32 &cur2)
{
	m_filter_1_y0 = 0;
	m_filter_1_y1 = 0;
	m_filter_1_x1 = 0;
	m_filter_1_x2 = 0;

	m_filter_1_h = 0;
	m_filter_1_b = 0;
	m_filter_1_l = 0;
	m_filter_1_n = 0;

	m_filter_2_y0 = 0;
	m_filter_2_y1 = 0;
	m_filter_2_x1 = 0;
	m_filter_2_x2 = 0;

	m_filter_2_h = 0;
	m_filter_2_b = 0;
	m_filter_2_l = 0;
	m_filter_2_n = 0;

	// 鍵を押したら、切る高さは滑らせずに書いてある値から始める
	cur = u32(m_filter_1_a & 0x7ff) << 16;
	m_filter_1_p1 = filter_p1(m_filter_1_a);
	cur2 = filter_2_target(m_filter_2_a);
	m_filter_2_p1 = filter_p1(cur2 < 0 ? 0 : cur2 >> 16);
}

s32 swp30_device::filter_block::step(s16 input, u32 &cur, s32 &cur2)
{
	s32 y0 = 0;
	// S-MU2000: 切る高さ（0x00 の下 11bit）は、書いた値へすぐ飛ばずに滑っていく。速さは 0x01 の上 7bit を
	// 指数 4bit・仮数 3bit とみて、1 サンプルに (8+m) << (e+2) / 65536 単位。firmware は 10ms ごとに
	// 「前の値からの差」に見合う速さを書く（表 0x1E58D6）。実機の録音で確かめた（doc/upstream.md の 40）
	const u32 target = u32(m_filter_1_a & 0x7ff) << 16;
	if(cur != target) {
		const u32 code = (m_level_1 >> 8) & 0x7f;
		const u32 r = (8 + (code & 7)) << ((code >> 3) + 2);
		if(cur < target)
			cur = target - cur <= r ? target : cur + r;
		else
			cur = cur - target <= r ? target : cur - r;
		m_filter_1_p1 = filter_p1(cur >> 16);
	}
	// S-MU2000: フィルタ 2（0x02）も滑る。速さは 0x03 の上 7bit を同じ形で読んだものの 1.5 倍（firmware は
	// いつも 0x5010 を書くので、1 サンプルに 0.75 単位）。切る高さ 0（HPF を切ったときの 0x8000）の行き先は
	// 0 ではなく 0x800 下で、そこから上げると約 60ms 何も起きずに、0 を越えてから効き始める。0 より下にいる
	// あいだは素通し。実機の録音で確かめた（doc/upstream.md の 41）
	const s32 target2 = filter_2_target(m_filter_2_a);
	if(cur2 != target2) {
		const u32 code = (m_level_2 >> 8) & 0x7f;
		const s32 r = ((8 + (code & 7)) << ((code >> 3) + 2)) * 3 / 2;
		if(cur2 < target2)
			cur2 = target2 - cur2 <= r ? target2 : cur2 + r;
		else
			cur2 = cur2 - target2 <= r ? target2 : cur2 - r;
		m_filter_2_p1 = filter_p1(cur2 < 0 ? 0 : cur2 >> 16);
	}
	const u32 filter_2_on = (m_filter_2_a & 0x7800) | (cur2 < 0 ? 0 : cur2 >> 16);
	// S-MU2000: レゾナンス（b）は、その段の種類がレゾナンス付き（bit 12）のときだけ効く。
	// MAME は 1 段目の種類で決めた値を 2 段目にも使っていたので、Q 固定のはずの 2 段目
	// （ギターの 160Hz のハイパスなど）に山ができていた。b を書いた後に種類が変わった
	// ときも古い値のままだった（doc/upstream.md の 6 番）
	const s32 p2_1 = BIT(m_filter_1_a, 12) ? m_filter_p2 : 0x80;
	const s32 p2_2 = BIT(m_filter_2_a, 12) ? m_filter_p2 : 0x80;
	if(m_filter_1_a & 0x7fff) {
		if(!BIT(m_filter_1_a, 13)) {
			m_filter_1_h = (input << 6) - m_filter_1_l - ((s64(p2_1) * m_filter_1_b) >> 7);
			m_filter_1_b = m_filter_1_b + ((s64(m_filter_1_p1) * m_filter_1_h) >> 16);
			m_filter_1_n = m_filter_1_h + m_filter_1_l;
			m_filter_1_l = m_filter_1_l + ((s64(m_filter_1_p1) * m_filter_1_b) >> 16);

			switch(m_filter_1_a >> 14) {
			case 0x0: y0 = m_filter_1_l; break;
			case 0x1: y0 = m_filter_1_b; break;
			case 0x2: y0 = m_filter_1_h; break;
			case 0x3: y0 = m_filter_1_n; break;
			}
		} else {
			switch(m_filter_1_a >> 12) {
			case 0x2: y0 = m_filter_1_y0 + ((s64(m_filter_1_p1) * ((input << 6) - m_filter_1_y0)) >> 16); break;
			case 0x3: y0 = 2*m_filter_1_y0 - m_filter_1_y1 + ((s64(m_filter_1_p1) * ((input << 6) - m_filter_1_y0)) >> 16) + ((s64(p2_1) * (m_filter_1_y1 - m_filter_1_y0)) >> 7); break;
			case 0x6: y0 = ((input - m_filter_1_x1) << 6) + m_filter_1_y0 + ((s64(m_filter_1_p1) * (0 - m_filter_1_y0)) >> 16); break;
			case 0x7: y0 = ((input - m_filter_1_x1) << 6) + 2*m_filter_1_y0 - m_filter_1_y1 + ((s64(m_filter_1_p1) * (0 - m_filter_1_y0)) >> 16) + ((s64(p2_1) * (m_filter_1_y1 - m_filter_1_y0)) >> 7); break;
			case 0xa: y0 = ((input - 2*m_filter_1_x1 + m_filter_1_x2) << 6) + m_filter_1_y0 + ((s64(m_filter_1_p1) * (0 - m_filter_1_y0)) >> 16); break;
			case 0xb: y0 = ((input - 2*m_filter_1_x1 + m_filter_1_x2) << 6) + 2*m_filter_1_y0 - m_filter_1_y1 + ((s64(m_filter_1_p1) * (0 - m_filter_1_y0)) >> 16) + ((s64(p2_1) * (m_filter_1_y1 - m_filter_1_y0)) >> 7); break;
			case 0xe: y0 = ((input - 2*m_filter_1_x1 + m_filter_1_x2) << 6) + m_filter_1_y0 + ((s64(m_filter_1_p1) * ((m_filter_1_x1 << 6) - m_filter_1_y0)) >> 16); break;
			case 0xf: y0 = ((input - 2*m_filter_1_x1 + m_filter_1_x2) << 6) + 2*m_filter_1_y0 - m_filter_1_y1 + ((s64(m_filter_1_p1) * ((m_filter_1_x1 << 6) - m_filter_1_y0)) >> 16) + ((s64(p2_1) * (m_filter_1_y1 - m_filter_1_y0)) >> 7); break;
			}

			m_filter_1_x2 = m_filter_1_x1;
			m_filter_1_x1 = input;
			m_filter_1_y1 = m_filter_1_y0;
			m_filter_1_y0 = y0;
		}

		if(filter_2_on) {
			if(!BIT(m_filter_2_a, 13)) {
				m_filter_2_h = y0 - m_filter_2_l - ((s64(p2_2) * m_filter_2_b) >> 7);
				m_filter_2_b = m_filter_2_b + ((s64(m_filter_2_p1) * m_filter_2_h) >> 16);
				m_filter_2_n = m_filter_2_h + m_filter_2_l;
				m_filter_2_l = m_filter_2_l + ((s64(m_filter_2_p1) * m_filter_2_b) >> 16);

				switch(m_filter_2_a >> 14) {
				case 0x0: y0 = m_filter_2_l; break;
				case 0x1: y0 = m_filter_2_b; break;
				case 0x2: y0 = m_filter_2_h; break;
				case 0x3: y0 = m_filter_2_n; break;
				}
			} else {
				s32 y0_1 = y0;
				switch(m_filter_2_a >> 12) {
				case 0x2: y0 = m_filter_2_y0 + ((s64(m_filter_2_p1) * (y0 - m_filter_2_y0)) >> 16); break;
				case 0x3: y0 = 2*m_filter_2_y0 - m_filter_2_y1 + ((s64(m_filter_2_p1) * (y0 - m_filter_2_y0)) >> 16) + ((s64(p2_2) * (m_filter_2_y1 - m_filter_2_y0)) >> 7); break;
				case 0x6: y0 = (y0 - m_filter_2_x1) + m_filter_2_y0 + ((s64(m_filter_2_p1) * (0 - m_filter_2_y0)) >> 16); break;
				case 0x7: y0 = (y0 - m_filter_2_x1) + 2*m_filter_2_y0 - m_filter_2_y1 + ((s64(m_filter_2_p1) * (0 - m_filter_2_y0)) >> 16) + ((s64(p2_2) * (m_filter_2_y1 - m_filter_2_y0)) >> 7); break;
				case 0xa: y0 = (y0 - 2*m_filter_2_x1 + m_filter_2_x2) + m_filter_2_y0 + ((s64(m_filter_2_p1) * (0 - m_filter_2_y0)) >> 16); break;
				case 0xb: y0 = (y0 - 2*m_filter_2_x1 + m_filter_2_x2) + 2*m_filter_2_y0 - m_filter_2_y1 + ((s64(m_filter_2_p1) * (0 - m_filter_2_y0)) >> 16) + ((s64(p2_2) * (m_filter_2_y1 - m_filter_2_y0)) >> 7); break;
				case 0xe: y0 = (y0 - 2*m_filter_2_x1 + m_filter_2_x2) + m_filter_2_y0 + ((s64(m_filter_2_p1) * ((m_filter_2_x1 << 6) - m_filter_2_y0)) >> 16); break;
				case 0xf: y0 = (y0 - 2*m_filter_2_x1 + m_filter_2_x2) + 2*m_filter_2_y0 - m_filter_2_y1 + ((s64(m_filter_2_p1) * ((m_filter_2_x1 << 6) - m_filter_2_y0)) >> 16) + ((s64(p2_2) * (m_filter_2_y1 - m_filter_2_y0)) >> 7); break;
				}

				m_filter_2_x2 = m_filter_2_x1;
				m_filter_2_x1 = y0_1;
				m_filter_2_y1 = m_filter_2_y0;
				m_filter_2_y0 = y0;
			}
		}
	}

	s32 result = volume_apply(m_level_1, input << 6) + volume_apply(m_level_2, y0);
	if(result < -0x400000)
		result = -0x400000;
	else if(result > 0x3fffff)
		result = 0x3fffff;
	return result;
}

u16 swp30_device::filter_block::filter_1_a_r() const
{
	return m_filter_1_a;
}

u16 swp30_device::filter_block::level_1_r() const
{
	return m_level_1;
}

u16 swp30_device::filter_block::filter_2_a_r() const
{
	return m_filter_2_a;
}

u16 swp30_device::filter_block::level_2_r() const
{
	return m_level_2;
}

u16 swp30_device::filter_block::filter_b_r() const
{
	return m_filter_b;
}

void swp30_device::filter_block::filter_1_a_w(u16 data)
{
	// S-MU2000: 係数は step() が滑っている途中の切る高さから出す（ここでは行き先を覚えるだけ）
	m_filter_1_a = data;
}

void swp30_device::filter_block::level_1_w(u16 data)
{
	m_level_1 = data;
}

void swp30_device::filter_block::filter_2_a_w(u16 data)
{
	m_filter_2_a = data;
	// p1 は step で、滑っている途中の値から出す
}

void swp30_device::filter_block::level_2_w(u16 data)
{
	m_level_2 = data;
}

void swp30_device::filter_block::filter_b_w(u16 data)
{
	m_filter_b = data;
	// S-MU2000: 種類に関係なく b から計算しておき、使うかは step() が段ごとに決める
	u32 p2 = (m_filter_b >> 11) + 4;
	m_filter_p2 = (0x10 - (p2 & 7)) << (4 - (p2 >> 3));
}


s32 swp30_device::filter_block::volume_apply(u8 level, s32 sample)
{
	// Level is 4.4 floating point positive, and represents an attenuation
	// Sample is 16.6 signed and the result is in the same format

	// ff seems to be hardcoded to 0 output
	if(level == 0xff)
		return 0;

	s32 e = level >> 4;
	s32 m = level & 0xf;
	return ((sample << 5) - (sample * m)) >> (e+5);
}


//--------------------------------------------------------------------------------

// IIR1 filters block
//
//   cccccc 100000   IIR1 a1
//   cccccc 100010   IIR1 b1
//   cccccc 100100   IIR1 a0
//   cccccc 100110   IIR2 b1
//   cccccc 101000   IIR2 a1
//   cccccc 101010   IIR2 a0
//
// This block takes samples from the filter block and applies two 3-point
// FIR filters.  The filter constants are encoded in signed 3.13
// format.
//
// Given two consecutive inputs x0, x1 (x1 being the oldest) and the
// previous output y1 a IIR1 filter computes the output y0 as:
//
//   y0 = a0 * x0 + a1 * x1 + b1 * y1
//
// It gets 16.6 and outputs 17.6 saturated values.

void swp30_device::iir1_block::clear()
{
	m_a[0][0] = 0;
	m_a[0][1] = 0;
	m_b[0]    = 0;
	m_a[1][0] = 0;
	m_a[1][1] = 0;
	m_b[1]    = 0;

	m_hx[0] = 0;
	m_hy[0] = 0;
	m_hx[1] = 0;
	m_hy[1] = 0;
}

void swp30_device::iir1_block::keyon()
{
	m_hx[0] = 0;
	m_hy[0] = 0;
	m_hx[1] = 0;
	m_hy[1] = 0;
}

s32 swp30_device::iir1_block::step(s32 input)
{
	s32 ya = std::clamp<s32>((s64(m_a[0][0]) * input + s64(m_a[0][1]) * m_hx[0] + s64(m_b[0]) * m_hy[0]) >> 13, -0x800000, 0x7fffff);
	s32 yb = std::clamp<s32>((s64(m_a[1][0]) * ya    + s64(m_a[1][1]) * m_hx[1] + s64(m_b[1]) * m_hy[1]) >> 13, -0x800000, 0x7fffff);

	m_hx[0] = input;
	m_hy[0] = ya;

	m_hx[1] = ya;
	m_hy[1] = yb;

	return yb;
}

template<u32 Filter> u16 swp30_device::iir1_block::a0_r() const
{
	return m_a[Filter][0];
}

template<u32 Filter> u16 swp30_device::iir1_block::a1_r() const
{
	return m_a[Filter][1];
}

template<u32 Filter> u16 swp30_device::iir1_block::b1_r() const
{
	return m_b[Filter];
}

template<u32 Filter> void swp30_device::iir1_block::a0_w(u16 data)
{
	m_a[Filter][0] = data;
}

template<u32 Filter> void swp30_device::iir1_block::a1_w(u16 data)
{
	m_a[Filter][1] = data;
}

template<u32 Filter> void swp30_device::iir1_block::b1_w(u16 data)
{
	m_b[Filter] = data;
}


//--------------------------------------------------------------------------------

// Envelope block
//
//
//   cccccc 000110   ssss ssss iiii iiii                      Attack speed and start volume
//   cccccc 000111   ssss ssss tttt tttt                      Decay 1 speed and target
//   cccccc 001000   ssss ssss tttt tttt                      Decay 2 speed and target
//   cccccc 001001   ssss ssss gggg gggg                      Release speed & global volume
//
// The envelope block manages the final volume of an awm2 voice and
// allows to automatically run it through four steps:
// - Attack, climbing up from a programmed value to global volume with a slowing down curve
// - Decay 1, going down to a target volume in a linear fashion
// - Decay 2, going up or down to another target in a linear fashion
// - Release, going down to silence in a linear fashion
//
// The global volume, though, is taken into account by adding it to
// the raw envelope volume.  Hence attack actually targets 0, and the
// levels reached by the decays are the given target plus the global
// volume.
//
// The volume itself is a 14-bit 4.10 attenuation which is manipulated
// as a single number by this block.  An idle voice has stage release
// and volume 3fff.  Start volume, targets and global volume are 4.4,
// hence just zero-extended on the right.
//
// Volume modification uses a concept of speed.  At each sample a
// value is added depending on the speed and a cycle:
// - Speed 78+:    value is always 7f
//
// - Speed 70..77: value alternates between 3f and 7f on a 8-samples
//                 cycle with seven 7f on speed 77, six on 76, etc up
//                 to only 3f on 70.
//
// - Speed 48..6f: same cycles with changing pair for every 8 speeds,
//                 going 1f/3f then f/1f all the way down to 1/3.
//
// - Speed 40..47: 16-samples cycles alternating 0 and 1, where on
//                 every two-sample block there is a 1 and then either a
//                 0 or a 1.  Speed 47 has one 0, 46 has two, all the way
//                 down to 40 which is 50% 0.
//
// - Speed 38..3f: cycles of size 2*16, where in each block of 2 cycles there
//                 may be one 1 depending on the cycle. Goes from
//                 fifteen 1 (out of thirty-two) on speed 3f to eight ones on
//                 speed 38.
//
// - Speed 00..37: same cycles with bigger blocks, going size 4 for 30..37 up
//                 to size 32 for 00..07.
//
// Phase on the cycles seems unpredictable.
//
// Decay 1, Decay 2 and Release use directly the speed as programmed
// (with Release inverting bit 7).  Speeds 80+ gives someone weird
// results, with steps still of 7f but sometimes acting on the target
// level.  Attack uses the given speed but adds to it bits 9..13 of
// the volume multiplied by 4, giving a fast curve at the start which
// decelerates when approaching maximum volume.
//
// Sequencing is automatic.  If release speed is non-zero at keyon
// then the chip will ride the attack from is start value to 0, then
// go to the two decay values then all the way to 3fff on release.  If
// release speed is zero, it will hold still when reaching the end of
// decay 2.  At any time if a non-zero value is written to release
// speed and the envelope is not yet in the release stage then the
// chip switches to release.
//
// A 16-bits readonly register gives the main cpu the current stage
// and raw envelope volume (without the global volume added), with
// stages numbered 0 to 3 in the two top bits and the volume in the
// bottom 14.

void swp30_device::envelope_block::clear()
{
	m_attack = 0;
	m_decay1 = 0;
	m_decay2 = 0;
	m_release_glo = 0;
	m_envelope_level = 0x3fff;
	m_envelope_mode = RELEASE;
}

void swp30_device::envelope_block::keyon()
{
	m_envelope_level = (m_attack & 0xff) << 6;
	if((m_attack & 0xff) == 0)
		m_envelope_level = 0x80 << 6;
	m_envelope_mode = ATTACK;
}

u16 swp30_device::envelope_block::status() const
{
	return (m_envelope_mode << 14) | m_envelope_level;
}

bool swp30_device::envelope_block::active() const
{
	return m_envelope_level != 0x3fff || m_envelope_mode != RELEASE;
}

int swp30_device::sounding_voices() const
{
	int n = 0;
	for (const envelope_block &e : m_envelope)
		n += e.active();
	return n;
}

u16 swp30_device::envelope_block::level_step(s32 level, u32 sample_counter)
{
	// Phase is incorrect, and very weird

	if(level >= 0x78)
		return 0x7f;

	// S-MU2000: level は負にもなる（ピッチ EG は 16 段遅らせて引く）。
	// 算術シフトなので k0 がそのまま増え、8 段下がるごとに半分の速さになる。
	// 下は -16（k0 = 10）までしか来ない
	s32 k0 = level >> 3;
	u32 k1 = u32(level) & 7;

	if(level >= 0x48) {
		k0 -= 9;
		u32 a = (4 << k0) - 1;
		u32 b = (2 << k0) - 1;
		static const u8 mx[8] = { 0x00, 0x20, 0x44, 0xa2, 0x55, 0x75, 0xee, 0xfe };
		return ((mx[k1] >> (sample_counter & 7)) & 1) ? a : b;
	}

	if(level >= 0x40) {
		if(sample_counter & 1)
			return 1;
		u32 s1 = (sample_counter & 0xe) >> 1;
		static const u8 mx[8] = { 0x00, 0x01, 0x22, 0xa8, 0x55, 0xab, 0x77, 0xfd };
		return (mx[k1] >> s1) & 1;
	}

	const u32 sh = u32(8 - k0);       // 負の level ではここが 8 より大きくなる

	if(sample_counter & util::make_bitmask<u32>(sh))
		return 0;

	static const u16 mx[8] = { 0x5555, 0x5557, 0x5757, 0x5777, 0x7777, 0x777f, 0x7f7f, 0x7fff };
	return (mx[k1] >> ((sample_counter >> sh) & 0xf)) & 1;
}

// S-MU2000: 画面がフィルタの特性を描くための口。声のフィルタと同じ step を回す
void swp30_device::filter_impulse(u16 f1a, u16 level1, u16 f2a, u16 level2, u16 fb, float *out, int n)
{
	filter_block f;
	f.filter_1_a_w(f1a);
	f.level_1_w(level1);
	f.filter_2_a_w(f2a);
	f.level_2_w(level2);
	f.filter_b_w(fb);
	u32 cur = 0;
	s32 cur2 = 0;
	f.keyon(cur, cur2);
	constexpr s16 ONE = 0x4000;
	for(int i = 0; i < n; i++)
		out[i] = float(f.step(i == 0 ? ONE : 0, cur, cur2)) / float(s32(ONE) << 6);
}

// S-MU2000: 画面がビブラートを描くための口。声の LFO と同じ step・get_pitch を回す
// （乱数の型は使わない前提。3 のときは 0 を返す）
void swp30_device::lfo_pitch_trace(u16 type_step_pitch, s16 *out, int n)
{
	lfo_block l;
	l.type_step_pitch_w(type_step_pitch);
	if(l.m_type == 3) {
		for(int i = 0; i < n; i++)
			out[i] = 0;
		return;
	}
	for(int i = 0; i < n; i++) {
		out[i] = l.get_pitch();
		l.advance();
	}
}

// S-MU2000: 画面が包絡線の時間を描くための口（level_step をそのまま）
u16 swp30_device::envelope_step(s32 speed, u32 sample_counter)
{
	return envelope_block::level_step(speed, sample_counter);
}

u16 swp30_device::envelope_block::step(u32 sample_counter)
{
	u16 result = m_envelope_level + ((m_release_glo & 0xff) << 6);
	switch(m_envelope_mode) {
	case ATTACK: {
		s32 level = m_envelope_level - level_step((m_attack >> 8) + ((m_envelope_level >> 9) << 2), sample_counter);
		if(level <= 0) {
			level = 0;
			m_envelope_mode = DECAY1;
		}
		m_envelope_level = level;
		if((m_attack & 0xff) == 0)
			result = (m_release_glo & 0xff) << 6;
		break;
	}

	case DECAY1: case DECAY2: {
		u16 reg = m_envelope_mode == DECAY1 ? m_decay1 : m_decay2;
		s32 limit = (reg & 0xff) << 6;
		s32 level = m_envelope_level;
		if(level < limit) {
			level += level_step(reg >> 8, sample_counter);
			if(level > limit)
				level = limit;
		} else if(level> limit) {
			level -= level_step(reg >> 8, sample_counter);
			if(level < limit)
				level = limit;
		}
		m_envelope_level = level;
		if(level == limit) {
			if(m_envelope_mode == DECAY1)
				m_envelope_mode = DECAY2;

			else if(m_release_glo & 0x8000)   // S-MU2000: bit 15 が立っているときだけ（doc/upstream.md の 16）
				m_envelope_mode = RELEASE;
		}
		break;
	}

	case RELEASE: {
		s32 level = m_envelope_level + level_step((m_release_glo >> 8) ^ 0x80, sample_counter);
		if(level > 0x3fff)
			level = 0x3fff;
		m_envelope_level = level;
		break;
	}
	}
	return result;
}

u16 swp30_device::envelope_block::attack_r() const
{
	return m_attack;
}

void swp30_device::envelope_block::attack_w(u16 data)
{
	m_attack = data;
}

u16 swp30_device::envelope_block::decay1_r() const
{
	return m_decay1;
}

void swp30_device::envelope_block::decay1_w(u16 data)
{
	m_decay1 = data;
}

u16 swp30_device::envelope_block::decay2_r() const
{
	return m_decay2;
}

void swp30_device::envelope_block::decay2_w(u16 data)
{
	m_decay2 = data;
}

u16 swp30_device::envelope_block::release_glo_r() const
{
	return m_release_glo;
}

void swp30_device::envelope_block::release_glo_w(u16 data)
{
	m_release_glo = data;
	// S-MU2000: MAME は release の速さが 0 でなければ release に入れていた。firmware は遅れて鳴らす層の
	// キーオンの前に 0x01xx を書く（pc 0x12E3DE）が、実機ではその層は鳴り続ける。鍵を離すときに書く値は
	// どれも bit 15 が立っている（0xA8〜0xF0）ので、bit 15 を「release せよ」の印とみる（doc/upstream.md の 16）
	if(data & 0x8000)
		m_envelope_mode = RELEASE;
}

void swp30_device::envelope_block::trigger_release()
{
	m_release_glo |= 0xff00;
	m_envelope_mode = RELEASE;
}


/*--------------------------------------------------------------------------------

LFO block

  cccccc 000101  .... .... .aaa aaaa       LFO amplitude depth
  cccccc 001010  tt.s ssss mppp pppp       LFO type, step, pitch mode, pitch depth

The LFO is a slow oscillator with a period between 0.2 and 6
seconds (0.17 to 5.2Hz).  It starts with a 18-bits counter which is
initialized to a random value on keyon.  At each sample the value
of step is added to the counter.  When sep is zero, the counter is
frozen.  In addition, somewhere in every 0x4000 block at a somewhat
unpredictable time, the counter jumps by an extra 0x40.  The final
period thus ends up being 0x3fc00/step cycles, or between 8423 and
261120 cycles.

From the 18-bits counter a 12-bit state value is created.  How
depends on the type:

 Type 0 (saw);  state is bits 6-17 of the counter

 Type 1 (triangle): state is bits 6-16 of the counter followed by a
    zero if bit 17 = 0, inverted bits 6-16 followed by a zero if
    bit 17 = 1

 Type 2 (rectangle): state is 0 if bit 17 = 0, fff if bit 17 = 1

 Type 3 (sample&hold): state is random, changes of value when bits
    9-17 change

 Amplitude LFO.  The current value of the state is multiplied by
 the amplitude depth (zero hence makes it disabled) and divided by
 0x20, giving a 14-bit, 4.10 attenuation (same format as for the
 envelope).

 Pitch LFO.  The value of the state minus 0x400 is multiplied by
 the pitch depth.  It is then shifted by 8 in coarse mode (m=1) and
 11 in fine mode (m=0).  The resulting signed value is added to the
 pitch used by the streaming block.
*/

void swp30_device::lfo_block::clear()
{
	m_counter = 0;
	m_state = 0;
	m_type = 0;
	m_step = 0;
	m_amplitude = 0;
	m_pitch_mode = false;
	m_pitch_depth = 0;
	m_r_type_step_pitch = 0;
	m_r_amplitude = 0;
}

void swp30_device::lfo_block::keyon(swp30_device &swp)
{
	// S-MU2000: MAME は乱数から始めていた。実機は同じ音を何度弾いてもビブラートとトレモロが
	// 同じ形で始まる（doc/upstream.md の 15）。カウンタは 0 から始め、三角波は下の tri_state で
	// 中央から上がり始める。乱数は MEG のディザと同じ数列なので、引く回数は変えない
	swp.rand();
	m_counter = 0;
	switch(m_type) {
	case 0: m_state = m_counter >> 6; break;
	case 1: m_state = tri_state(m_counter); break;
	case 2: m_state = m_counter & 0x20000 ? 0xfff : 0; break;
	case 3: m_state = swp.rand() & 0xfff; break;
	}
}

// S-MU2000: 三角波。MAME の式はカウンタ 0 で一番下から始まるが、実機は中央（0x800）から上がり始める。
// 1/4 周期（0x10000）ずらすと、ビブラートの深さ最大・遅れ 0 の Square Lead C5 で実機と 3 セント以内で重なる
u32 swp30_device::lfo_block::tri_state(u32 counter)
{
	const u32 c = (counter + 0x10000) & 0x3ffff;
	return c & 0x20000 ? (~c >> 5) & 0xffe : (c >> 5) & 0xffe;
}

void swp30_device::lfo_block::step(swp30_device &swp)
{
	// S-MU2000: 乱数を使わない所を advance に分けた（画面が波の形を描くのに同じものを使う）
	const u32 pc = advance();
	if(m_type == 3 && ((pc ^ m_counter) & 0x3fe00))
		m_state = swp.rand() & 0xfff;
}

u32 swp30_device::lfo_block::advance()
{
	u32 pc = m_counter;
	m_counter = (m_counter + m_step) & 0x3ffff;
	if((m_counter & 0x03fc0) == 0x02000)
		m_counter += 0x40;
	switch(m_type) {
	case 0: m_state = m_counter >> 6; break;
	case 1: m_state = tri_state(m_counter); break;
	case 2: m_state = m_counter & 0x20000 ? 0xfff : 0; break;
	}
	return pc;
}

u16 swp30_device::lfo_block::get_amplitude() const
{
	// S-MU2000: 三角波の音量側は、音程側（tri_state、中央から上がる）と違い、一番下（効きなし）から
	// 上がり始める。中央から始めていたころは、要素自身が音量の LFO を持つ XG の変化音色
	// （0/21/38・0/64/44・0/69/90・0/70/7 など）の頭が実機より最大 6dB 小さかったり大きかったりした。
	// 1/4 周期ずつ 4 通り試して、実機で 2 回ずつ録った 116 音色との差が一番小さいのがこの形
	// （合計 325 → 273。上の 6 つは 1.6〜6.0dB → 0.0〜0.1dB。doc/todo.md）。
	// モジュレーションホイールで掛けるトレモロは firmware が音量を書き換えるので、ここを通らない
	if(m_type == 1) {
		const u32 c = m_counter;
		const u32 st = c & 0x20000 ? (~c >> 5) & 0xffe : (c >> 5) & 0xffe;
		return (st * m_amplitude) >> 5;
	}
	return (m_state * m_amplitude) >> 5;
}

s16 swp30_device::lfo_block::get_pitch() const
{
	// S-MU2000: the 12-bit state is centred on 0x800, not 0x400.  With 0x400
	// the vibrato sat about half a swing sharp and swung twice as far as the
	// real MU2000 (measured on hardware: vib rate sweep, doc/upstream.md).
	s32 v = (m_state - 0x800) * m_pitch_depth;
	if(m_pitch_mode)
		return v >> 9;
	else
		return v >> 12;

}

void swp30_device::lfo_block::type_step_pitch_w(u16 data)
{
	m_r_type_step_pitch = data;
	m_type = data >> 14;
	// S-MU2000: the step is 6 bits.  The firmware sets bit 13 for vibrato rates
	// above ~72; dropping it turned a fast vibrato into a ~1Hz wobble.
	m_step = (data >> 8) & 0x3f;
	m_pitch_mode = data & 0x80;
	m_pitch_depth = data & 0x7f;
}

void swp30_device::lfo_block::amplitude_w(u16 data)
{
	m_r_amplitude = data;
	m_amplitude = data & 0x7f;
}

u16 swp30_device::lfo_block::type_step_pitch_r()
{
	return m_r_type_step_pitch;
}

u16 swp30_device::lfo_block::amplitude_r()
{
	return m_r_amplitude;
}




s32 swp30_device::volume_apply(s32 level, s32 sample)
{
	// Level is 4.10 floating point positive, and represents an attenuation
	// Sample is 16.6 signed and the result is in the same format

	// Passed-in value may have overflowed
	if(level >= 0x3fff)
		return 0;

	s32 e = level >> 10;
	s32 m = level & 0x3ff;
	s64 mul = (0x4000000 - (m << 15)) >> e;
	// S-MU2000: 掛けた結果は切り捨てない（MAME と同じ）。前は 256 刻みで 0 の側へ切り捨てていたが、それは
	// 16bit に切り詰めた実機の録音に合わせた誤りだった。浮動小数で録ると、実機の減衰の雑音はパートの音量と一緒に
	// 小さくなり、深く絞っても 0 にはならない（doc/upstream.md の 34、discussion #69）
	return s32((sample * mul) >> 26);
}

void swp30_device::awm2_step(std::array<s32, 0x40> &samples_per_chan)
{
	// S-MU2000: 休んでいる声（鳴っておらずピッチ EG も着いた声）は回さない。回しても印を立て直すだけで
	// 何も変わらない。回す順（番号の小さい順）は同じなので、LFO が引く乱数の並びも同じ
	samples_per_chan.fill(0);
	for(u64 live = ~m_awm_idle; live; live &= live - 1) {
		const int chan = std::countr_zero(live);
		// S-MU2000: 着いている声（ほとんど全部）は印を立てるだけで済ませる。peg_step の頭と同じ
		if(m_peg_cur[chan] == s32(util::sext(u32(m_pitch_offset[chan] & 0x3fff), 14)))
			m_peg_reached[chan] = 1;
		else
			peg_step(chan);
		if(!m_envelope[chan].active()) {
			if(m_peg_reached[chan] && m_peg_cur[chan] == s32(util::sext(u32(m_pitch_offset[chan] & 0x3fff), 14)))
				m_awm_idle |= u64(1) << chan;
			continue;
		}

		auto &lfo = m_lfo[chan];

		auto [sample1, trigger_release] = m_streaming[chan].step(m_wave_cache, lfo.get_pitch(), u16(m_peg_cur[chan] & 0x3fff));
		if(trigger_release)
			m_envelope[chan].trigger_release();

		// S-MU2000: 1 声の中身を追う（--dump-dac のとき、決めた声だけ）
		s32 sample2 = m_filter[chan].step(sample1, m_filter_cur[chan], m_filter2_cur[chan]);
		s32 sample3 = m_iir1[chan].step(sample2);
		s32 sample4 = volume_apply(m_envelope[chan].step(m_meg->m_sample_counter) + lfo.get_amplitude(), sample3);

		if(m_dbg_dac && chan == m_dbg_chan &&
		   m_meg->m_sample_counter >= m_dbg_dac_from &&
		   m_meg->m_sample_counter < m_dbg_dac_from + m_dbg_dac_count) {
			const auto &st = m_streaming[chan];
			const auto &fl = m_filter[chan];
			fprintf(m_dbg_dac,
			        "vox %u pos=%d wave=%d filt=%d iir=%d out=%d"
			        " f1a=%04x l1=%04x f2a=%04x l2=%04x fb=%04x"
			        " h=%d b=%d l=%d"
			        " iirA=%d,%d,%d iirB=%d,%d,%d hx=%d,%d hy=%d,%d\n",
			        m_meg->m_sample_counter, st.m_pos, sample1, sample2, sample3, sample4,
			        fl.m_filter_1_a, fl.m_level_1, fl.m_filter_2_a, fl.m_level_2, fl.m_filter_b,
			        fl.m_filter_1_h, fl.m_filter_1_b, fl.m_filter_1_l,
			        m_iir1[chan].m_a[0][0], m_iir1[chan].m_a[0][1], m_iir1[chan].m_b[0],
			        m_iir1[chan].m_a[1][0], m_iir1[chan].m_a[1][1], m_iir1[chan].m_b[1],
			        m_iir1[chan].m_hx[0], m_iir1[chan].m_hx[1],
			        m_iir1[chan].m_hy[0], m_iir1[chan].m_hy[1]);
		}

		lfo.step(*this);
		samples_per_chan[chan] = sample4;
	}
}



// S-MU2000: コンストラクタと初期化を書き換えた。
// もとは MAME の address_space / drccache / stream_alloc を組み立てていた。
swp30_device::swp30_device()
{
	m_meg_storage = std::make_unique<meg_state>();
	m_meg = m_meg_storage.get();
	m_meg->m_swp = this;
	m_meg->reset();
	m_meg_program_changed = true;
	// 静まった区画を回さない（SMU2000_MEG_SKIP=0 で切る）。毎サンプル読むので、ここで 1 回だけ
	if (const char *e = std::getenv("SMU2000_MEG_SKIP"))
		m_meg_skip_on = e[0] != '0';
	m_meg_skip_debug = std::getenv("SMU2000_MEG_SKIP_DEBUG") != nullptr;

	// MEG のプログラム空間(9bit, 64bit幅)とリバーブ RAM(18bit, 16bit幅)は
	// もとは address_map で組まれていた。ここでは実体を直に指す。
	//
	// プログラムの実体は m_meg->m_program。MAME は AS_PROGRAM に
	//   map(0x000, 0x17f).r(FUNC(swp30_device::meg_prg_map_r));
	// を貼って、そこから m_meg->m_program[address] を返していた。
	// 別の空配列を指していると命令が全部 0 になり、MEG が何も出さない
	m_reverb_ram.assign(1 << 18, 0);
	m_program_cache.set(m_meg->m_program.data(), m_meg->m_program.size() * sizeof(u64));
	m_reverb_cache.set_writable(m_reverb_ram.data(), m_reverb_ram.size() * sizeof(u16));

	// 1 サンプル分だけのバッファ。出力 20ch(DAC 4 + MELO 16)、入力 16ch(MELI)
	m_buf.reset(sound_buffer::INPUTS, sound_buffer::OUTPUTS, 1);

	reset();
}

void swp30_device::set_wave_rom(const void *base, size_t bytes)
{
	m_wave_cache.set(base, bytes);
}

void swp30_device::set_sintab(const u16 *base, size_t count)
{
	m_sintab.set(base, count);
}


// S-MU2000: device_start の置換で巻き添えになっていた meg_state::reset() を戻す
void swp30_device::meg_state::reset()
{
	std::fill(m_program.begin(),       m_program.end(),       0);
	std::fill(m_const.begin(),         m_const.end(),         0);
	std::fill(m_offset.begin(),        m_offset.end(),        0);
	std::fill(m_lfo.begin(),           m_lfo.end(),           0);
	std::fill(m_lfo_increment.begin(), m_lfo_increment.end(), 0);
	std::fill(m_lfo_counter.begin(),   m_lfo_counter.end(),   0);
	std::fill(m_map.begin(),           m_map.end(),           0);
	m_ram_read = 0;
	m_ram_write = 0;
	m_ram_index = 0;
	m_program_address = 0;
	m_pc = 0;
	std::fill(m_m.begin(), m_m.end(), 0);
	std::fill(m_r.begin(), m_r.end(), 0);
	std::fill(m_t.begin(), m_t.end(), 0);
	m_p = 0;
	std::fill(m_mw_value.begin(),     m_mw_value.end(),     0);
	std::fill(m_mw_reg.begin(),       m_mw_reg.end(),       0);
	std::fill(m_rw_value.begin(),     m_rw_value.end(),     0);
	std::fill(m_rw_reg.begin(),       m_rw_reg.end(),       0);
	std::fill(m_index_value.begin(),  m_index_value.end(),  false);
	std::fill(m_index_active.begin(), m_index_active.end(), 0);
	std::fill(m_memw_value.begin(),   m_memw_value.end(),   false);
	std::fill(m_memw_active.begin(),  m_memw_active.end(),  0);
	std::fill(m_memr_value.begin(),   m_memr_value.end(),   false);
	std::fill(m_memr_active.begin(),  m_memr_active.end(),  0);
	m_delay_3 = 0;
	m_delay_2 = 0;
	m_sample_counter = 0;
	m_retval = 0;
}

void swp30_device::reset()
{
	m_rand_seed = m_rand_seed_base;
	m_keyon_mask = 0;
	m_awm_idle = 0;
	m_meg_flag_n = m_meg_flag_z = false;
	m_meg_ix2_value.fill(0); m_meg_ix2_act.fill(0); m_meg_ram_index2 = 0;


	std::fill(m_mixer.begin(), m_mixer.end(), mixer_slot());
	m_mix_dirty[0] = m_mix_dirty[1] = ~u64(0);

	for(auto &s : m_streaming)
		s.clear();
	m_pitch_offset.fill(0);
	m_peg_rate.fill(0);
	m_peg_cur.fill(0);
	m_peg_reached.fill(0);
	m_filter_cur.fill(0);
	m_filter2_cur.fill(0);
	for(auto &f : m_filter)
		f.clear();
	for(auto &i : m_iir1)
		i.clear();
	for(auto &e : m_envelope)
		e.clear();
	for(auto &l : m_lfo)
		l.clear();

	m_meg->reset();

	m_wave_adr = 0;
	m_wave_size = 0;
	m_wave_access = 0;
	m_wave_val = 0;
	m_revram_adr = 0;
	m_revram_data = 0;
	m_revram_enable = 0;

	for(auto &s : m_nsend)
		s[0] = s[1] = 0;

	std::fill(m_meli.begin(),  m_meli.end(),  0);
	std::fill(m_melo.begin(),  m_melo.end(),  0);
	std::fill(m_adc.begin(),   m_adc.end(),   0);
}

// S-MU2000: address_map をやめ、素の分岐にした。
// レジスタは 64ch x 64 スロットの格子で、番地 = チャンネル * 0x40 + スロット（16bit 単位）。
// ハンドラは offset >> 6 でチャンネルを取り出すので、offset には chan << 6 を渡す。
// ハンドラ自体は一切変更していない。

u16 swp30_device::read16(offs_t addr)
{
	addr &= 0xfff;
	const u32 slot = addr & 0x3f;
	const u32 chan = (addr >> 6) & 0x3f;

	// --- チャンネルごとのレジスタ（全 64ch 共通、offset にチャンネル<<6 を渡す）
	switch(slot) {
	case 0x00: return filter_1_a_r(chan << 6);
	case 0x01: return level_1_r(chan << 6);
	case 0x02: return filter_2_a_r(chan << 6);
	case 0x03: return level_2_r(chan << 6);
	case 0x04: return filter_b_r(chan << 6);
	case 0x05: return lfo_amplitude_r(chan << 6);
	case 0x06: return attack_r(chan << 6);
	case 0x07: return decay1_r(chan << 6);
	case 0x08: return decay2_r(chan << 6);
	case 0x09: return release_glo_r(chan << 6);
	case 0x0a: return lfo_type_step_pitch_r(chan << 6);
	case 0x0b: return peg_rate_r(chan << 6);
	case 0x10: return pitch_offset_r(chan << 6);
	case 0x11: return pitch_r(chan << 6);
	case 0x12: return start_h_r(chan << 6);
	case 0x13: return start_l_r(chan << 6);
	case 0x14: return loop_h_r(chan << 6);
	case 0x15: return loop_l_r(chan << 6);
	case 0x16: return address_h_r(chan << 6);
	case 0x17: return address_l_r(chan << 6);
	case 0x20: return a1_r<0>(chan << 6);
	case 0x21: return meg_const_r<0>(chan << 6);
	case 0x22: return b1_r<0>(chan << 6);
	case 0x23: return meg_const_r<1>(chan << 6);
	case 0x24: return a0_r<0>(chan << 6);
	case 0x25: return meg_const_r<2>(chan << 6);
	case 0x26: return b1_r<1>(chan << 6);
	case 0x27: return meg_const_r<3>(chan << 6);
	case 0x28: return a1_r<1>(chan << 6);
	case 0x29: return meg_const_r<4>(chan << 6);
	case 0x2a: return a0_r<1>(chan << 6);
	case 0x2b: return meg_const_r<5>(chan << 6);
	case 0x30: return meg_offset_r<0>(chan << 6);
	case 0x31: return meg_offset_r<1>(chan << 6);
	case 0x32: return vol_r<0x00|0>(chan << 6);
	case 0x33: return vol_r<0x00|1>(chan << 6);
	case 0x34: return vol_r<0x00|2>(chan << 6);
	case 0x35: return route_r<0x00|0>(chan << 6);
	case 0x36: return route_r<0x00|1>(chan << 6);
	case 0x37: return route_r<0x00|2>(chan << 6);
	case 0x38: return vol_r<0x40|0>(chan << 6);
	case 0x39: return vol_r<0x40|1>(chan << 6);
	case 0x3a: return vol_r<0x40|2>(chan << 6);
	case 0x3b: return route_r<0x40|0>(chan << 6);
	case 0x3c: return route_r<0x40|1>(chan << 6);
	case 0x3d: return route_r<0x40|2>(chan << 6);
	case 0x3e: return meg_lfo_r<0>(chan << 6);
	case 0x3f: return meg_lfo_r<1>(chan << 6);
	}

	// --- 制御レジスタ（チャンネル位置に単発で置かれている）
	switch(addr) {
	case 0x04e: return internal_adr_r();
	case 0x04f: return internal_r();
	case 0x08e: return wave_adr_r<1>();
	case 0x08f: return wave_adr_r<0>();
	case 0x0ce: return wave_size_r<1>();
	case 0x0cf: return wave_size_r<0>();
	case 0x10e: return wave_access_r();
	case 0x10f: return wave_busy_r();
	case 0x30f: return u16(m_rec_pos);
	case 0x14e: return wave_val_r<1>();
	case 0x14f: return wave_val_r<0>();
	case 0x18e: return keyon_mask_r<3>();
	case 0x18f: return keyon_mask_r<2>();
	case 0x1ce: return keyon_mask_r<1>();
	case 0x1cf: return keyon_mask_r<0>();
	case 0x20e: return keyon_r();
	case 0x40f: return meg_prg_address_r();
	case 0x44e: return meg_prg_r<0>();
	case 0x44f: return meg_prg_r<1>();
	case 0x48e: return meg_prg_r<2>();
	case 0x48f: return meg_prg_r<3>();
	case 0x60e: return meg_map_r<0>();
	case 0x64e: return meg_map_r<1>();
	case 0x68e: return meg_map_r<2>();
	case 0x6ce: return meg_map_r<3>();
	case 0x70e: return meg_map_r<4>();
	case 0x74e: return meg_map_r<5>();
	case 0x78e: return meg_map_r<6>();
	case 0x7ce: return meg_map_r<7>();
	// S-MU2000: 書いた値を読み返せる。AUTO PAN 2 は表をリバーブ RAM へ直に書く前に、
	// ここが 0 でなくなるのを待つ（読めないと firmware が止まる。doc/upstream.md の 22）
	case 0x80e: return m_revram_enable;
	case 0x84e: return revram_status_r();
	case 0x98e: return revram_data_r<1>();
	case 0x98f: return revram_data_r<0>();
	}

	// S-MU2000: snd_r は MAME にも実体が無い（書き込み専用の受け皿しかない）
	return 0;
}

void swp30_device::write16(offs_t addr, u16 data)
{
	addr &= 0xfff;
	const u32 slot = addr & 0x3f;
	const u32 chan = (addr >> 6) & 0x3f;
	// S-MU2000: 書かれた声は休みから戻す（どのレジスタでも。全体のレジスタで余計に戻しても害は無い）
	m_awm_idle &= ~(u64(1) << chan);

	// S-MU2000: 環境の読み取りは 1 回だけ。レジスタ書き込みは演奏中に何千回も
	// 通るので、毎回 getenv を呼ぶとそれだけで目に見えて遅くなる
	static const char *const wtrace = getenv("WTRACE");
	if(const char *e = wtrace) {
		const u32 from = u32(atoi(e));
		if(m_meg->m_sample_counter >= from && m_meg->m_sample_counter < from + 30000)
			fprintf(stderr, "W %u ch%02x sl%02x = %04x\n", m_meg->m_sample_counter, chan, slot, data);
	}

	// --- チャンネルごとのレジスタ（全 64ch 共通、offset にチャンネル<<6 を渡す）
	switch(slot) {
	case 0x00: filter_1_a_w(chan << 6, data); return;
	case 0x01: level_1_w(chan << 6, data); return;
	case 0x02: filter_2_a_w(chan << 6, data); return;
	case 0x03: level_2_w(chan << 6, data); return;
	case 0x04: filter_b_w(chan << 6, data); return;
	case 0x05: lfo_amplitude_w(chan << 6, data); return;
	case 0x06: attack_w(chan << 6, data); return;
	case 0x07: decay1_w(chan << 6, data); return;
	case 0x08: decay2_w(chan << 6, data); return;
	case 0x09: release_glo_w(chan << 6, data); return;
	case 0x0a: lfo_type_step_pitch_w(chan << 6, data); return;
	case 0x0b: peg_rate_w(chan << 6, data); return;
	case 0x10: pitch_offset_w(chan << 6, data); return;
	case 0x11: pitch_w(chan << 6, data); return;
	case 0x12: start_h_w(chan << 6, data); return;
	case 0x13: start_l_w(chan << 6, data); return;
	case 0x14: loop_h_w(chan << 6, data); return;
	case 0x15: loop_l_w(chan << 6, data); return;
	case 0x16: address_h_w(chan << 6, data); return;
	case 0x17: address_l_w(chan << 6, data); return;
	case 0x20: a1_w<0>(chan << 6, data); return;
	case 0x21: meg_const_w<0>(chan << 6, data); return;
	case 0x22: b1_w<0>(chan << 6, data); return;
	case 0x23: meg_const_w<1>(chan << 6, data); return;
	case 0x24: a0_w<0>(chan << 6, data); return;
	case 0x25: meg_const_w<2>(chan << 6, data); return;
	// **2 段目は b1 が先**。ここを a1 と取り違えると、帰還の係数に
	// 前向きの係数（この曲では -20591 ≒ -2.5）が入り、フィルタが発散する。
	// 1 サンプルごとに符号を変えて 2.5 倍ずつ育ち、声が 22kHz の矩形波になる。
	// 実装のすぐ上にある表（IIR1 filters block）はもともとこの順で書いてある
	case 0x26: b1_w<1>(chan << 6, data); return;
	case 0x27: meg_const_w<3>(chan << 6, data); return;
	case 0x28: a1_w<1>(chan << 6, data); return;
	case 0x29: meg_const_w<4>(chan << 6, data); return;
	case 0x2a: a0_w<1>(chan << 6, data); return;
	case 0x2b: meg_const_w<5>(chan << 6, data); return;
	case 0x30: meg_offset_w<0>(chan << 6, data); return;
	case 0x31: meg_offset_w<1>(chan << 6, data); return;
	case 0x32: vol_w<0x00|0>(chan << 6, data); return;
	case 0x33: vol_w<0x00|1>(chan << 6, data); return;
	case 0x34: vol_w<0x00|2>(chan << 6, data); return;
	case 0x35: route_w<0x00|0>(chan << 6, data); return;
	case 0x36: route_w<0x00|1>(chan << 6, data); return;
	case 0x37: route_w<0x00|2>(chan << 6, data); return;
	case 0x38: vol_w<0x40|0>(chan << 6, data); return;
	case 0x39: vol_w<0x40|1>(chan << 6, data); return;
	case 0x3a: vol_w<0x40|2>(chan << 6, data); return;
	case 0x3b: route_w<0x40|0>(chan << 6, data); return;
	case 0x3c: route_w<0x40|1>(chan << 6, data); return;
	case 0x3d: route_w<0x40|2>(chan << 6, data); return;
	case 0x3e: meg_lfo_w<0>(chan << 6, data); return;
	case 0x3f: meg_lfo_w<1>(chan << 6, data); return;
	}

	// --- 制御レジスタ（チャンネル位置に単発で置かれている）
	switch(addr) {
	case 0x04e: internal_adr_w(data); return;
	case 0x08e: wave_adr_w<1>(data); return;
	case 0x08f: wave_adr_w<0>(data); return;
	case 0x0ce: wave_size_w<1>(data); return;
	case 0x0cf: wave_size_w<0>(data); return;
	case 0x10e: wave_access_w(data); return;
	case 0x30e: m_rec_ctrl = data; return;
	case 0x14e: wave_val_w<1>(data); return;
	case 0x14f: wave_val_w<0>(data); return;
	case 0x18e: keyon_mask_w<3>(data); return;
	case 0x18f: keyon_mask_w<2>(data); return;
	case 0x1ce: keyon_mask_w<1>(data); return;
	case 0x1cf: keyon_mask_w<0>(data); return;
	case 0x20e: keyon_w(data); return;
	case 0x40e: meg_lfo_commit_w(data); return;
	case 0x40f: meg_prg_address_w(data); return;
	case 0x44e: meg_prg_w<0>(data); return;
	case 0x44f: meg_prg_w<1>(data); return;
	case 0x48e: meg_prg_w<2>(data); return;
	case 0x48f: meg_prg_w<3>(data); return;
	case 0x60e: meg_map_w<0>(data); return;
	case 0x64e: meg_map_w<1>(data); return;
	case 0x68e: meg_map_w<2>(data); return;
	case 0x6ce: meg_map_w<3>(data); return;
	case 0x70e: meg_map_w<4>(data); return;
	case 0x74e: meg_map_w<5>(data); return;
	case 0x78e: meg_map_w<6>(data); return;
	case 0x7ce: meg_map_w<7>(data); return;
	case 0x80e: revram_enable_w(data); return;
	case 0x80f: revram_clear_w(data); return;
	case 0x94e: revram_adr_w<1>(data); return;
	case 0x94f: revram_adr_w<0>(data); return;
	case 0x98e: revram_data_w<1>(data); return;
	case 0x98f: revram_data_w<0>(data); return;
	}

	snd_w(addr, data);
}

// Control registers
template<int Sel> u16 swp30_device::keyon_mask_r()
{
	return m_keyon_mask >> (16*Sel);
}

template<int Sel> void swp30_device::keyon_mask_w(u16 data)
{
	m_keyon_mask = (m_keyon_mask & ~(u64(0xffff) << (16*Sel))) | (u64(data) << (16*Sel));
}

u16 swp30_device::keyon_r()
{
	return 0;
}

void swp30_device::keyon_w(u16)
{
	m_awm_idle &= ~m_keyon_mask;
	for(int chan=0; chan<64; chan++) {
		u64 mask = u64(1) << chan;
		if(m_keyon_mask & mask) {
			if(::smu2000::g_verbose) {
				m_dbg_notes.emplace_back(m_dbg_energy[chan], m_dbg_len[chan]);
				m_dbg_energy[chan] = m_dbg_len[chan] = 0;
			}
			m_streaming[chan].keyon();
			m_filter   [chan].keyon(m_filter_cur[chan], m_filter2_cur[chan]);
			m_iir1     [chan].keyon();
			m_envelope [chan].keyon();
			m_lfo      [chan].keyon(*this);
			// S-MU2000: ピッチ EG はキーオン前に書かれた初めのレベルから始める
			m_peg_cur[chan] = s32(util::sext(u32(m_pitch_offset[chan] & 0x3fff), 14));
			m_peg_reached[chan] = 1;

			if(1)
				logerror("[%08d] keyon %02x %s\n", m_meg->m_sample_counter, chan, m_streaming[chan].describe());
		}
	}
	m_keyon_mask = 0;
}


u16 swp30_device::meg_state::prg_address_r()
{
	return m_program_address;
}

void swp30_device::meg_state::prg_address_w(u16 data)
{
	m_program_address = data;
	if(m_program_address >= 0x180)
		m_program_address = 0;
}

template<int Sel> u16 swp30_device::meg_state::prg_r()
{
	constexpr offs_t shift = 48-16*Sel;
	return m_program[m_program_address] >> shift;
}

template<int Sel> void swp30_device::meg_state::prg_w(u16 data)
{
	constexpr offs_t shift = 48-16*Sel;
	constexpr u64 mask = ~(u64(0xffff) << shift);
	m_program[m_program_address] = (m_program[m_program_address] & mask) | (u64(data) << shift);

	if(Sel == 3) {
		m_program_address ++;
		if(m_program_address == 0x180)
			m_program_address = 0;
	}
}

template<int Sel> u16 swp30_device::meg_state::map_r()
{
	return m_map[Sel];
}

template<int Sel> void swp30_device::meg_state::map_w(u16 data)
{
	m_map[Sel] = data;
}



u16 swp30_device::meg_prg_address_r()
{
	return m_meg->prg_address_r();
}

void swp30_device::meg_prg_address_w(u16 data)
{
	m_meg->prg_address_w(data);
}

template<int Sel> u16 swp30_device::meg_prg_r()
{
	return m_meg->prg_r<Sel>();
}

template<int Sel> void swp30_device::meg_prg_w(u16 data)
{
	// S-MU2000: **中身が変わったときだけ**作り直させる。firmware は同じ語を書き直すことがあり、
	// そのたびに命令表と JIT を作り直していた（静まった区画を数え直すことにもなる。6.237）
	const u32 a = m_meg->m_program_address;
	const u64 before = a < 0x180 ? m_meg->m_program[a] : 0;
	m_meg->prg_w<Sel>(data);
	if(a >= 0x180) {
		m_meg_program_changed = true;
		m_meg_map_dirty = true;
	} else if(m_meg->m_program[a] != before) {
		m_meg_program_changed = true;
		m_meg_prg_dirty[a >> 6] |= u64(1) << (a & 63);
	}
}


template<int Sel> u16 swp30_device::meg_map_r()
{
	return m_meg->map_r<Sel>();
}

template<int Sel> void swp30_device::meg_map_w(u16 data)
{
	// S-MU2000: 番地の解き方は解いた命令表に焼いてあるので、作り直させる（中身が変わったときだけ）
	if(m_meg->map_r<Sel>() != data) {
		m_meg_program_changed = true;
		m_meg_map_dirty = true;
	}
	m_meg->map_w<Sel>(data);
}


template<int Sel> void swp30_device::wave_adr_w(u16 data)
{
	if(Sel)
		m_wave_adr = (m_wave_adr & 0x0000ffff) | (data << 16);
	else
		m_wave_adr = (m_wave_adr & 0xffff0000) |  data;
	logerror("wave_adr_w %08x\n", m_wave_adr);
}

template<int Sel> u16 swp30_device::wave_adr_r()
{
	return m_wave_adr >> (16*Sel);
}

template<int Sel> void swp30_device::wave_size_w(u16 data)
{
	if(Sel)
		m_wave_size = (m_wave_size & 0x0000ffff) | (data << 16);
	else
		m_wave_size = (m_wave_size & 0xffff0000) |  data;
	logerror("wave_size_w %08x\n", m_wave_size);
}

template<int Sel> u16 swp30_device::wave_size_r()
{
	return m_wave_size >> (16*Sel);
}

void swp30_device::wave_access_w(u16 data)
{
	m_wave_access = data;
	logerror("wave_access_w %04x\n", m_wave_access);
	// S-MU2000: 0x7000 は録音（sample_step）。位置は 0 から数え直す
	if(data == 0x7000)
		m_rec_pos = 0;
	// S-MU2000: 0x9000 は続けて読む（カードへの書き出し）。最初の語をここで用意する
	if(data == 0x8000 || data == 0x9000) {
		m_wave_val = m_wave_cache.read_dword(m_wave_adr);
		logerror("wave read adr=%08x size=%08x -> %08x\n", m_wave_adr, m_wave_size, m_wave_val);
	}
}

u16 swp30_device::wave_access_r()
{
	return m_wave_access;
}

u16 swp30_device::wave_busy_r()
{
	// S-MU2000: 続けて読むとき、firmware は下 8bit が 0 でなくなるのを待ってから 0x14e、0x14f の順に 1 語読む。
	// (値 & 0x40ff) が 0x4000 なら打ち切りとみなすので、残りがある間は 0x0001 を返す
	if(m_wave_access == 0x9000)
		return m_wave_size ? 0x0001 : 0xffff;
	return m_wave_size ? 0 : 0xffff;
}

template<int Sel> u16 swp30_device::wave_val_r()
{
	const u16 v = m_wave_val >> (16*Sel);
	// S-MU2000: 続けて読むときは、下の半分を読んだら次の語へ進む
	if(!Sel && m_wave_access == 0x9000 && m_wave_size) {
		m_wave_adr ++;
		m_wave_size --;
		m_wave_val = m_wave_size ? m_wave_cache.read_dword(m_wave_adr) : 0;
	}
	return v;
}

template<int Sel> void swp30_device::wave_val_w(u16 data)
{
	if(Sel)
		m_wave_val = (m_wave_val & 0x0000ffff) | (data << 16);
	else
		m_wave_val = (m_wave_val & 0xffff0000) |  data;
	if(!Sel) {
		//      logerror("wave_val_w %08x\n", m_wave_val);
		if(m_wave_access == 0x5000) {
			m_wave_cache.write_dword(m_wave_adr, m_wave_val);
			m_wave_adr ++;
			m_wave_size --;
		}
	}
}

// Encoding of the 27-bits sample values into 16-bits values to store
// and retrieve from the reverb ram.  Technically they're supposed to
// be 18-bits but the two low bits are never connected to anything.

u16 swp30_device::meg_state::revram_encode(u32 v)
{
	v &= 0x7ffffff;
	u32 s = 0;
	if(v & 0x4000000) {
		v ^= 0x7ffffff;
		s = 1;
	}
	u32 e = 15;
	while(e && !(v & (0x400 << e)))
		e --;
	u32 m = e ? (v >> (e-1)) & 0x7ff : v;
	return (e << 12) | (s << 11) | m;
}

u32 swp30_device::meg_state::revram_decode(u16 v)
{
	u32 e = (v >> 12) & 15;
	u32 s = (v >> 11) & 1;
	u32 m = v & 0x7ff;
	u32 vb = e ? (m | 0x800) << (e-1) : m;
	// S-MU2000: MAME は e = 0 の負の値を 0xffffffe0 で反転していて、下の 5bit が反転されなかった。
	// -1 を詰めて戻すと -32 になり、リバーブの尾に小さな負の値が膨らんで残る。詰め方（revram_encode）の
	// ちょうど逆になるよう全部反転する（doc/upstream.md の 18）
	if(s)
		vb ^= e ? (0xffffffff << (e-1)) & 0xffffffff : 0xffffffff;
	return vb;
}


void swp30_device::revram_enable_w(u16 data)
{
	logerror("revram enable = %04x\n", data);
	if(data == m_revram_enable)
		return;
	m_revram_enable = data;
	// S-MU2000: 無効な区画への出し入れは訳すときに省いてあるので、変わったら訳し直す。
	// プログラムが変わったときと同じで、書き込みが落ち着くまでは解釈実行で回す
	meg_jit_invalidate();
	m_meg_jit_wait = 1;
}

void swp30_device::revram_clear_w(u16 data)
{
	logerror("revram clear = %04x\n", data);

	// S-MU2000: MAME は何もしていなかった（doc/upstream.md の 8）。
	//
	// firmware はエフェクトの種類を替えるたびに、enable にビットを立ててから clear に同じビットを
	// 書く（0002・0004・0008 のように 1 ビットずつ）。ビット i は MEG のメモリ地図 m_map[i] の区画
	// （先頭 = 下 8 ビット × 1024、長さ = 2 の (10 + 8-10 ビット) 乗）で、書いた時点の地図の区画を
	// 0 にするものと読んだ。マスターとスレーブで地図が違っても、それぞれの区画に合う。
	//
	// 消さないと、前のエフェクトが残した遅延メモリの中身を新しいエフェクトのプログラムが読む。
	// パフォーマンスを選ぶのと同時に鍵盤を弾くと（Performance 002 Stereo Grand など）、
	// 残りを読んだ帰還の網が飽和して張り付き、全振幅のまま戻らなかった。実機は普通に鳴る
	for(int i = 0; i != 8; i++) {
		if(!BIT(data, i))
			continue;
		const u32 base = BIT(m_meg->m_map[i], 0, 8) << 10;
		const u32 size = 1 << (10 + BIT(m_meg->m_map[i], 8, 3));
		const u32 end = std::min<u32>(base + size, u32(m_reverb_ram.size()));
		if(base < end)
			std::fill(m_reverb_ram.begin() + base, m_reverb_ram.begin() + end, 0);
	}
}

u16 swp30_device::revram_status_r()
{
	return 0;
}

template<int Sel> void swp30_device::revram_adr_w(u16 data)
{
	if(Sel)
		m_revram_adr = (m_revram_adr & 0x0000ffff) | (data << 16);
	else
		m_revram_adr = (m_revram_adr & 0xffff0000) |  data;
}

template<int Sel> void swp30_device::revram_data_w(u16 data)
{
	if(Sel)
		m_revram_data = (m_revram_data & 0x0000ffff) | (data << 16);
	else
		m_revram_data = (m_revram_data & 0xffff0000) |  data;

	// S-MU2000: 書く値は上の 16bit が Q15（1.0 = 0x7fff）。MEG がメモリへ書く値（p >> 15）は 1.0 が 2^23 なので、
	// 符号付きで 8bit 落として同じ目盛りにする。MAME は >> 5 で、8 倍（+18dB）大きく入っていた（doc/upstream.md の 24）
	if(!Sel)
		m_reverb_cache.write_word(m_revram_adr, meg_state::revram_encode(u32(s32(m_revram_data) >> 8)));
}

template<int Sel> u16 swp30_device::revram_data_r()
{
	if(Sel)
		m_revram_data = u32(s32(meg_state::revram_decode(m_reverb_cache.read_word(m_revram_adr)) << 5) << 3);

	return Sel ? m_revram_data >> 16 : m_revram_data;
}



// Streaming block trampolines
u16 swp30_device::pitch_r(offs_t offset)
{
	return m_streaming[offset >> 6].pitch_r();
}

void swp30_device::pitch_w(offs_t offset, u16 data)
{
	m_streaming[offset >> 6].pitch_w(data);
}

// S-MU2000: チップの中のピッチ EG（doc/upstream.md の 13）。MAME はスロット 0x0B と 0x10 を読み捨てていた。
// スロット 0x10 は目標で、下の 14bit が符号付き、ピッチ（スロット 0x11）と同じ目盛り。
// bit 14 が何の印かは分かっていない。立っていなくても足す（doc/upstream.md の 13 の追記）。
// firmware はキーオンの前に初めのレベルを書き、キーオン後に段ごとの目標と速さを書いて、
// 着いた印（内部ポート 4 の bit 14）を見て次の段へ進む（0x12B81C）
u16 swp30_device::pitch_offset_r(offs_t offset)
{
	return m_pitch_offset[offset >> 6];
}

void swp30_device::pitch_offset_w(offs_t offset, u16 data)
{
	const int chan = offset >> 6;
	m_pitch_offset[chan] = data;
	if(m_peg_cur[chan] != s32(util::sext(u32(data & 0x3fff), 14)))
		m_peg_reached[chan] = 0;
}

u16 swp30_device::peg_rate_r(offs_t offset)
{
	return m_peg_rate[offset >> 6];
}

void swp30_device::peg_rate_w(offs_t offset, u16 data)
{
	m_peg_rate[offset >> 6] = data;
}

// 今の値を目標へ、速さ（スロット 0x0B の bit 14-8）で近づける。刻みは音量の EG と同じ表を
// 16 段遅らせて引く（4 分の 1 の速さ）。DuckLead の -375 セント → +100 → 0 と Bund、VoxLead の
// 鳴り始めが実機と合う。
//
// S-MU2000: 16 より小さい速さは 0 で止めていたが、実機はそこから下も続いていた。
// XG の SFX「Starship」（バンク 64 の 88 番）は速さ 8 を使う。止めていたころは
// ピッチの登りが実機の 2 倍（+1.55 半音 / 実機 +0.75 半音）になっていた。
// 表は 8 段下がるごとに半分の速さなので、符号付きのまま引けばそのまま伸びる
void swp30_device::peg_step(int chan)
{
	const s32 target = s32(util::sext(u32(m_pitch_offset[chan] & 0x3fff), 14));
	s32 cur = m_peg_cur[chan];
	if(cur == target) {
		m_peg_reached[chan] = 1;
		return;
	}
	const int rate = int((m_peg_rate[chan] >> 8) & 0x7f) - 16;
	const s32 step = m_envelope[chan].level_step(rate, m_meg->m_sample_counter);
	if(cur < target) {
		cur += step;
		if(cur > target) cur = target;
	} else {
		cur -= step;
		if(cur < target) cur = target;
	}
	m_peg_cur[chan] = cur;
	m_peg_reached[chan] = cur == target;
}

u16 swp30_device::start_h_r(offs_t offset)
{
	return m_streaming[offset >> 6].start_h_r();
}

u16 swp30_device::start_l_r(offs_t offset)
{
	return m_streaming[offset >> 6].start_l_r();
}

void swp30_device::start_h_w(offs_t offset, u16 data)
{
	m_streaming[offset >> 6].start_h_w(data);
}

void swp30_device::start_l_w(offs_t offset, u16 data)
{
	m_streaming[offset >> 6].start_l_w(data);
}

u16 swp30_device::loop_h_r(offs_t offset)
{
	return m_streaming[offset >> 6].loop_h_r();
}

u16 swp30_device::loop_l_r(offs_t offset)
{
	return m_streaming[offset >> 6].loop_l_r();
}

void swp30_device::loop_h_w(offs_t offset, u16 data)
{
	m_streaming[offset >> 6].loop_h_w(data);
}

void swp30_device::loop_l_w(offs_t offset, u16 data)
{
	m_streaming[offset >> 6].loop_l_w(data);
}

u16 swp30_device::address_h_r(offs_t offset)
{
	return m_streaming[offset >> 6].address_h_r();
}

u16 swp30_device::address_l_r(offs_t offset)
{
	return m_streaming[offset >> 6].address_l_r();
}

void swp30_device::address_h_w(offs_t offset, u16 data)
{
	m_streaming[offset >> 6].address_h_w(data);
}

void swp30_device::address_l_w(offs_t offset, u16 data)
{
	m_streaming[offset >> 6].address_l_w(data);
}


// IIR block trampolines
u16 swp30_device::filter_1_a_r(offs_t offset)
{
	return m_filter[offset >> 6].filter_1_a_r();
}

void swp30_device::filter_1_a_w(offs_t offset, u16 data)
{
	m_filter[offset >> 6].filter_1_a_w(data);
}

u16 swp30_device::level_1_r(offs_t offset)
{
	return m_filter[offset >> 6].level_1_r();
}

void swp30_device::level_1_w(offs_t offset, u16 data)
{
	m_filter[offset >> 6].level_1_w(data);
}

u16 swp30_device::filter_2_a_r(offs_t offset)
{
	return m_filter[offset >> 6].filter_2_a_r();
}

void swp30_device::filter_2_a_w(offs_t offset, u16 data)
{
	m_filter[offset >> 6].filter_2_a_w(data);
}

u16 swp30_device::level_2_r(offs_t offset)
{
	return m_filter[offset >> 6].level_2_r();
}

void swp30_device::level_2_w(offs_t offset, u16 data)
{
	m_filter[offset >> 6].level_2_w(data);
}

u16 swp30_device::filter_b_r(offs_t offset)
{
	return m_filter[offset >> 6].filter_b_r();
}

void swp30_device::filter_b_w(offs_t offset, u16 data)
{
	m_filter[offset >> 6].filter_b_w(data);
}

// FIR block trampolines
template<u32 Filter> u16 swp30_device::a0_r(offs_t offset)
{
	return m_iir1[offset >> 6].a0_r<Filter>();
}

template<u32 Filter> u16 swp30_device::a1_r(offs_t offset)
{
	return m_iir1[offset >> 6].a1_r<Filter>();
}

template<u32 Filter> u16 swp30_device::b1_r(offs_t offset)
{
	return m_iir1[offset >> 6].b1_r<Filter>();
}

template<u32 Filter> void swp30_device::a0_w(offs_t offset, u16 data)
{
	m_iir1[offset >> 6].a0_w<Filter>(data);
}

template<u32 Filter> void swp30_device::a1_w(offs_t offset, u16 data)
{
	m_iir1[offset >> 6].a1_w<Filter>(data);
}

template<u32 Filter> void swp30_device::b1_w(offs_t offset, u16 data)
{
	m_iir1[offset >> 6].b1_w<Filter>(data);
}

// Envelope block trampolines
u16 swp30_device::attack_r(offs_t offset)
{
	return m_envelope[offset >> 6].attack_r();
}

void swp30_device::attack_w(offs_t offset, u16 data)
{
	m_envelope[offset >> 6].attack_w(data);
}

u16 swp30_device::decay1_r(offs_t offset)
{
	return m_envelope[offset >> 6].decay1_r();
}

void swp30_device::decay1_w(offs_t offset, u16 data)
{
	m_envelope[offset >> 6].decay1_w(data);
}

u16 swp30_device::decay2_r(offs_t offset)
{
	return m_envelope[offset >> 6].decay2_r();
}

void swp30_device::decay2_w(offs_t offset, u16 data)
{
	m_envelope[offset >> 6].decay2_w(data);
}

u16 swp30_device::release_glo_r(offs_t offset)
{
	return m_envelope[offset >> 6].release_glo_r();
}

void swp30_device::release_glo_w(offs_t offset, u16 data)
{
	m_envelope[offset >> 6].release_glo_w(data);
}




template<int Sel> u16 swp30_device::vol_r(offs_t offset)
{
	return m_mixer[(Sel & 0x40) | (offset >> 6)].vol[Sel & 3];
}

template<int Sel> void swp30_device::vol_w(offs_t offset, u16 data)
{
	// S-MU2000: firmware は A/D パートのミキサ（MELI 6/7 など 8 個）を同じ値で 1 秒に 2000 回ほど書き直す。
	// 変わらない書き込みで並びを作り直さない
	u16 &v = m_mixer[(Sel & 0x40) | (offset >> 6)].vol[Sel & 3];
	if(v == data)
		return;
	v = data;
	mixer_mark((Sel & 0x40) | (offset >> 6));
}

template<int Sel> u16 swp30_device::route_r(offs_t offset)
{
	return m_mixer[(Sel & 0x40) | (offset >> 6)].route[Sel & 3];
}

template<int Sel> void swp30_device::route_w(offs_t offset, u16 data)
{
	u16 &r = m_mixer[(Sel & 0x40) | (offset >> 6)].route[Sel & 3];
	if(r == data)
		return;
	r = data;
	mixer_mark((Sel & 0x40) | (offset >> 6));
}

u16 swp30_device::lfo_type_step_pitch_r(offs_t offset)
{
	return m_lfo[offset >> 6].type_step_pitch_r();
}

void swp30_device::lfo_type_step_pitch_w(offs_t offset, u16 data)
{
	m_lfo[offset >> 6].type_step_pitch_w(data);
}

u16 swp30_device::lfo_amplitude_r(offs_t offset)
{
	return m_lfo[offset >> 6].amplitude_r();
}

void swp30_device::lfo_amplitude_w(offs_t offset, u16 data)
{
	m_lfo[offset >> 6].amplitude_w(data);
}

u16 swp30_device::internal_adr_r()
{
	return m_internal_adr;
}

void swp30_device::internal_adr_w(u16 data)
{
	m_internal_adr = data;
}

u16 swp30_device::internal_r()
{
	u8 chan = m_internal_adr & 0x3f;
	switch(m_internal_adr >> 8) {
	case 0:
		return m_envelope[chan].status();

	case 4:
		// S-MU2000: ピッチ EG が目標に着いたか（bit 14）。firmware はこれを見て次の段へ進む
		// （0x12B81C）。下の 14bit は今の値にしておく（読まれていない）
		return (m_peg_reached[chan] ? 0x4000 : 0) | (m_peg_cur[chan] & 0x3fff);
		// used at 44c4
		// tests & 0x4000 only
		//      logerror("read %02x.4\n", chan);
		return 0x0000;

	case 6:
		return 0x8000;
	}

	logerror("internal_r port %x channel %02x sample %d\n", m_internal_adr >> 8, m_internal_adr & 0x1f, m_meg->m_sample_counter);

	return 0;
}


// Catch-all

void swp30_device::snd_w(offs_t offset, u16 data)
{
	int chan = (offset >> 6) & 0x3f;
	int slot = offset & 0x3f;

	if(slot == 0x0b)
		return;

	std::string preg = "-";
	if(slot >= 0x21 && slot <= 0x2b && (slot & 1))
		preg = util::string_format("fp%03x", (slot-0x21)/2 + 6*chan);
	else if(slot == 0x0e || slot == 0x0f)
		preg = util::string_format("sy%02x", (slot-0x0e) + 2*chan);
	else if(slot == 0x30 || slot == 0x31)
		preg = util::string_format("dt%02x", (slot-0x30) + 2*chan);
	else if(slot >= 0x38 && slot <= 0x3a)
		preg = util::string_format("mix[%x, %02x]", slot - 0x38, chan);
	else if(slot >= 0x3b && slot <= 0x3d)
		preg = util::string_format("route[%x, %02x]", slot - 0x3b, chan);
	else if(slot == 0x3e || slot == 0x3f)
		preg = util::string_format("lfo[%02x]", (slot-0x3e) + 2*chan);
	else
		preg = util::string_format("%02x.%02x", chan, slot);

	logerror("snd_w [%04x %04x] %-5s, %04x\n", offset, offset*2, preg, data);
}



// Synthesis and meg

// S-MU2000: execute_min_cycles() は MAME 専用なので削除
// S-MU2000: execute_max_cycles() は MAME 専用なので削除
// S-MU2000: meg_prg_map() は MAME 専用なので削除
u64 swp30_device::meg_prg_map_r(offs_t address)
{
	return m_meg->m_program[address];
}

// S-MU2000: meg_reverb_map() は MAME 専用なので削除
// S-MU2000: swp30d_const_r() は MAME 専用なので削除
// S-MU2000: swp30d_offset_r() は MAME 専用なので削除
// S-MU2000: memory_space_config() は MAME 専用なので削除
// S-MU2000: create_disassembler() は MAME 専用なので削除
// S-MU2000: state_import() は MAME 専用なので削除
// S-MU2000: state_export() は MAME 専用なので削除
// S-MU2000: state_string_export() は MAME 専用なので削除
/*======================= Mixer block ============================================

  ssssss 110010  Mixer            llll llll rrrr rrrr                      Route attenuation left/right input s
  ssssss 110011  Mixer            0000 0000 1111 1111                      Route attenuation slot 0/1   input s
  ssssss 110100  Mixer            2222 2222 3333 3333                      Route attenuation slot 2/3   input s
  ssssss 110101  Mixer            fedc ba98 7654 3210                      Route mode bit 2 input s output 0-f
  ssssss 110110  Mixer            fedc ba98 7654 3210                      Route mode bit 1 input s output 0-f
  ssssss 110111  Mixer            fedc ba98 7654 3210                      Route mode bit 0 input s output 0-f
  ssssss 111000  Mixer            llll llll rrrr rrrr                      Route attenuation left/right input s+40
  ssssss 111001  Mixer            0000 0000 1111 1111                      Route attenuation slot 0/1   input s+40
  ssssss 111010  Mixer            2222 2222 3333 3333                      Route attenuation slot 2/3   input s+40
  ssssss 111011  Mixer            fedc ba98 7654 3210                      Route mode bit 2 input s+40 output 0-f
  ssssss 111100  Mixer            fedc ba98 7654 3210                      Route mode bit 1 input s+40 output 0-f
  ssssss 111101  Mixer            fedc ba98 7654 3210                      Route mode bit 0 input s+40 output 0-f

The mixer block ensures mixing and routing in the whole system,
between the AM2, the MEG, and the MELI/MELO streams.  The values
passing through are all 27-bits wide.

It has 96 mono inputs:
  - 64 outputs of the AWM2 block, numbered 0-63
  - 16 outputs of the MEG, numbered 64-79, which are read from MEG
    registers m20-m2f
  - 16 inputs (8 stereo) on the MELI ports, numbered 80-95

It has 16 stereo outputs:
  - 8 outputs on the MELO ports, numbered 0-7
  - 8 outputs to the MEG as 16 mono streams, numbered 8-15, which
    are written to MEG registers m20-m2f

Six 8-bit values provide attenuations, and three 16-bits values
provide routing for each of the 96 inputs to each of the 16
outputs.

For a given source, target pair the three bits of routing target
are interpreted following in the following way:

         210
      0: 000 - Not routed
      1: 001 - No attenuation, add to both channels
      2: 010 - No attenuation, add to left channel
      3: 011 - No attenuation, add to right channel
      4: 100 - Use attenuation slot 0
      5: 101 - Use attenuation slot 1
      6: 110 - Use attenuation slot 2
      7: 111 - Use attenuation slot 3

The attenuation slots are built from the six attenuation values.
Attenuation for a given channel (left/right) and a slot (0-3) is
the sum of the left/right attenuation and the slot attenuation.
Final value is 4.4 with >= ff hardcoded to mute.

There is space in the map for channels number 96-127.  The MUs
never touch that space, it seems that it may have (mostly negative)
impacts on the adc outputs (MEG registers m30-m33).
*/

s32 swp30_device::mixer_att(s32 sample, s32 att)
{
	if(att >= 0xff)
		return 0;
	// S-MU2000: 下 4 ビットは 32 分の 1 刻み（1 から 17/32 まで）。MAME は 16 分の 1 と
	// していたので、0x18 と 0x20 が同じ大きさになり、パンで音量が行き来していた。
	// 実機で 17 段のパンを測って ±0.1dB で合う（doc/upstream.md の 5 番）
	return (sample - ((sample * (att & 0xf)) >> 5)) >> (att >> 4);
}

void swp30_device::mixer_rebuild()
{
	for(int mix = 0; mix != 0x60; mix++) {
		if(!((m_mix_dirty[mix >> 6] >> (mix & 63)) & 1))
			continue;
		u64 route = (u64(m_mixer[mix].route[0]) << 32) | (u64(m_mixer[mix].route[1]) << 16) | m_mixer[mix].route[2];
		const std::array<u16, 3> &vol = m_mixer[mix].vol;
		auto &taps = m_mix_taps[mix];
		int n = 0;
		// 減衰なしは減衰 0 と同じ値になる（mixer_att(x, 0) == x）。0xff 以上は 0 を足すだけなので並びに入れない
		auto raw = [&](int dst) { taps[n++] = mix_tap{ u8(dst), 0, 0 }; };
		auto att = [&](int dst, u32 a) { if(a < 0xff) taps[n++] = mix_tap{ u8(dst), u8(a & 0xf), u8(a >> 4) }; };
		for(int out = 0; out != 16; out++) {
			int mode = ((route >> (out+32-2)) & 4) | ((route >> (out+16-1)) & 2) | ((route >> (out+0-0)) & 1);
			switch(mode) {
			case 0: // No routing
				break;

			case 1: // No attenuation, add to both channels
				raw(out*2);
				raw(out*2+1);
				break;

			case 2: // No attenuation, add to left channel
				raw(out*2);
				break;

			case 3: // No attenuation, add to right channel
				raw(out*2+1);
				break;

			case 4: // Use attenuation slot 0
				att(out*2,   (vol[0] >> 8)   + (vol[1] >> 8));
				att(out*2+1, (vol[0] & 0xff) + (vol[1] >> 8));
				break;

			case 5: // Use attenuation slot 1
				att(out*2,   (vol[0] >> 8)   + (vol[1] & 0xff));
				att(out*2+1, (vol[0] & 0xff) + (vol[1] & 0xff));
				break;

			case 6: // Use attenuation slot 2
				att(out*2,   (vol[0] >> 8)   + (vol[2] >> 8));
				att(out*2+1, (vol[0] & 0xff) + (vol[2] >> 8));
				break;

			case 7: // Use attenuation slot 3
				att(out*2,   (vol[0] >> 8)   + (vol[2] & 0xff));
				att(out*2+1, (vol[0] & 0xff) + (vol[2] & 0xff));
				break;
			}
		}
		m_mix_ntaps[mix] = u8(n);
	}
	m_mix_dirty[0] = m_mix_dirty[1] = 0;
	// 振り分け先の無い入力は mixer_step で見ない
	m_mix_nactive = 0;
	for(int mix = 0; mix != 0x60; mix++)
		if(m_mix_ntaps[mix])
			m_mix_active[m_mix_nactive++] = u8(mix);
}

void swp30_device::mixer_step(const std::array<s32, 0x40> &samples_per_chan)
{
	if(m_mix_dirty[0] | m_mix_dirty[1])
		mixer_rebuild();

	std::array<s32, 0x20> mixer_out;
	std::fill(mixer_out.begin(), mixer_out.end(), 0);

	for(int ai = 0; ai != m_mix_nactive; ai++) {
		const int mix = m_mix_active[ai];
		const int n = m_mix_ntaps[mix];

		s32 input;
		if(mix < 0x40)
			input = samples_per_chan[mix];
		else if(mix < 0x50)
			input = m_meg->m_m[0x20 | (mix & 0xf)];
		else
			input = m_meli[mix & 0xf];

		if(input == 0)
			continue;

		const mix_tap *t = m_mix_taps[mix].data();
		for(int i = 0; i != n; i++)
			mixer_out[t[i].dst] += (input - ((input * t[i].frac) >> 5)) >> t[i].shift;   // mixer_att と同じ
	}
	m_rec_bus = mixer_out[0x10];   // S-MU2000: 録音はミキサの出力 8 の左（sample_step）
	std::copy(mixer_out.begin() + 0x00, mixer_out.begin() + 0x10, m_melo.begin());
	std::copy(mixer_out.begin() + 0x10, mixer_out.begin() + 0x20, m_meg->m_m.begin() + 0x20);
	// S-MU2000: 外から来た音を MEG の入口に足す（軽量モードの横取りより前。どちらの道でもエフェクトに入る）
	if(m_ext_on)
		for(int i = 0; i != 16; i++)
			if(m_ext_bus[i])
				m_meg->m_m[0x20 + i] = s32(std::clamp<s64>(s64(m_meg->m_m[0x20 + i]) + m_ext_bus[i], -0x7fffffffLL, 0x7fffffffLL));
	// S-MU2000: 軽量モードでは、エフェクトへの送りを横取りして MEG には渡さない。
	// MEG 側は無音を受けるので、出てくるのはこちらの C++ のエフェクトだけになる
	if(m_native) {
		static const int SLOT[4] = { 0x24, 0x26, 0x2c, 0x28 };   // リバーブ・コーラス・バリエーション・インサーション 1
		for(int i = 0; i != 4; i++) {
			m_nsend[i][0] = m_meg->m_m[SLOT[i]];
			m_nsend[i][1] = m_meg->m_m[SLOT[i] + 1];
			m_meg->m_m[SLOT[i]] = m_meg->m_m[SLOT[i] + 1] = 0;
		}
		if(m_native_full) {
			// 乾いた音はミキサの出力 8（m20/m21）。MEG を回さないので、ミキサの入力 64-79
			// （MEG の出口）は毎サンプル 0 にしてあり、ここには声だけが集まっている
			m_ndry[0] = m_meg->m_m[0x20];
			m_ndry[1] = m_meg->m_m[0x21];
			for(int i = 0x20; i != 0x30; i++)
				m_meg->m_m[i] = 0;
		}
	}
	// 調べもの用（一時）: エフェクトへの送り 16 本を書き出す
	if(m_dbg_dac && m_meg->m_sample_counter >= m_dbg_dac_from &&
	   m_meg->m_sample_counter < m_dbg_dac_from + m_dbg_dac_count) {
		fprintf(m_dbg_dac, "send %u", m_meg->m_sample_counter);
		for(int i = 0x10; i != 0x20; i++)
			fprintf(m_dbg_dac, " s%02x=%d", i - 0x10, mixer_out[i]);
		fprintf(m_dbg_dac, "\n");
	}
}


//     MEG:

//   010000 001110  MEG/Control      .... .... .... ....    commit LFO increments on write
//   010000 001111  MEG/Control      .... ...a aaaa aaaa    program address
//   010001 00111*  MEG/Control      dddd dddd dddd dddd    program data 1/2
//   010010 00111*  MEG/Control      dddd dddd dddd dddd    program data 2/2

//   aaaaaa 100001  MEG/Data         cccc cccc cccc cccc    constant index 6*a + 0
//   aaaaaa 100011  MEG/Data         cccc cccc cccc cccc    constant index 6*a + 1
//   aaaaaa 100101  MEG/Data         cccc cccc cccc cccc    constant index 6*a + 2
//   aaaaaa 100111  MEG/Data         cccc cccc cccc cccc    constant index 6*a + 3
//   aaaaaa 101001  MEG/Data         cccc cccc cccc cccc    constant index 6*a + 4
//   aaaaaa 101011  MEG/Data         cccc cccc cccc cccc    constant index 6*a + 5
//   aaaaaa 11000a  MEG/Data         oooo oooo oooo oooo    offset index a
//   aaaaaa 11111a  MEG/LFO          pppp ttss iiii iiii    LFO index a, phase, type, shift, increment



//      General structure

//   The MEG is a DSP with 384 program steps connected to a 0x40000
//   samples ram.  Instructions are 64 bits wide, and to each
//   instruction is associated a 1.15 fixed point signed value
//   (between -1 and 1), Every third instruction (pc multiple of 3)
//   can initiate a memory access to the reverb buffer which will be
//   completed two instructions later.  Each of those instructions is
//   associated to a 16-bits address offset value.

//   The main computation unit is a MAC cell which multiplies two
//   numbers and adds a third.

//   Every 44100th of a second the 384 program steps are run once in
//   order (no branches) to compute everything.


//      Registers

//   The DSP has multiple register sets:

//   - 127 standard registers (bank 'r') numbered 01-7f, with the extra
//     register number 00 being hardwired to 0 (like in mips)

//   - 63 mmio registers (bank 'm') numbered 01-3f with 00 wired to 0,
//     which are usable as normal registers but also are used for
//     communication

//   - 8 t(emporary) registers

//   - a p register to store the result of the MAC

//   - an index register that is optionally added to the memory address

//   - two external memory data ports, one holding the value to write,
//     one holding the latest one read

//   The registers from r and m are 24 bits each, signed.  The p
//   register (and the MAC block itself) is 42 bits, 27.15.

//   The m bank is used as the communication interface with the mixer
//   and adcs.  Once every sample the registers m20 to m2f are sent as
//   stereo values to the eight MELO ports.  Registers m30 to m33 are
//   sent to the two stereo DACs.  In addition the mixer outputs to
//   the MEG are loaded in registers m20 to m2f.  That bank also
//   communicates with lfos, with the prng and with the external
//   memory data ports.


//      LFO

//   24 LFO registers are available.  The LFO registers
//   internal counters are 22 bits wide.  The LSB of the register gives
//   the increment per sample, encoded in a special 3.5 format.
//   With scale = 3bits and v = 5bits,
//     step  = base[scale] + (v << shift[scale])
//     base  = { 0, 32, 64, 128, 256, 512, 1024, 2048 }
//     shift = { 0,  0,  1,   2,   3,   4,    5,    6 }

//   The top 17 bits of the counter are extracted.  They are shifted
//   up by 0-3 bits (depending on s) and truncated at the top, then
//   the phase p selects a value to add to the state.  That gives the
//   final 17-bits state which is shaped according to the type:

//   0: sine
//   1: triangle
//   2: saw upwards
//   3: saw downwards

//   The final 16 bits value is then shifted by 7 bits to generate the
//   final positive 23-bits value.

//   Writes to the MEG/LFO register changes phase, type and shift
//   immediatly but doesn't change the increment yet.  Writing then to
//   the commit register sets all the delayed increment changes
//   simultaneously.  This allows to keep the counters from
//   independant LFOs in sync.


//      Reverb ram access

 //   8 mappings can be setup, which allow to manage rotating buffers in
//   the samples ram easily by automating masking and offset adding.  The
//   register format is: pppppsss oooooooo.  'p' is the base pc/12 at
//   which the map starts to be used.  's' is the sub-buffer size,
//   defined as 1 << (10+s).  The base offset is o << 10.  There are no
//   alignment issues, e.g. you can have a buffer at 0x28000 which is
//   0x10000 samples long.


//      Instructions

//    33333333 33333333 22222222 22222222 11111111 11111111 00000000 00000000
//    fedcba98 76543210 fedcba98 76543210 fedcba98 76543210 fedcba98 76543210
//    ABCDEFFF Grrrrrrr HHHmmmmm m-II--J- KKLLMMNN OOPPQRrr rrrrrSmm mmmm----
//    +                               + +                                ++++ = bits set at least once in the mu100 programs

//    m = low is read port, high is write port, memory register
//    r = low is read port, high is write port, regular register

//    A = seems to disable writing to p and nothing else? Used for lo-fi variation only
//    B = set index to p
//    C = set mem write register to p
//    D = temp register write enable
//    E = temp register write source, 0=const, 1=p
//    F = temp register number
//    G = r register write source, 0 = p, 1 = r register
//    H = m register write source (0, 1, 3 unknown, 2 lfo, 4 mem read, 5 rand, 6 p, 7 m register)
//    I = memory mode, none/read/write/read+1
//    J = add index to address on memory access
//    K = saturation mode (0 = none, 1 = 24.15, 2 = 0 to max positive 24.15, 3 = abs then max positive 24.15)
//    L = shift left writing to p
//    M = adder mode (0 = add, 1 = sub, 2 = add abs, 3 = binary and)
//    N = a selector (0=p, 1=r, 2=m, 3=0)
//    O = multiplier mode (0=off, 1=m1, 2=m1*m2, 3=m2)
//    P = mul 1st input = 0,3=constant, 1,2=temp register (note that 2 and 3 seem never used)
//    Q = expand 1st input
//    R = mul 2nd input = 0=r, 1=m
//    S = disable dithering when copying from p

//   The instructions are VLIW, 64-bits wide.  The VLIW structure
//   means bits of the instruction are directly associated to
//   structures in the chip (muxes, etc) instead of the usual
//   instruction encoding of normal cpus.


//      Instruction execution

//            +--------+            +--------+                   +--------+
//            | t read |            | r read |                   | m read |
//            |  port  |            |  port  |                   |  port  |
//            +----+---+            +---+----+                   +---+----+
//                 |                 r<-+                            +->m
//           +-----+-------+            | +----------------------+---+
// Constant--+ m1 selector |            +-|-------------+        |
//           +-----+-------+     +------+-+----+      +-+--------+-+
//                 |             | m2 selector |      | a selector |
//                 |             +-+-----------+      +-+--------+-+
//                 |               |                    |        |
//            +----+---+   +-------+----+      +--------+--+     |
//            | expand +---+ multiplier +------+   adder   |     |
//            +--------+   +------------+      +-----+-----+     |
//                                                   |           |
//                                              +----+----+      |
//                                              | shifter |      |
//                                              +----+----+      |
//                                                   |           |
//                                             +-----+------+    |
//                                             | saturation |    |
//                                             +-----+------+    |
//                                                   |           |
//                                                 +-+-+         |
//                                                 | p +---------+
//                                                 +-+-+
//                                                   |
//                                              +----+---+
//                                              | dither |
//                                              +----+---+
//                                                   |
//            +------+-------------+-----------+-----+---+
//            |      |             |           |         |
//         r  |      | Constant    |           |         | m  lfo, prng, mem read
//         |  |      |     |       |           |         | |  |
//     +---+--+--+ +-+-----+-+ +---+---+ +-----+-----+ +-+-+--+--+
//     | r write | | t write | | index | | mem write | | m write |
//     |  port   | |  port   | |       | | register  | |  port   |
//     +---------+ +---------+ +-------+ +-----------+ +---------+


//   Read ports are always active, but for the r and m ports if the
//   register number is 0 then the result is 0.

//   Write ports for r and m are disabled when the register number is
//   0.  T, index and mem write have explicit enable bits (D, B and C
//   respectively).  P write is disabled though the multiplier mode
//   and the A bit.  P is passthrough, e.g. if it's written to and
//   read in the same instruction the read value is the written value.

//   The m1 selector (P) chooses between the instruction-associated
//   constant and a T register.  Optionally the value can be expanded
//   from floating-point to linear (Q).  The m2 selector chooses
//   between the r and m read ports (R).  m1 and m2 are combined in
//   the multiplier block, which can multiply them together, or pass
//   m1, or pass m2 (4th case disables the write to p).

//   The a selector (N) can choose between outputting 0, m, r, p, or
//   if r or m is selected but the register number is 0, then p >> 15.
//   Then the result of the multiplier and the a selector are combined
//   in the adder, through one of four operations: add a and m, sub a
//   from m, add m to abs(a) and binary and between m and a.  Then a
//   shifter left shifts the result by 0, 1, 2 or 4 bits. Finally a
//   saturation method may be applied (K) before writing to p.  After
//   p a dither is optionally applied (S) before the value is
//   distributed to the other registers.


//   MEG quarter-sine "ROM"

//   Pretty sure it's actually a computation given how imprecise it
//   actually is (a rom would have no reason not to be perfect).  But
//   guessing what calculation gives the correct pattern of
//   imprecision is not trivial.

// S-MU2000: ここにあった 10 行を削除 — ROM 定義（sintab は外から渡す）

const std::array<u32, 256> swp30_device::meg_state::lfo_increment_table = []() {
	std::array<u32, 256> increments;
	constexpr int dt[8] = { 0, 32, 64, 128, 256, 512,  1024, 2048 };
	constexpr int sh[8] = { 0,  0,  1,   2,   3,   4,     5,    6 };

	for(u32 i=0; i != 256; i++) {
		int scale = (i >> 5) & 7;
		increments[i] = ((i & 31) << sh[scale]) + dt[scale];
	}
	return increments;
}();

// S-MU2000: ここにあった 30 行を削除 — 逆アセンブラの補助関数

u16 swp30_device::meg_state::const_r(offs_t offset)
{
	return m_const[offset];
}

void swp30_device::meg_state::const_w(offs_t offset, u16 data)
{
	if(u16(m_const[offset]) != data)
		m_swp->m_meg_const_gen++;
	m_const[offset] = data;
}

u16 swp30_device::meg_state::offset_r(offs_t offset)
{
	return m_offset[offset];
}

void swp30_device::meg_state::offset_w(offs_t offset, u16 data)
{
	// S-MU2000: 軽量モードの MEG と同じ作りの口が、番地表を読み直すのに使う
	if(m_offset[offset] != data)
		m_swp->m_meg_off_gen++;
	m_offset[offset] =  data;
}

u16 swp30_device::meg_state::lfo_r(offs_t offset)
{
	return m_lfo[offset];
}

void swp30_device::meg_state::lfo_w(offs_t offset, u16 data)
{
	// S-MU2000: 書いた LFO は、位相を 0 から数え直す（doc/upstream.md の 25）。MAME は起動から回しっぱなしで、
	// エフェクトを選んでから鳴らすまでの時間が同じでも、コーラスやフランジャーの揺れの位相が実機と揃わなかった
	if(offset < m_lfo_counter.size())
		m_lfo_counter[offset] = 0;
	m_lfo[offset] = data;
}

void swp30_device::meg_state::lfo_commit_w()
{
	for(int i=0; i != 24; i++)
		m_lfo_increment[i] = lfo_increment_table[m_lfo[i] & 0xff];
}


template<int Sel> u16 swp30_device::meg_const_r(offs_t offset)
{
	return m_meg->const_r((offset >> 6)*6 + Sel);
}

template<int Sel> void swp30_device::meg_const_w(offs_t offset, u16 data)
{
	//  logerror("meg const[%03x] = %04x / %f\n", (offset >> 6)*6 + Sel, data, s16(data) / 32768.0);
	m_meg->const_w((offset >> 6)*6 + Sel, data);
}

template<int Sel> u16 swp30_device::meg_offset_r(offs_t offset)
{
	return m_meg->offset_r((offset >> 6)*2 + Sel);
}

template<int Sel> void swp30_device::meg_offset_w(offs_t offset, u16 data)
{
	//  logerror("meg offset[%02x/%03x] = %x / %d\n", (offset >> 6)*2 + Sel, 3*((offset >> 6)*2 + Sel), data, data);
	m_meg->offset_w((offset >> 6)*2 + Sel, data);
}

template<int Sel> u16 swp30_device::meg_lfo_r(offs_t offset)
{
	return m_meg->lfo_r((offset >> 6)*2 + Sel);
}

template<int Sel> void swp30_device::meg_lfo_w(offs_t offset, u16 data)
{
	if((offset >> 6)*2 + Sel >= 0x18) {
		logerror("nolfo[%02x] = %04x\n", (offset >> 6)*2 + Sel, data);
	}
	// S-MU2000: 解いた命令表も JIT も LFO の値を実行時に読むので、作り直させない。
	// firmware はコーラスなどの揺れの速さを鳴らしている最中にも書くので、作り直すと遅い版で回る時間が長くなる
	m_meg->lfo_w((offset >> 6)*2 + Sel, data);
}

void swp30_device::meg_lfo_commit_w(u16)
{
	m_meg->lfo_commit_w();
}

void swp30_device::meg_state::lfo_step()
{
	for(int i = 0; i != 24; i++)
		m_lfo_counter[i] = (m_lfo_counter[i] + m_lfo_increment[i]) & 0x3fffff;
}

int swp30_device::meg_state::region_of(u16 pc) const
{
	const u16 key = (pc / 12) << 11;
	for(int i=0; i != 8; i++)
		if(i == 7 || m_map[i+1] <= m_map[i] || ((m_map[i+1] & 0xf800) > key))
			return i;
	return 7;
}

u32 swp30_device::meg_state::resolve_address(u16 pc, s32 offset)
{
	u16 key = (pc / 12) << 11;
	for(int i=0; i != 8; i++)
		if(i == 7 || m_map[i+1] <= m_map[i] || ((m_map[i+1] & 0xf800) > key)) {
			u32 mask = (1 << (10+BIT(m_map[i], 8, 3))) - 1;
			return (offset & mask) + (BIT(m_map[i], 0, 8) << 10);
		}
	return 0xffffffff;
}

u32 swp30_device::meg_state::get_lfo(int lfo)
{
	constexpr u32 offsets[16] = {
		0x00000, 0x02aaa, 0x04000, 0x05555,
		0x08000, 0x0aaaa, 0x0c000, 0x0d555,
		0x10000, 0x12aaa, 0x14000, 0x15555,
		0x18000, 0x1aaaa, 0x1c000, 0x1d555,
	};

	u32 base = (m_lfo_counter[lfo] >> 5);
	base = base << ((m_lfo[lfo] >> 8) & 3);
	base = base + offsets[m_lfo[lfo] >> 12];
	base = base & 0x1ffff;
	u32 res;
	switch((m_lfo[lfo] >> 10) & 3) {
	case 0: // sine
		if(base < 0x8000)
			res = m_swp->m_sintab[base];
		else if(base < 0x10000)
			res = m_swp->m_sintab[(base & 0x7fff) ^ 0x7fff];
		else if(base < 0x18000)
			res = m_swp->m_sintab[base & 0x7fff] ^ 0xffff;
		else
			res = m_swp->m_sintab[(base & 0x7fff) ^ 0x7fff] ^ 0xffff;
		break;

	case 1: // tri
		res = (base + 0x8000) & 0x1ffff;
		if(res & 0x10000)
			res ^= 0x1ffff;
		break;

	case 2: // saw up
		res = base >> 1;
		break;

	case 3: // saw down
		res = (base ^ 0x1ffff) >> 1;
		break;
	}
	return res << 7;
}

// Expand the first multiplier input

s16 swp30_device::meg_state::m1_expand(s16 v)
{
	if(v < 0)
		return 0;
	u32 s = v >> 12;
	v = 0x1000 | (v & 0xfff);
	return (s == 5) ? v : (s < 5) ? (v >> (5-s)) : (v << (s-5));
}

// S-MU2000: ここにあった 133 行を削除 — MEG の逆アセンブラ

void swp30_device::meg_state::call_rand(void *ms)
{
	auto *ms1 = static_cast<meg_state *>(ms);
	ms1->m_retval = ms1->m_swp->rand();
}

void swp30_device::meg_state::call_revram_encode(void *ms)
{
	auto *ms1 = static_cast<meg_state *>(ms);
	ms1->m_retval = revram_encode(ms1->m_retval);
}

void swp30_device::meg_state::call_revram_decode(void *ms)
{
	auto *ms1 = static_cast<meg_state *>(ms);
	ms1->m_retval = revram_decode(ms1->m_retval);
}

// S-MU2000: ここにあった 374 行を削除 — MEG の DRC 生成（インタプリタ経路を使うので不要）

// S-MU2000: 命令をあらかじめ解いておく。プログラムが変わったときだけ呼ぶ。
// 取り出すビットの位置は step() が使っていたものと同じ
void swp30_device::meg_state::decode_program()
{
	for(u32 pc = 0; pc != 0x180; pc++) {
		const u64 opcode = m_program[pc];
		decoded &d = m_decoded[pc];
		d.sm        = BIT(opcode, 0x04, 6);
		d.sr        = BIT(opcode, 0x0b, 7);
		d.dm        = BIT(opcode, 0x27, 6);
		d.dr        = BIT(opcode, 0x30, 7);
		d.t         = BIT(opcode, 0x38, 3);
		d.mmode     = BIT(opcode, 0x16, 2);
		d.m1t       = BIT(opcode, 0x14, 2);
		d.asel      = BIT(opcode, 0x18, 2);
		d.rop       = BIT(opcode, 0x1a, 2);
		d.shift     = BIT(opcode, 0x1c, 2);
		d.clamp     = BIT(opcode, 0x1e, 2);
		d.dm_src    = BIT(opcode, 0x2d, 3);
		d.memop     = BIT(opcode, 0x24, 2);
		d.m1_expand = BIT(opcode, 0x13);
		d.m2_from_m = BIT(opcode, 0x12);
		d.dr_from_r = BIT(opcode, 0x37);
		d.no_noise  = BIT(opcode, 0x0a);
		// S-MU2000: idx（bit 0x3e）と mw（bit 0x3d）が両方立った命令は、どちらでもなく 2 つ目の idx に書く（doc/upstream.md の 32）。
		// ロータリーの系統と V-FLANGER・MULTI COMP が、bit 0x22 の付いた読み出しでこれを足す。upstream 26 の「mw を取り込まない」は、このためだった
		d.index     = BIT(opcode, 0x3e) && !BIT(opcode, 0x3d);
		d.index2    = BIT(opcode, 0x3e) && BIT(opcode, 0x3d);
		d.memw      = BIT(opcode, 0x3d) && !BIT(opcode, 0x3e);
		d.mem_use_index2 = BIT(opcode, 0x22);
		d.t_write   = BIT(opcode, 0x3b);
		d.t_from_p  = BIT(opcode, 0x3c);
		d.mem_use_index = BIT(opcode, 0x21);
		d.mem_table = BIT(opcode, 0x23);
	}
}

// S-MU2000: p（27.15）を 24bit のレジスタに詰める。
//
// **24bit で折り返す**（MAME と同じ）。ただし、正の限界にちょうど張り付いた p にディザ（1 LSB に満たない雑音）が
// 乗って 1 つだけはみ出したとき（0x800000 / -0x800001）は、限界に止める。
// 前は全部を上下で止めていたが、それだと位相を足し続けて回すレジスタ（RING MOD の搬送波など）が
// 限界に張り付いて止まる（doc/upstream.md の 23）。飽和させたい命令は、p の段で止まっている（upstream 20）
static inline u32 meg_pack24(s64 p)
{
	// S-MU2000: 0 の側へ切り捨てる。負の無限大の側（>> 15）だと、音が止んだあとも IIR の段が
	// 1 LSB ずつの行き来を続け、-66dB の雑音と直流が残る。実機は数秒でぴったり 0 になる（doc/upstream.md の 18）
	s64 q = p / 32768;
	if(q ==  0x800000) q =  0x7fffff;
	if(q == -0x800001) q = -0x800000;
	return u32(util::sext(s32(q), 24));
}

// S-MU2000: 遅延メモリへ書く値も 0 の側へ切り捨てる（meg_pack24 と同じ。doc/upstream.md の 39、discussion #69）。
// MAME は p >> 15（負の無限大の側）で、負の値が 0 に戻らず、音が止んだあともリバーブが -100dB あたりで
// 鳴り続けた（数百 Hz の音）。実機は離して 2〜3 秒で消える
static inline s64 meg_mem_value(s64 p)
{
	return p / 32768;
}

// S-MU2000: MEG の分岐（doc/upstream.md の 11）。
//
// MAME は「分岐は無い」としていて、bit 0x3f の立った命令を ALU の無い命令として
// 読み流していた。LO-FI と DYNA FLT/FLANG/PHASE のプログラムにだけ現れる。並びから:
//
//   * bit 0x20 の立った命令は、結果（p）が負か、0 かを覚える
//   * bit 0x3f の立った命令は、bit 0x18-0x1f の条件が合えば、bit 0x10-0x17 の番地
//     （同じ 256 番地の中）まで、あいだの命令を何もしない命令にする
//   * 条件: bit 3 が 0 なら必ず。1 なら bit 2 が 1 で負のとき、0 で負でないとき。
//     bit 1 が立っていれば 0 のときも
//
// LO-FI では、振幅を ±m19 で止める 2 か所と、サンプリング周波数を落とす
// サンプル＆ホールドの間引き、DYNA 系では包絡の最大値の追従と整流がこの形
static inline bool meg_cond(u8 cond, bool n, bool z)
{
	if(!BIT(cond, 3))
		return true;
	bool c = BIT(cond, 2) ? n : !n;
	if(BIT(cond, 1))
		c = c || z;
	return c;
}

void swp30_device::meg_state::step()
{
	// S-MU2000: debugger_instruction_hook は削除

	// All register writes are delayed by 3 cycles, probably a pipeline
	// Register 0 in both banks are wired to value 0
	if(m_mw_reg[m_delay_3])
		m_m[m_mw_reg[m_delay_3]] = m_mw_value[m_delay_3];

	if(m_rw_reg[m_delay_3])
		m_r[m_rw_reg[m_delay_3]] = m_rw_value[m_delay_3];

	// Index is similarly delayed
	if(m_index_active[m_delay_3])
		m_ram_index = m_index_value[m_delay_3];
	if(m_swp->m_meg_ix2_act[m_delay_3])
		m_swp->m_meg_ram_index2 = m_swp->m_meg_ix2_value[m_delay_3];

	// Memory read and write ports are delayed by 2 cycles
	if(m_memw_active[m_delay_2]) {
		m_ram_write = m_memw_value[m_delay_2];
		m_memw_active[m_delay_2] = false;
	}
	if(m_memr_active[m_delay_2]) {
		m_ram_read = m_memr_value[m_delay_2];
		m_memr_active[m_delay_2] = false;
	}

	// S-MU2000: 分岐で飛び越している命令は何もしない
	if(m_swp->m_meg_skip_to) {
		if(m_pc < m_swp->m_meg_skip_to) {
			m_mw_reg[m_delay_3] = 0;
			m_rw_reg[m_delay_3] = 0;
			m_memw_active[m_delay_2] = false;
			m_index_active[m_delay_3] = false;
			m_swp->m_meg_ix2_act[m_delay_3] = 0;
			m_t_value[m_delay_2] = s16(std::clamp<s64>(m_p >> (15+8), -0x8000, 0x7fff));
			m_delay_3 = m_delay_3 == 2 ? 0 : m_delay_3 + 1;
			m_delay_2 ^= 1;
			m_pc ++;
			m_icount --;
			if(m_pc == 0x180)
				m_pc = 0;
			return;
		}
		m_swp->m_meg_skip_to = 0;
	}
	if(BIT(m_program[m_pc], 0x3f)) {
		const u64 opc = m_program[m_pc];
		if(meg_cond(BIT(opc, 0x18, 8), m_swp->m_meg_flag_n, m_swp->m_meg_flag_z)) {
			const u16 target = (m_pc & ~0xff) | BIT(opc, 0x10, 8);
			if(target > m_pc)
				m_swp->m_meg_skip_to = target;
		}
		// S-MU2000: 分岐の命令も t を書く（DYNA 系は分岐と同時に「大きさ - 包絡線」を t に入れる。doc/upstream.md の 31）
		if(m_decoded[m_pc].t_write)
			m_t[m_decoded[m_pc].t] = m_decoded[m_pc].t_from_p ? m_t_value[m_delay_2] : m_const[m_pc];
		m_mw_reg[m_delay_3] = 0;
		m_rw_reg[m_delay_3] = 0;
		m_memw_active[m_delay_2] = false;
		m_index_active[m_delay_3] = false;
		m_swp->m_meg_ix2_act[m_delay_3] = 0;
		m_t_value[m_delay_2] = s16(std::clamp<s64>(m_p >> (15+8), -0x8000, 0x7fff));
		m_delay_3 = m_delay_3 == 2 ? 0 : m_delay_3 + 1;
		m_delay_2 ^= 1;
		m_pc ++;
		m_icount --;
		if(m_pc == 0x180)
			m_pc = 0;
		return;
	}

	// S-MU2000: 解いておいた形を使う。中身は decode_program() が入れている
	const decoded &d = m_decoded[m_pc];

	const int sm = d.sm;
	const int sr = d.sr;
	const int dm = d.dm;
	const int dr = d.dr;
	const int t  = d.t;

	const u32 mmode = d.mmode;
	// S-MU2000: 掛け算の無い形（mmode 0）でも、加算器・シフト・飽和は p にかかる（doc/upstream.md の 21 と 30）
	if(mmode != 0 || d.shift || d.clamp || d.rop) {
		const u32 m1t = d.m1t;
		// S-MU2000: 第 1 入力の選び方 2 は、直前に印を立てた結果が負なら t、そうでなければ定数（doc/upstream.md の 29）
		s64 m1 = m1t == 1 ? m_t[t] : m1t == 2 ? (m_swp->m_meg_flag_n ? m_t[t] : m_const[m_pc]) : m_const[m_pc];
		if(d.m1_expand)
			m1 = m1_expand(m1);

		s64 m2 = d.m2_from_m ? m_m[sm] : m_r[sr];

		s64 m;
		switch(mmode) {
		case 0:
			m = 0;
			break;
		case 1:
			m = m1 << (8+15);
			break;
		case 2:
			m = m1 * m2;
			break;
		case 3:
			m = m2 << 15;
			break;
		}

		s64 a;
		switch(d.asel) {
		case 0: a = m_p; break;
		case 1: a = sr ? s64(m_r[sr]) << 15 : m_p >> 15; break;
		case 2: a = sm ? s64(m_m[sm]) << 15 : m_p >> 15; break;
		case 3: a = 0; break;
		}

		s64 r;
		switch(d.rop) {
		case 0:
			r = m + a;
			break;
		case 1:
			r = m - a;
			break;
		case 2:
			r = m + (a < 0 ? -a : a);
			break;
		case 3:
			r = m & a;
			break;
		}

		const int shift = d.shift;
		if(shift)
			r <<= shift == 3 ? 4 : shift;

		// wrap at 42 bits (27.15)
		// S-MU2000: 飽和する形（=s など）は折り返さずに止める（doc/upstream.md の 20）
		if(d.clamp == 0)
			r = util::sext(r, 42);

		switch(d.clamp) {
		case 0:
			break;
		case 1:
			r = std::clamp<s64>(r, -0x4000000000, 0x3fffffffff);
			break;
		case 2:
			r = std::clamp<s64>(r, 0, 0x3fffffffff);
			break;
		case 3:
			r = std::min<s64>(r < 0 ? -r : r, 0x3fffffffff);
			break;
		}

		m_p = r;
		if(BIT(m_program[m_pc], 0x20)) {
			m_swp->m_meg_flag_n = r < 0;
			m_swp->m_meg_flag_z = r == 0;
		}
	}

	m_mw_reg[m_delay_3] = dm;
	if(dm) {
		u32 v;
		switch(d.dm_src) {
		case 0: case 1: case 2: case 3:
			v = get_lfo(m_pc >> 4);
			break;
		case 4: v = m_ram_read; break;
		case 5: v = m_swp->rand() & 0xffffff; if(v & 0x00800000) v |= 0xff000000; break;
		case 6: {
			s64 p = m_p;
			if(!d.no_noise)
				p += m_swp->rand() & 0x07e0;
			v = meg_pack24(p);
			break;
		}
		case 7: v = m_m[sm]; break;
		}
		m_mw_value[m_delay_3] = v;
	}

	m_rw_reg[m_delay_3] = dr;
	if(dr) {
		u32 v;
		if(d.dr_from_r)
			v = m_r[sr];
		else {
			s64 p = m_p;
			if(!d.no_noise)
				p += m_swp->rand() & 0x07e0;
			v = meg_pack24(p);
		}
		m_rw_value[m_delay_3] = v;
	}

	if(d.memw) {
		m_memw_active[m_delay_2] = true;
		m_memw_value[m_delay_2] = meg_mem_value(m_p);
	} else
		m_memw_active[m_delay_2] = false;

	if(d.index) {
		m_index_active[m_delay_3] = true;
		m_index_value[m_delay_3] = m_p >> (15+8);
	} else
		m_index_active[m_delay_3] = false;
	m_swp->m_meg_ix2_act[m_delay_3] = d.index2;
	if(d.index2)
		m_swp->m_meg_ix2_value[m_delay_3] = m_p >> (15+8);

	// T write lookups the p value from two cycles before, but which
	// bits depends on the presence of index setting
	if(d.t_write) {
		if(d.t_from_p)
			m_t[t] = m_t_value[m_delay_2];
		else
			m_t[t] = m_const[m_pc];
	}
	// t は 16bit。p を 23bit 落としたものがそのまま入るが、p が飽和して
	// いると 0x8000 になって符号が裏返る。ここも上下で止める
	// （index 付きのときは 15bit の切り出しで、別の使い方）
	m_t_value[m_delay_2] = (d.index || d.index2) ? s16((m_p >> 8) & 0x7fff)
	                               : s16(std::clamp<s64>(m_p >> (15+8), -0x8000, 0x7fff));

	// Memory access
	switch(d.memop) {
	case 1: {
		// S-MU2000: 区画が無効の間（エフェクトの種類を替えている最中など）は、
		// 遅延メモリへの書き込みを落とす（doc/upstream.md の 33）
		if(BIT(m_swp->m_revram_enable, region_of(m_pc)))
			break;
		u32 address = resolve_address(m_pc, m_offset[m_pc/3] + (d.mem_use_index ? m_ram_index : 0) + (d.mem_use_index2 ? m_swp->m_meg_ram_index2 : 0) - m_sample_counter);
		if(address != 0xffffffff)
			// S-MU2000: リバーブ RAM も実体は素の配列。18bit ぶんで折り返す
			m_swp->m_reverb_ram[address & 0x3ffff] = revram_encode(m_ram_write);
		break;
	}
	case 2: case 3: {
		// S-MU2000: bit 0x23 の付いた読み出しは、リバーブ RAM の絶対番地（offset + idx）を読む。サンプルの数え上げを引かず、
		// map も通さない。firmware が種類を読み込むときに、波形や曲線の表をここへ直に書いている（doc/upstream.md の 24）
		if(d.mem_table) {
			const u32 address = (u32(m_offset[m_pc/3]) + (d.mem_use_index ? m_ram_index : 0) + (d.mem_use_index2 ? m_swp->m_meg_ram_index2 : 0) + (d.memop == 3 ? 1 : 0)) & 0x3ffff;
			m_memr_value[m_delay_2] = revram_decode(m_swp->m_reverb_ram[address]);
			m_memr_active[m_delay_2] = true;
			break;
		}
		// 区画が無効の間は 0 が返る（書き込みと同じく doc/upstream.md の 33）
		if(BIT(m_swp->m_revram_enable, region_of(m_pc))) {
			m_memr_value[m_delay_2] = 0;
			m_memr_active[m_delay_2] = true;
			break;
		}
		u32 address = resolve_address(m_pc, m_offset[m_pc/3] + (d.mem_use_index ? m_ram_index : 0) + (d.mem_use_index2 ? m_swp->m_meg_ram_index2 : 0) - m_sample_counter + (d.memop == 3 ? 1 : 0));
		if(address != 0xffffffff) {
			const u16 val = m_swp->m_reverb_ram[address & 0x3ffff];
			m_memr_value[m_delay_2] = revram_decode(val);
			m_memr_active[m_delay_2] = true;
		}
		break;
	}
	}

	// S-MU2000: 1 命令ずつ追う。p と、この命令が読んだ m/r を出す
	if(m_swp->m_dbg_meg && m_pc >= m_swp->m_dbg_meg_pc0 && m_pc < m_swp->m_dbg_meg_pc1 &&
	   m_sample_counter >= m_swp->m_dbg_meg_from &&
	   m_sample_counter < m_swp->m_dbg_meg_from + m_swp->m_dbg_meg_count)
		fprintf(m_swp->m_dbg_meg, "%u %03x p=%lld m%02x=%d r%02x=%d t%d=%d\n",
		        m_sample_counter, m_pc, (long long)m_p,
		        sm, m_m[sm], sr, m_r[sr], t, m_t[t]);

	m_delay_3 ++;
	if(m_delay_3 == 3)
		m_delay_3 = 0;

	m_delay_2 ++;
	if(m_delay_2 == 2)
		m_delay_2 = 0;

	m_pc ++;
	m_icount --;

	if(m_pc == 0x180)
		m_pc = 0;
}

// S-MU2000: サンプルの切れ目で、遅れて入る m/r/index の書き込みを流し切る。
//
// 書き込みは 3 命令遅れるので、プログラムの最後の 3 命令（0x17d-0x17f）が
// 書いたものは、そのままだと次のサンプルの 0x000-0x002 で入る。ところが
// ミキサはその前に m20-m2f へ送りを書き、m20-m3f の出口を読む。TALK MOD は
// インサーションの出口 m2c/m2d を 0x17e/0x17f で書くので、出口はミキサに
// 読まれず、次のサンプルの頭で送りを出口の古い値で潰してしまい、音が全く
// 出なかった（パフォーマンス 017 FeedbackEG など）。ヤマハがこう書いている
// 以上、実機はサンプルの切れ目で書き込みが済んでからミキサとやり取りする
// はず（切れ目に空きのクロックがあると見る）。doc/upstream.md の 9
void swp30_device::meg_state::flush_writes()
{
	for(u32 i = 0; i != 3; i++) {
		const u32 k = (m_delay_3 + i) % 3;
		if(m_mw_reg[k])
			m_m[m_mw_reg[k]] = m_mw_value[k];
		if(m_rw_reg[k])
			m_r[m_rw_reg[k]] = m_rw_value[k];
		if(m_index_active[k])
			m_ram_index = m_index_value[k];
		if(m_swp->m_meg_ix2_act[k])
			m_swp->m_meg_ram_index2 = m_swp->m_meg_ix2_value[k];
		m_mw_reg[k] = 0;
		m_rw_reg[k] = 0;
		m_index_active[k] = false;
		m_swp->m_meg_ix2_act[k] = 0;
	}
}

// S-MU2000: 命令ごとの判定を、プログラムが変わったときに 1 回だけ済ませる。
// step() が毎回やっていた分岐のうち、プログラムと番地の割り当て（m_map）
// だけで決まるものをここで解く
void swp30_device::meg_state::build_ops(op *ops) const
{
	for(u32 pc = 0; pc != 0x180; pc++) {
		const decoded &d = m_decoded[pc];
		op &o = ops[pc];
		o = op{};
		o.alu       = d.mmode != 0 || d.shift || d.clamp || d.rop;   // mmode 0 でも加算器・シフト・飽和はかかる（upstream 21・30）
		o.mmode     = d.mmode;
		o.m1_from_t = d.m1t == 1 ? 1 : d.m1t == 2 ? 2 : 0;   // 2 は印で t と定数を選ぶ
		o.m1_expand = d.m1_expand;
		o.m2_from_m = d.m2_from_m;
		switch(d.asel) {
		case 0: o.asel = 0; break;
		case 1: o.asel = d.sr ? 1 : 3; break;
		case 2: o.asel = d.sm ? 2 : 3; break;
		case 3: o.asel = 4; break;
		}
		o.rop       = d.rop;
		o.shift     = d.shift == 3 ? 4 : d.shift;
		o.clamp     = d.clamp;
		o.sm = d.sm; o.sr = d.sr; o.dm = d.dm; o.dr = d.dr; o.t = d.t;
		o.dm_src    = d.dm_src;
		o.no_noise  = d.no_noise;
		o.dr_from_r = d.dr_from_r;
		o.memw      = d.memw;
		o.index     = d.index;
		o.index2    = d.index2;
		o.mem_use_index2 = d.mem_use_index2;
		o.t_write   = d.t_write;
		o.t_from_p  = d.t_from_p;
		o.memop     = d.memop;
		o.mem_table = d.mem_table;
		o.mem_use_index = d.mem_use_index;
		o.lfo          = pc >> 4;
		o.offset_index = pc / 3;
		o.latch  = BIT(m_program[pc], 0x20);
		o.jump   = BIT(m_program[pc], 0x3f);
		o.cond   = BIT(m_program[pc], 0x18, 8);
		o.target = (pc & ~0xff) | BIT(m_program[pc], 0x10, 8);

		// resolve_address() と同じ選び方。i == 7 で必ず止まるので「無し」は無い
		const u16 key = (pc / 12) << 11;
		for(int i=0; i != 8; i++)
			if(i == 7 || m_map[i+1] <= m_map[i] || ((m_map[i+1] & 0xf800) > key)) {
				o.addr_mask = (1 << (10+BIT(m_map[i], 8, 3))) - 1;
				o.addr_base = BIT(m_map[i], 0, 8) << 10;
				o.region    = u8(i);
				break;
			}
	}
}

// S-MU2000: 1 サンプルぶん（384 命令）をまとめて回す。中身は step() と同じ。
// 384 は 3 でも 2 でも割り切れるので、遅延の輪の位置は回し終えると元に戻る
void swp30_device::meg_state::run_program(const op *ops)
{
	u32 d3 = m_delay_3, d2 = m_delay_2;
	s64 p = m_p;
	const u32 sample_counter = m_sample_counter;
	bool flag_n = m_swp->m_meg_flag_n, flag_z = m_swp->m_meg_flag_z;
	auto &ix2_value = m_swp->m_meg_ix2_value;
	auto &ix2_act = m_swp->m_meg_ix2_act;
	s32 &ram_index2 = m_swp->m_meg_ram_index2;
	u32 skip_to = 0;

	for(u32 pc = 0; pc != 0x180; pc++) {
		const op &o = ops[pc];

		if(m_mw_reg[d3])
			m_m[m_mw_reg[d3]] = m_mw_value[d3];
		if(m_rw_reg[d3])
			m_r[m_rw_reg[d3]] = m_rw_value[d3];
		if(m_index_active[d3])
			m_ram_index = m_index_value[d3];
		if(ix2_act[d3])
			ram_index2 = ix2_value[d3];
		if(m_memw_active[d2]) {
			m_ram_write = m_memw_value[d2];
			m_memw_active[d2] = false;
		}
		if(m_memr_active[d2]) {
			m_ram_read = m_memr_value[d2];
			m_memr_active[d2] = false;
		}

		// S-MU2000: 分岐（step() と同じ）。飛び越す命令と分岐の命令は何もしない
		if(skip_to && pc >= skip_to)
			skip_to = 0;
		if(skip_to || o.jump) {
			if(!skip_to && meg_cond(o.cond, flag_n, flag_z) && o.target > pc)
				skip_to = o.target;
			// 分岐の命令も t を書く（step() と同じ）
			if(o.jump && o.t_write)
				m_t[o.t] = o.t_from_p ? m_t_value[d2] : m_const[pc];
			m_mw_reg[d3] = 0;
			m_rw_reg[d3] = 0;
			m_memw_active[d2] = false;
			m_index_active[d3] = false;
			ix2_act[d3] = 0;
			m_t_value[d2] = s16(std::clamp<s64>(p >> (15+8), -0x8000, 0x7fff));
			d3 = d3 == 2 ? 0 : d3 + 1;
			d2 ^= 1;
			continue;
		}

		if(o.alu) {
			s64 m1 = o.m1_from_t == 1 ? m_t[o.t] : o.m1_from_t == 2 ? (flag_n ? m_t[o.t] : m_const[pc]) : m_const[pc];
			if(o.m1_expand)
				m1 = m1_expand(m1);
			s64 m2 = o.m2_from_m ? m_m[o.sm] : m_r[o.sr];

			s64 m;
			switch(o.mmode) {
			case 0:  m = 0; break;
			case 1:  m = m1 << (8+15); break;
			case 2:  m = m1 * m2; break;
			default: m = m2 << 15; break;
			}

			s64 a;
			switch(o.asel) {
			case 0:  a = p; break;
			case 1:  a = s64(m_r[o.sr]) << 15; break;
			case 2:  a = s64(m_m[o.sm]) << 15; break;
			case 3:  a = p >> 15; break;
			default: a = 0; break;
			}

			s64 r;
			switch(o.rop) {
			case 0:  r = m + a; break;
			case 1:  r = m - a; break;
			case 2:  r = m + (a < 0 ? -a : a); break;
			default: r = m & a; break;
			}

			r <<= o.shift;
			if(o.clamp == 0)
				r = util::sext(r, 42);

			switch(o.clamp) {
			case 0:  break;
			case 1:  r = std::clamp<s64>(r, -0x4000000000, 0x3fffffffff); break;
			case 2:  r = std::clamp<s64>(r, 0, 0x3fffffffff); break;
			default: r = std::min<s64>(r < 0 ? -r : r, 0x3fffffffff); break;
			}
			p = r;
			if(o.latch) {
				flag_n = r < 0;
				flag_z = r == 0;
			}
		}

		m_mw_reg[d3] = o.dm;
		if(o.dm) {
			u32 v;
			switch(o.dm_src) {
			case 0: case 1: case 2: case 3:
				v = get_lfo(o.lfo);
				break;
			case 4: v = m_ram_read; break;
			case 5: v = m_swp->rand() & 0xffffff; if(v & 0x00800000) v |= 0xff000000; break;
			case 6: {
				s64 q = p;
				if(!o.no_noise)
					q += m_swp->rand() & 0x07e0;
				v = meg_pack24(q);
				break;
			}
			default: v = m_m[o.sm]; break;
			}
			m_mw_value[d3] = v;
		}

		m_rw_reg[d3] = o.dr;
		if(o.dr) {
			u32 v;
			if(o.dr_from_r)
				v = m_r[o.sr];
			else {
				s64 q = p;
				if(!o.no_noise)
					q += m_swp->rand() & 0x07e0;
				v = meg_pack24(q);
			}
			m_rw_value[d3] = v;
		}
		if(o.rand_n)
			m_swp->rand_skip(o.rand_n);

		m_memw_active[d2] = o.memw;
		if(o.memw)
			m_memw_value[d2] = meg_mem_value(p);

		m_index_active[d3] = o.index;
		if(o.index)
			m_index_value[d3] = p >> (15+8);
		ix2_act[d3] = o.index2;
		if(o.index2)
			ix2_value[d3] = p >> (15+8);

		if(o.t_write)
			m_t[o.t] = o.t_from_p ? m_t_value[d2] : m_const[pc];
		m_t_value[d2] = (o.index || o.index2) ? s16((p >> 8) & 0x7fff)
		                        : s16(std::clamp<s64>(p >> (15+8), -0x8000, 0x7fff));

		if(o.memop >= 2 && o.mem_table) {
			// 絶対番地の読み出し（上の step と同じ）
			const u32 address = (u32(m_offset[o.offset_index]) + (o.mem_use_index ? m_ram_index : 0) + (o.mem_use_index2 ? ram_index2 : 0) + (o.memop == 3 ? 1 : 0)) & 0x3ffff;
			m_memr_value[d2] = revram_decode(m_swp->m_reverb_ram[address]);
			m_memr_active[d2] = true;
			goto mem_done;
		}
		if(o.memop) {
			// S-MU2000: 区画が無効の間は、書き込みは落ち、読み出しは 0 になる（step() と同じ）
			if(BIT(m_swp->m_revram_enable, o.region)) {
				if(o.memop != 1) {
					m_memr_value[d2] = 0;
					m_memr_active[d2] = true;
				}
				goto mem_done;
			}
			u32 off = u32(m_offset[o.offset_index]) + u32(o.mem_use_index ? m_ram_index : 0) + u32(o.mem_use_index2 ? ram_index2 : 0) - sample_counter;
			if(o.memop == 3)
				off += 1;
			const u32 address = ((off & o.addr_mask) + o.addr_base) & 0x3ffff;
			if(o.memop == 1)
				m_swp->m_reverb_ram[address] = revram_encode(m_ram_write);
			else {
				m_memr_value[d2] = revram_decode(m_swp->m_reverb_ram[address]);
				m_memr_active[d2] = true;
			}
		}
	mem_done:

		d3 = d3 == 2 ? 0 : d3 + 1;
		d2 ^= 1;
	}

	m_p = p;
	m_delay_3 = d3;
	m_delay_2 = d2;
	m_swp->m_meg_flag_n = flag_n;
	m_swp->m_meg_flag_z = flag_z;
	m_pc = 0;
	m_icount -= 0x180;
}

// S-MU2000: 区画ごとの入口と出口を数える（プログラムか地図が変わったとき）。
// 入口は、その区画の中で書くより先に読む m20-m2f（ミキサが毎サンプル書く送り）。
// 出口は、その区画が書く m20-m3f（ミキサへ戻る音と DAC）
void swp30_device::meg_regions_rebuild(bool keep_quiet)
{
	m_meg->build_ops(m_meg_ops.data());
	// 状態を読み戻したあと（プログラムは同じ）は、数えていた静かな長さを引き継ぐ
	for(meg_region &g : m_meg_regions) {
		const u32 quiet = keep_quiet ? g.quiet : 0;
		g = meg_region{};
		g.quiet = quiet;
	}
	u64 written[8] = {};
	for(u32 pc = 0; pc != 0x180; pc++) {
		const meg_state::op &o = m_meg_ops[pc];
		const u32 k = o.region & 7;
		meg_region &g = m_meg_regions[k];
		const bool real = o.alu || o.dm || o.dr || o.memw || o.memop || o.index || o.index2 || o.t_write || o.jump;
		if(!real)
			continue;
		g.used = true;
		auto read_m = [&](u32 x) {
			if(x >= 0x20 && x < 0x30 && !(written[k] & (u64(1) << x)))
				g.in_mask |= 1u << (x - 0x20);
		};
		if(o.alu && (o.mmode == 2 || o.mmode == 3) && o.m2_from_m && o.sm)
			read_m(o.sm);
		if(o.alu && o.asel == 2 && o.sm)
			read_m(o.sm);
		if(o.dm && o.dm_src == 7 && o.sm)
			read_m(o.sm);
		if(o.dm) {
			written[k] |= u64(1) << o.dm;
			if(o.dm >= 0x20)
				g.out_mask |= u64(1) << o.dm;
		}
	}
	// 静まってから空にするまで: 区画の窓（遅延メモリ）の長さに、0.1 秒の余裕
	for(u32 k = 0; k != 8; k++) {
		const u32 size = 1u << (10 + BIT(m_meg->m_map[k], 8, 3));
		m_meg_regions[k].hold = size + 4410;
		if(m_meg_skip_debug && m_meg_regions[k].used)
			std::fprintf(stderr, "meg-region %p %u in %04x out %016llx hold %u\n", (void *)this, k,
			             m_meg_regions[k].in_mask, (unsigned long long)m_meg_regions[k].out_mask, m_meg_regions[k].hold);
	}
}

// 命令表を作り直し、空にしている区画の命令を「何もしない命令」に替える。
// 何もしない命令も、前の命令が遅れて書くもの（3 命令遅れ）は受け取り、自分では何も書かない。
// 分岐で飛び越した命令と同じ扱い（run_program の skip_to）で、JIT もそのまま訳せる
void swp30_device::meg_ops_rebuild()
{
	m_meg->build_ops(m_meg_ops.data());
	m_meg_idle_primed = false;
	if(!m_meg_skip_mask) {
		m_meg_idle_all = false;
		return;
	}
	for(u32 pc = 0; pc != 0x180; pc++) {
		meg_state::op &o = m_meg_ops[pc];
		if(BIT(m_meg_skip_mask, o.region & 7)) {
			// 乱数は全部の区画で 1 本の並びを分け合う。引く回数と位置をそのままにしておけば、
			// 回している区画のディザは飛ばさないときとビット単位で同じになる
			const u8 n = u8(o.dm && (o.dm_src == 5 || (o.dm_src == 6 && !o.no_noise)))
				+ u8(o.dr && !o.dr_from_r && !o.no_noise);
			const u8 region = o.region;
			o = meg_state::op{};
			o.region = region;
			o.rand_n = n;
		}
	}
	// 続けて飛ばす命令の引く回数は、続きの最後の命令にまとめる（種の掛け算が 1 回で済む）。
	// あいだに乱数を引く命令は無いので、あとの命令が受け取る値は同じ
	u32 carry = 0;
	for(u32 pc = 0; pc != 0x180; pc++) {
		meg_state::op &o = m_meg_ops[pc];
		const bool skipped = BIT(m_meg_skip_mask, o.region & 7);
		if(!skipped) {
			if(carry)
				m_meg_ops[pc - 1].rand_n = u16(carry);
			carry = 0;
			continue;
		}
		carry += o.rand_n;
		o.rand_n = 0;
	}
	if(carry)
		m_meg_ops[0x17f].rand_n = u16(carry);
	// 全部の命令が空か（空にした区画のほかに、何かする命令が残っていないか）
	m_meg_idle_all = true;
	m_meg_idle_rand = 0;
	for(const meg_state::op &o : m_meg_ops) {
		if(o.alu || o.dm || o.dr || o.memw || o.memop || o.index || o.index2 || o.t_write || o.jump) {
			m_meg_idle_all = false;
			break;
		}
		m_meg_idle_rand += o.rand_n;
	}
	m_meg_idle_primed = false;
}

void swp30_device::meg_skip_before()
{
	u32 wake = 0;
	for(u32 k = 0; k != 8; k++) {
		if(!BIT(m_meg_skip_mask, k))
			continue;
		for(u32 in = m_meg_regions[k].in_mask; in; in &= in - 1)
			if(m_meg->m_m[0x20 + std::countr_zero(in)]) {
				wake |= 1u << k;
				break;
			}
	}
	if(!wake)
		return;
	m_meg_skip_mask &= ~wake;
	if(m_meg_skip_debug)
		std::fprintf(stderr, "meg-skip %p wake %02x -> %02x at %u\n", (void *)this, wake, m_meg_skip_mask, m_meg->m_sample_counter);
	for(u32 k = 0; k != 8; k++)
		if(BIT(wake, k))
			m_meg_regions[k].quiet = 0;
	meg_ops_rebuild();
	// 訳し直すまでは解釈実行（命令表と同じ結果）。書き込みが落ち着くまで待つのはプログラムを
	// 替えたときと同じ
	meg_jit_invalidate();
	m_meg_jit_wait = 1;
}

void swp30_device::meg_skip_after()
{
	u32 add = 0;
	for(u32 k = 0; k != 8; k++) {
		meg_region &g = m_meg_regions[k];
		if(!g.used || !g.in_mask || BIT(m_meg_skip_mask, k))
			continue;
		bool silent = true;
		for(u32 in = g.in_mask; in && silent; in &= in - 1)
			if(m_meg->m_m[0x20 + std::countr_zero(in)])
				silent = false;
		for(u64 out = g.out_mask; out && silent; out &= out - 1)
			if(m_meg->m_m[std::countr_zero(out)])
				silent = false;
		// 3 命令遅れで、まだ入っていない書き込みも見る（区画の終わりで書いたもの）
		for(u32 d = 0; d != 3 && silent; d++) {
			const u32 x = m_meg->m_mw_reg[d];
			if(x && BIT(g.out_mask, x) && m_meg->m_mw_value[d])
				silent = false;
		}
		if(!silent) {
			g.quiet = 0;
			continue;
		}
		if(++g.quiet >= g.hold)
			add |= 1u << k;
	}
	if(!add)
		return;
	m_meg_skip_mask |= add;
	if(m_meg_skip_debug)
		std::fprintf(stderr, "meg-skip %p quiet %02x -> %02x at %u\n", (void *)this, add, m_meg_skip_mask, m_meg->m_sample_counter);
	meg_ops_rebuild();
	meg_jit_invalidate();
	m_meg_jit_wait = 1;
}

// S-MU2000: execute_run() を run_sample() に置き換えた。
// もとは MAME のスケジューラが m_icount 分だけ回す作りだった。
// ここではホストが「1 サンプルくれ」と呼ぶ形にする。DRC は使わない。
//
// 1 サンプル = sample_step() 1 回 + MEG のプログラム 384 ステップ。
void swp30_device::run_sample(s32 &left, s32 &right)
{
	if(m_meg_program_changed || m_meg_ops_stale) {
		m_meg->decode_program();
		// S-MU2000: プログラムか地図が変わったら、区画を数え直して全部回すところから
		// （状態を読み戻しただけなら、飛ばしている区画は保存に入っているのでそのまま）。
		// 書き換わった命令のある区画だけ戻す。地図が変わったら区画の境目が動くので全部
		if(m_meg_program_changed) {
			u32 dirty = 0;
			if(m_meg_map_dirty)
				dirty = 0xff;
			else
				for(u32 w = 0; w != 6; w++)
					for(u64 b = m_meg_prg_dirty[w]; b; b &= b - 1)
						dirty |= 1u << (m_meg->region_of(u16(w * 64 + std::countr_zero(b))) & 7);
			m_meg_skip_mask &= ~dirty;
			for(u32 k = 0; k != 8; k++)
				if(BIT(dirty, k))
					m_meg_regions[k].quiet = 0;
			m_meg_prg_dirty.fill(0);
			m_meg_map_dirty = false;
		}
		meg_regions_rebuild(true);
		meg_ops_rebuild();
		m_meg_program_changed = false;
		// S-MU2000: 軽量モードの口は、形を見分け直す（dsp/meg_fx.h）
		m_mfx_gen++;
		m_meg_ops_stale = false;
		// S-MU2000: JIT はすぐには作り直さない。firmware はエフェクトを組むとき、プログラムと番地を
		// 何百サンプルにもわたって少しずつ書くので、毎サンプル訳し直すと訳すほうが重くなる。
		// 書き込みが 64 サンプル止まるまでは解釈実行で回す（JIT とビット単位で同じ）
		meg_jit_invalidate();
		m_meg_jit_wait = 1;
	} else if(m_meg_jit_wait && ++m_meg_jit_wait > 64) {
		m_meg_jit_wait = 0;
		meg_jit_rebuild();
	}

	sample_step();
	// S-MU2000: 空にしている区画の入口に音が来たら、このサンプルから元に戻す
	if(m_meg_skip_mask)
		meg_skip_before();
	std::array<s32, 16> meg_in;
	if(m_meg_tap)
		std::copy(m_meg->m_m.begin() + 0x20, m_meg->m_m.begin() + 0x30, meg_in.begin());
	if(m_native && m_native_full) {
		// S-MU2000: 完全な軽量モード。MEG の 384 段は回さない（doc/native-dsp.md）
	} else if(m_meg_idle_primed && !m_dbg_meg) {
		// S-MU2000: 全部の区画が空で、状態はもう動かない。空の命令が引くはずの乱数だけ進める
		if(m_meg_idle_rand)
			rand_skip(m_meg_idle_rand);
		m_meg->m_pc = 0;
		m_meg->m_icount -= 0x180;
	} else if(m_dbg_meg) {
		// S-MU2000: 1 命令ずつ追うときは元の step() で回す
		for(int i = 0; i != 384; i++)
			m_meg->step();
	} else if(m_profile) {
		// S-MU2000: MEG だけの時間を測る（blocktime が使う）
		const auto t0 = std::chrono::steady_clock::now();
		if(!meg_jit_run())
			m_meg->run_program(m_meg_ops.data());
		m_t_meg += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
	} else if(!meg_jit_run())
		// S-MU2000: 機械語にできていれば、そちらで回す（swp30_jit.cpp）
		m_meg->run_program(m_meg_ops.data());
	// S-MU2000: 空の 1 サンプルを回し終えたら、次からは回さない
	if(!(m_native && m_native_full) && !m_dbg_meg)
		m_meg_idle_primed = m_meg_idle_all;
	// S-MU2000: 静まった区画を数える（MEG を回したときだけ）
	if(m_meg_skip_on && !(m_native && m_native_full) && !m_dbg_meg)
		meg_skip_after();
	if(m_meg_tap)
		m_meg_tap(m_meg_tap_ctx, meg_in.data(), &m_meg->m_m[0x20]);

	// sound_stream_update() がやっていたことをここで行う。
	// DAC は出力 0-3 の先頭 2 本。scale は 1<<17。
	left  = m_adc[0];
	right = m_adc[1];

	// S-MU2000: 軽量モードの C++ エフェクトを、ここで足す（doc/native-dsp.md）
	if(m_native) {
		constexpr float SCALE = 131072.0f;      // m_adc の全振幅（0x20000）
		using nfx = smu2000::dsp::native_fx;
		static const nfx::slot_id ID[4] = { nfx::REVERB, nfx::CHORUS, nfx::VARIATION, nfx::INS1 };
		// MEG と同じ作りで鳴らせる口を見分ける（プログラムが変わったとき）。命令の範囲はマスタの
		// firmware の置き方（リバーブ 0x00-0x97、コーラス 0x98-0xbf、インサーション 1 は区画 2、バリエーションは区画 3）
		static const int LO[4] = { 0x000, 0x098, 0x120, 0x0c0 }, HI[4] = { 0x098, 0x0c0, 0x180, 0x120 };
		static const int SEND[4] = { 0x24, 0x26, 0x2c, 0x28 };   // 送りのレジスタ（m_nsend に控えてある）
		if(m_mfx_seen != m_mfx_gen) {
			m_mfx_seen = m_mfx_gen;
			for(int i = 0; i != 4; i++) {
				m_native->mfx(ID[i]).identify(m_meg->m_program.data(), LO[i], HI[i]);
				m_mfx_cfg_gen[i] = ~0u;
				m_mfx_quiet[i] = 0;
				m_mfx_nout[i] = 0;
			}
		}
		float wl = 0.0f, wr = 0.0f;
		for(int i = 0; i != 4; i++) {
			if(!(m_native_mask & (1 << i)))
				continue;
			auto &s = m_native->mfx(ID[i]);
			if(s.active()) {
				// MEG と同じ作り。係数と番地は firmware が MEG に書いた値をそのまま読む
				// （書き換えを拾うため 32 サンプルごと）。戻りは MEG と同じレジスタに書き、
				// 次のサンプルのミキサが戻りのレベルとパンを掛ける
				auto &fx = s.fx();
				// 係数と番地は、firmware が書き換えたときだけ読み直す
				const u32 gen = m_meg_const_gen + (m_meg_off_gen << 16);
				if(m_mfx_cfg_gen[i] != gen) {
					m_mfx_cfg_gen[i] = gen;
					// 窓の長さは地図から（命令表は、静まった区画を空の命令に替えていることがある）
					fx.resize(1u << (10 + BIT(m_meg->m_map[m_meg->region_of(HI[i] - 1)], 8, 3)));
					fx.configure(m_meg->m_const.data(), m_meg->m_offset.data(), LO[i]);
					fx.set_table(m_reverb_ram.size() >= 0x40000 ? m_reverb_ram.data() : nullptr);
					if(s.fresh() && m_reverb_ram.size() >= 0x40000) {
						fx.load_ram(m_reverb_ram.data(), BIT(m_meg->m_map[m_meg->region_of(HI[i] - 1)], 0, 8) << 10, m_meg->m_sample_counter);
						s.mark_loaded();
					}
					m_mfx_hold[i] = (1u << (10 + BIT(m_meg->m_map[m_meg->region_of(HI[i] - 1)], 8, 3))) + 4410;
				}
				// 静まった口は回さない（MEG の区画飛ばしと同じ決まり）。送りが 0 で、戻りが 1 LSB に
				// 満たないまま、遅延の窓の長さ + 0.1 秒たったら、送りが来るまで止める。戻りは 0
				const bool silent_in = m_nsend[i][0] == 0 && m_nsend[i][1] == 0;
				if(!silent_in)
					m_mfx_quiet[i] = 0;
				else if(m_mfx_quiet[i] >= m_mfx_hold[i]) {
					for(int j = 0; j != m_mfx_nout[i]; j++)
						m_mfx_out[i][j] = 0;
					continue;
				}
				float lfo[24];
				for(u32 used = fx.lfo_used(); used; used &= used - 1) {
					const int n = std::countr_zero(used);
					lfo[n] = float(m_meg->get_lfo(n)) * (1.0f / 8388608.0f);
				}
				float in[8], out[8];
				for(int j = 0; j != fx.n_in(); j++) {
					const int reg = fx.in_reg(j);
					const s32 v = reg == SEND[i] ? m_nsend[i][0] : reg == SEND[i] + 1 ? m_nsend[i][1] : m_meg->m_m[reg];
					in[j] = float(v) * (1.0f / 8388608.0f);
				}
				fx.process(in, out, lfo);
				m_mfx_nout[i] = fx.n_out();
				bool silent_out = true;
				for(int j = 0; j != fx.n_out(); j++) {
					m_mfx_reg[i][j] = fx.out_reg(j);
					m_mfx_out[i][j] = s32(std::clamp(out[j], -1.0f, 1.0f) * 8388607.0f);
					silent_out = silent_out && m_mfx_out[i][j] == 0;
				}
				if(silent_in && silent_out)
					m_mfx_quiet[i]++;
				else
					m_mfx_quiet[i] = 0;
				continue;
			}
			m_mfx_nout[i] = 0;
			const float il = float(m_nsend[i][0]) / SCALE, ir = float(m_nsend[i][1]) / SCALE;
			float ol = 0.0f, orr = 0.0f;
			m_native->process(ID[i], il, ir, ol, orr);
			const float g = m_native->ret(ID[i]);
			wl += ol * g;
			wr += orr * g;
		}
		// MEG と同じ作りの口の戻りは、MEG が書くのと同じレジスタへ（MEG を回したあとなので上書きになる）。
		// 区画の最後の 3 命令で書く戻り（バリエーションのリバーブなど）は、MEG ではまだ遅れの輪にいて、
		// 次のサンプルの頭（flush_writes）で入る。送りの無い MEG のその書き込みは取り消す
		for(int i = 0; i != 4; i++)
			if(m_native_mask & (1 << i))
				for(int j = 0; j != m_mfx_nout[i]; j++) {
					const int reg = m_mfx_reg[i][j];
					m_meg->m_m[reg] = m_mfx_out[i][j];
					for(int k = 0; k != 3; k++)
						if(m_meg->m_mw_reg[k] == reg)
							m_meg->m_mw_reg[k] = 0;
				}
		// MEG を通る道で減るぶん（実測で合わせた）。乾いた音も送りも同じ目盛りなので、
		// どちらのモードでもこれを掛ける
		constexpr float DRY_GAIN = 0.1767f;
		if(m_native_full) {
			// MEG を回していないので、乾いた音もここで混ぜてマスター EQ を掛ける。
			// 目盛りは MEG の出口（m30/m31 >> 4）に合わせてある
			// 乾いた音の大きさ。MEG を通ると減るぶんを実測で合わせた
			// （同じ曲を MEG 有り・無しで書き出して rms をそろえた）
			const float dl = float(m_ndry[0]) / SCALE, dr = float(m_ndry[1]) / SCALE;
			float ml = (dl + wl) * DRY_GAIN, mr = (dr + wr) * DRY_GAIN;
			m_native->meq().process(ml, mr, ml, mr);
			left  = std::clamp<s32>(s32(ml * SCALE), -0x20000, 0x1ffff);
			right = std::clamp<s32>(s32(mr * SCALE), -0x20000, 0x1ffff);
			m_adc[0] = left;
			m_adc[1] = right;
		} else {
			left  += s32(wl * SCALE * DRY_GAIN);
			right += s32(wr * SCALE * DRY_GAIN);
		}
	}

	// S-MU2000: 音が出ないときの手掛かり。-v のときだけ最大値を覚える
	if(::smu2000::g_verbose) {
		for(int i=0; i != 4; i++)
			if(std::abs(m_adc[i]) > m_dbg_adc_max) m_dbg_adc_max = std::abs(m_adc[i]);
		for(int i=0; i != 4; i++)
			if(std::abs(m_meg->m_m[0x30+i]) > m_dbg_meg_max) m_dbg_meg_max = std::abs(m_meg->m_m[0x30+i]);
		for(int i=0; i != 16; i++) {
			if(std::abs(m_meg->m_m[0x20+i]) > m_dbg_megin_max) m_dbg_megin_max = std::abs(m_meg->m_m[0x20+i]);
			if(std::abs(m_melo[i]) > m_dbg_melo_max) m_dbg_melo_max = std::abs(m_melo[i]);
		}
	}
}

void swp30_device::adc_step()
{
	for(int i=0; i != 4; i++)
		m_adc[i] = std::clamp<s32>(m_meg->m_m[0x30 + i] >> 4, -0x20000, +0x1ffff);
}

// S-MU2000: MEG の中身をそのまま書き出す。逆アセンブルは tools/meg_dis.py。
// エフェクトが今どんなプログラムで動いているかを見るための窓
void swp30_device::dump_meg(const char *path)
{
	std::FILE *f = std::fopen(path, "w");
	if(!f)
		return;
	for(u32 pc = 0; pc != 0x180; pc++)
		fprintf(f, "prg %03x %016llx %04x\n", pc,
		        (unsigned long long)m_meg->m_program[pc], u16(m_meg->m_const[pc]));
	for(u32 i = 0; i != 0x80; i++)
		fprintf(f, "off %02x %04x\n", i, m_meg->m_offset[i]);
	for(u32 i = 0; i != 0x18; i++)
		fprintf(f, "lfo %02x %04x\n", i, m_meg->m_lfo[i]);
	for(u32 i = 0; i != 8; i++)
		fprintf(f, "map %x %04x\n", i, m_meg->m_map[i]);
	// ミキサの結線も。どのパートがどの母線へ行くかが分かる
	for(u32 i = 0; i != 0x60; i++)
		if(m_mixer[i].route[0] | m_mixer[i].route[1] | m_mixer[i].route[2])
			fprintf(f, "mix %02x route %04x %04x %04x vol %04x %04x %04x\n", i,
			        m_mixer[i].route[0], m_mixer[i].route[1], m_mixer[i].route[2],
			        m_mixer[i].vol[0], m_mixer[i].vol[1], m_mixer[i].vol[2]);
	std::fclose(f);
}

void swp30_device::sample_step()
{
	m_meg->flush_writes();

	// S-MU2000: MEG の m レジスタ 0x20-0x3f を毎サンプル書き出す（--dump-dac）。
	// **混ぜる前**なので、ここに出るのは MEG が 384 段回し終わった直後の姿、
	// つまりエフェクトの出口。次の行の mixer_step が 0x20-0x2f を
	// ミキサの出力（＝エフェクトへの送り）で上書きしてしまうので、
	// 出口を見るにはこの位置でなければならない
	if(m_dbg_dac && m_meg->m_sample_counter >= m_dbg_dac_from &&
	   m_meg->m_sample_counter < m_dbg_dac_from + m_dbg_dac_count)
	{
		fprintf(m_dbg_dac, "%u", m_meg->m_sample_counter);
		for(int i=0; i != 0x40; i++)
			fprintf(m_dbg_dac, " m%02x=%d", i, m_meg->m_m[i]);
		for(int i=0; i != 0x80; i++)
			fprintf(m_dbg_dac, " r%02x=%d", i, m_meg->m_r[i]);
		fprintf(m_dbg_dac, "\n");
	}

	std::array<s32, 0x40> samples_per_chan;
	awm2_step(samples_per_chan);
	// S-MU2000: ミュートした声は、画面にもミックスにも出さない
	if(const u64 mute = m_voice_mute.load(std::memory_order_relaxed))
		for(int i = 0; i < 0x40; i++)
			if((mute >> i) & 1)
				samples_per_chan[i] = 0;
	// S-MU2000: 声ごとの出力を画面へ（パートの音のスペクトラム）
	if(m_voice_tap)
		m_voice_tap(m_voice_tap_ctx, samples_per_chan.data());

	// S-MU2000: 声そのものの出力（--dump-dac のときだけ）。
	// 暴れているのが声なのかエフェクトなのかを分けるため
	if(m_dbg_dac && m_meg->m_sample_counter >= m_dbg_dac_from &&
	   m_meg->m_sample_counter < m_dbg_dac_from + m_dbg_dac_count) {
		fprintf(m_dbg_dac, "awm %u", m_meg->m_sample_counter);
		for(int i=0; i != 0x40; i++)
			if(samples_per_chan[i])
				fprintf(m_dbg_dac, " c%02x=%d", i, samples_per_chan[i]);
		fprintf(m_dbg_dac, "\n");
	}

	if(::smu2000::g_verbose)
		for(int i=0; i != 0x40; i++)
			if(std::abs(samples_per_chan[i]) > m_dbg_awm_max) m_dbg_awm_max = std::abs(samples_per_chan[i]);
	if(::smu2000::g_verbose)
		for(int i=0; i != 0x40; i++)
			if(m_envelope[i].active()) {
				m_dbg_energy[i] += u64(std::abs(samples_per_chan[i]));
				m_dbg_len[i]++;
			}
	adc_step();
	mixer_step(samples_per_chan);
	m_meg->lfo_step();

	// S-MU2000: サンプリングの録音。firmware は録音を始めるとき、スレーブに番地（サンプリング RAM の先頭
	// 0x1000000）と長さ（語数）を書いてから、波形アクセスに 0x7000 を書く。あとは 0x30f（書いた位置の下 16bit）と
	// 0x10f の bit 14（書き終わり）を見続け、止めるときにアクセスを 0 に戻す。
	// 1 サンプルごとに、ミキサの出力 8 の左（m_rec_bus）を 16bit で書く。32bit の語 1 つに 2 サンプル（下の 16bit が先）で、
	// 声が 16bit のサンプルを読むとき（streaming_block::read_16）と同じ並び。
	// 出力 8 に繋がっているのは A/D INPUT の MELI 6（AD1）と 7（AD2）だけで、REC の InputSrc（AD1 / AD2 / AD1+2）は
	// その 2 本の音量（0x5b8/0x5b9、0x5f8/0x5f9 に 0x00ff か 0xffff）を切り替えている（firmware 2.01 の 0x13af14）。
	// MELI は 16bit を 8bit 上げて入れているので、8bit 下げて戻す
	if(m_wave_access == 0x7000 && m_wave_size) {
		const u16 v = u16(std::clamp<s32>(m_rec_bus >> 8, -0x8000, 0x7fff));
		u32 w = m_wave_cache.read_dword(m_wave_adr);
		w = (m_rec_pos & 1) ? ((w & 0x0000ffff) | (u32(v) << 16)) : ((w & 0xffff0000) | v);
		m_wave_cache.write_dword(m_wave_adr, w);
		m_rec_pos++;
		if(!(m_rec_pos & 1)) {
			m_wave_adr++;
			m_wave_size--;
		}
	}

	m_meg->m_sample_counter ++;
}

// S-MU2000: sound_stream_update() は MAME 専用なので削除
// S-MU2000: DEFINE_DEVICE_TYPE は MAME 専用なので削除


// 状態の保存と復元。
//
// 入れないもの
//   m_sintab / m_program_cache / m_wave_cache …… ROM と読み口。変わらない
//   m_buf ……………………………………………… 1 サンプルの間しか持たない置き場
//   m_dbg_* ………………………………………… 調べもの用の数え上げ
void swp30_device::state(state_io &s)
{
	s.tag("swp30");
	m_machine.state_sync(s);
	s.v(m_rand_seed);

	// リバーブ RAM。残響の続きを合わせるのに要る
	{
		u32 n = u32(m_reverb_ram.size());
		s.v(n);
		if (!s.writing())
			m_reverb_ram.resize(n);
		s.mem(m_reverb_ram.data(), m_reverb_ram.size() * sizeof(u16));
	}

	s.stdarr(m_streaming);
	s.stdarr(m_filter);
	s.stdarr(m_iir1);
	s.stdarr(m_envelope);
	s.stdarr(m_lfo);
	s.stdarr(m_mixer);
	m_mix_dirty[0] = m_mix_dirty[1] = ~u64(0);
	s.stdarr(m_melo);
	s.stdarr(m_meli);
	s.stdarr(m_adc);

	s.tag("meg");
	{
		// **meg_state は親デバイスへのポインタを持っている**。まるごと写すと
		// 読み戻した側が「保存した側の機械」を指してしまい、乱数もリバーブ
		// RAM も別の機械のものを触りに行く。写したあとに繋ぎ直す
		swp30_device *keep = m_meg->m_swp;
		if (s.writing())
			m_meg->m_swp = nullptr;   // 保存の中には入れない
		s.v(*m_meg);
		m_meg->m_swp = keep;
	}
	s.v(m_meg_program_changed);
	if (!s.writing())
		m_meg_ops_stale = true;

	s.v(m_sample_counter);
	s.v(m_wave_adr); s.v(m_wave_size); s.v(m_wave_val);
	s.v(m_revram_adr); s.v(m_revram_data);
	s.v(m_wave_access); s.v(m_revram_enable);
	s.v(m_keyon_mask); s.v(m_internal_adr);
	// 版 6 から: MEG の印（分岐と乗算器の第 1 入力の選び方 2 が使う）と 2 つ目の idx（doc/upstream.md の 29・32）
	if(s.version() >= 6) {
		s.v(m_meg_flag_n); s.v(m_meg_flag_z);
		s.stdarr(m_meg_ix2_value); s.stdarr(m_meg_ix2_act); s.v(m_meg_ram_index2);
	} else if(!s.writing()) {
		m_meg_flag_n = m_meg_flag_z = false;
		m_meg_ix2_value.fill(0); m_meg_ix2_act.fill(0); m_meg_ram_index2 = 0;
	}
	// 版 4 から: サンプリングの録音の位置
	if(s.version() >= 4) {
		s.v(m_rec_pos); s.v(m_rec_ctrl);
	} else if(!s.writing()) {
		m_rec_pos = 0; m_rec_ctrl = 0;
	}
	// 版 3 から: ピッチ EG（スロット 0x0B と 0x10、今の値、着いた印）。版 2 の状態には無いので 0 にする
	if(s.version() >= 3) {
		s.stdarr(m_pitch_offset);
		s.stdarr(m_peg_rate);
		s.stdarr(m_peg_cur);
		s.stdarr(m_peg_reached);
	} else if(!s.writing()) {
		m_pitch_offset.fill(0);
		m_peg_rate.fill(0);
		m_peg_cur.fill(0);
		m_peg_reached.fill(0);
	}
	// S-MU2000: 休んでいる声の印は保存しない。読み戻したら全部回すところから（回しても変わらない）
	if(!s.writing())
		m_awm_idle = 0;
	// 版 14 から: MEG の静まった区画（飛ばしている区画と、静かになってからの長さ）。
	// 入れないと、読み戻した側だけ全部の区画を回し、止めていた区画のレジスタがずれる
	if(s.version() >= 14) {
		s.v(m_meg_skip_mask);
		for(meg_region &g : m_meg_regions)
			s.v(g.quiet);
	} else if(!s.writing()) {
		m_meg_skip_mask = 0;
		for(meg_region &g : m_meg_regions)
			g.quiet = 0;
	}
	// 版 15 から: 書き換わった命令と地図（書き換えの途中で保存しても、戻す区画が同じになるように）
	if(s.version() >= 15) {
		s.stdarr(m_meg_prg_dirty);
		s.v(m_meg_map_dirty);
	} else if(!s.writing()) {
		m_meg_prg_dirty.fill(0);
		m_meg_map_dirty = true;
	}
	// 版 16 から: 滑っている途中のフィルタ 1 の切る高さ。前の版は滑らせなかったので、書いてある値に着いていた
	if(s.version() >= 16) {
		s.stdarr(m_filter_cur);
	} else if(!s.writing()) {
		for(int i = 0; i < 0x40; i++)
			m_filter_cur[i] = u32(m_filter[i].m_filter_1_a & 0x7ff) << 16;
	}
	// 版 17 から: 同じくフィルタ 2
	if(s.version() >= 17) {
		s.stdarr(m_filter2_cur);
	} else if(!s.writing()) {
		for(int i = 0; i < 0x40; i++)
			m_filter2_cur[i] = filter_block::filter_2_target(m_filter[i].m_filter_2_a);
	}
}
