// midi_bench.cpp — A/B benchmark for the SMU2000_VST2 MIDI input path.
//
// Feeds synthetic dense/sparse MIDI through the SHIPPED queue code
// (SMU2000_VST2/midi_queue.h) against the real smu2000::vst3::engine, and
// against a byte-for-byte replica of the pre-rewrite plugin path (append +
// per-block sortedness scan + std::sort fallback + per-distinct-offset fill
// slicing). Both share one engine instance; the delta is pure MIDI plumbing
// (queue insert, sort, number of engine::fill/m_machine round-trips).
//
//   build32-msvc\midibench.exe [roms_dir]      (default: .\roms)
//
// S-MU2000_ROMS is set from the arg before boot, so no install needed.

#include "vst3/engine.h"
#include "../SMU2000_VST2/midi_queue.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

uint64_t rng_state = 0x2545F4914F6CDD1Dull;
uint32_t rnd(uint32_t mod)  // xorshift64*
{
  rng_state ^= rng_state >> 12; rng_state ^= rng_state << 25; rng_state ^= rng_state >> 27;
  return (uint32_t)((rng_state * 0x2545F4914F6CDD1Dull) >> 33) % mod;
}

#ifdef _WIN32
double now_s()
{
  static LARGE_INTEGER f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return q; }();
  LARGE_INTEGER t; QueryPerformanceCounter(&t);
  return (double) t.QuadPart / (double) f.QuadPart;
}
#else
double now_s();
#endif

// Counts engine calls so the fill()/mutex storm is visible, not just wall time.
template <class Engine>
struct counting_engine
{
  Engine* e;
  long fills = 0, midis = 0, samples = 0;
  void midi(const uint8_t* b, size_t n, int port) { e->midi(b, n, port); ++midis; }
  void fill(float* l, float* r, int n, const float* il, const float* ir)
  {
    e->fill(l, r, n, il, ir); ++fills; samples += n;
  }
};

// ---- replica of the OLD plugin path (pre-rewrite SMU2000_VST2.cpp) ----

void push_append(smu2000::midi::queue& mq, int offset, int port, const uint8_t* b, uint32_t n)
{
  const uint32_t pos = (uint32_t) mq.arena.size();
  if (n) mq.arena.insert(mq.arena.end(), b, b + n);
  mq.q.push_back(smu2000::midi::event{offset, pos, n, (uint8_t) port});
}

template <class Engine>
void drain_legacy(smu2000::midi::queue& mq, Engine& eng, float* left, float* right, int nFrames)
{
  if (mq.empty()) { eng.fill(left, right, nFrames, nullptr, nullptr); return; }

  bool sorted = true;
  for (size_t i = 1; i < mq.size(); ++i)
    if (mq.at(i).offset < mq.at(i - 1).offset) { sorted = false; break; }
  if (!sorted)
    std::sort(mq.q.begin(), mq.q.end(),
              [](const smu2000::midi::event& a, const smu2000::midi::event& b)
              { return a.offset != b.offset ? a.offset < b.offset : a.pos < b.pos; });

  int produced = 0;
  size_t i = 0;
  while (i < mq.size())
  {
    int off = mq.at(i).offset;
    if (off < 0) off = 0;
    if (off > nFrames) off = nFrames;
    if (off > produced)
    {
      eng.fill(left + produced, right + produced, off - produced, nullptr, nullptr);
      produced = off;
    }
    while (i < mq.size() && mq.at(i).offset <= off)
    {
      const auto& ev = mq.at(i);
      eng.midi(mq.bytes(ev), ev.len, ev.port);
      ++i;
    }
  }
  if (produced < nFrames)
    eng.fill(left + produced, right + produced, nFrames - produced, nullptr, nullptr);
  mq.clear();
}

// ---- event streams ----

struct block_events
{
  std::vector<int> offsets;          // non-decreasing when !shuffled
  std::vector<std::array<uint8_t, 3>> msgs;
};

block_events make_block(int nFrames, double density, int& ch, int shuffle)
{
  block_events b;
  for (int s = 0; s < nFrames; ++s)
  {
    if (rnd(1000) >= (unsigned)(density * 1000)) continue;
    const int channel = ch++ & 15;
    const uint8_t note = (uint8_t) rnd(96);
    if (rnd(2))
      b.msgs.push_back({(uint8_t) (0x90 | channel), note, (uint8_t) (40 + rnd(80))});
    else
      b.msgs.push_back({(uint8_t) (0x80 | channel), note, 0});
    b.offsets.push_back(s);
    if (rnd(10) == 0)  // CC filler (mixer/pitch streams are dense-offset MIDI too)
    {
      b.msgs.push_back({(uint8_t) (0xB0 | channel), 7, (uint8_t) rnd(128)});
      b.offsets.push_back(s);
    }
  }
    if (shuffle == 2 && b.offsets.size() > 1)  // adversarial: fully reverse-ordered delivery
      for (size_t i = 0, j = b.offsets.size() - 1; i < j; ++i, --j)
        std::swap(b.offsets[i], b.offsets[j]);
    else if (shuffle == 1 && b.offsets.size() > 1)
    {
    // shuffle offsets, keep msgs in arrival slots: arrival order is now scrambled
    std::vector<size_t> idx(b.offsets.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    for (size_t i = idx.size() - 1; i > 0; --i) std::swap(idx[i], idx[rnd((uint32_t) i)]);
    std::vector<int> off2(b.offsets.size());
    for (size_t i = 0; i < idx.size(); ++i) off2[i] = b.offsets[idx[i]];
    b.offsets = std::move(off2);
  }
  return b;
}

// ---- scenarios ----

constexpr int kBlock = 512;

// No-op engine: isolates pure queue plumbing (insert, scan/sort, window walk,
// per-call loop overhead) from the ~10 ms/block of emulator DSP.
struct stub_engine
{
  long fills = 0, midis = 0;
  void midi(const uint8_t*, size_t, int) { ++midis; }
  void fill(float*, float*, int, const float*, const float*) { ++fills; }
};

// A/B the same block deterministically through both strategies, interleaved
// so emulator voice load evolves together; min-of-rounds filters system noise.
struct ab_result
{
  double a = 1e30, b = 1e30;      // best (fastest) full-sequence seconds
  long fills_a = 0, fills_b = 0, events = 0;  // per-block avg from last round
};

template <class PushA, class DrainA, class PushB, class DrainB>
ab_result run_ab(smu2000::vst3::engine& eng, smu2000::midi::queue& mq, int blocks, int rounds,
                 double density, int shuffle, PushA pushA, DrainA drainA, PushB pushB,
                 DrainB drainB)
{
  counting_engine<smu2000::vst3::engine> ceA{&eng, 0, 0, 0};
  counting_engine<smu2000::vst3::engine> ceB{&eng, 0, 0, 0};
  static float left[kBlock], right[kBlock];
  ab_result out;
  uint64_t base = rng_state;

  for (int round = 0; round < rounds; ++round)
  {
    double ta = 0, tb = 0;
    rng_state = base + 0x9E3779B97F4A7C15ull * (uint64_t) round;  // same stream both sides
    int ch = round * 7;
    long ev = 0, fa = 0, fb = 0;
    for (int blk = 0; blk < blocks; ++blk)
    {
      block_events be = make_block(kBlock, density, ch, shuffle);
      const uint64_t saved = rng_state;
      ev += (long) be.offsets.size();

      double t0 = now_s();
      for (size_t k = 0; k < be.offsets.size(); ++k)
        pushA(mq, be.offsets[k], 0, be.msgs[k].data(), 3);
      drainA(mq, ceA, left, right, kBlock);
      ta += now_s() - t0;
      fa += ceA.fills; ceA.fills = 0;

      rng_state = saved;
      block_events be2 = make_block(kBlock, density, ch, shuffle);

      t0 = now_s();
      for (size_t k = 0; k < be2.offsets.size(); ++k)
        pushB(mq, be2.offsets[k], 0, be2.msgs[k].data(), 3);
      drainB(mq, ceB, left, right, kBlock);
      tb += now_s() - t0;
      fb += ceB.fills; ceB.fills = 0;
    }
    out.a = std::min(out.a, ta);
    out.b = std::min(out.b, tb);
    out.fills_a = fa / blocks;
    out.fills_b = fb / blocks;
    out.events = ev / blocks;
  }
  return out;
}

// Same event stream, no DSP: what does the plumbing itself cost?
template <class PushFn, class DrainFn>
void run_plumbing(smu2000::midi::queue& mq, int blocks, int rounds, double density, int shuffle,
                  PushFn push, DrainFn drain, double& best, long& fills_pb)
{
  static float left[kBlock], right[kBlock];
  uint64_t base = rng_state;
  best = 1e30;
  for (int round = 0; round < rounds; ++round)
  {
    stub_engine st;
    rng_state = base + 0x9E3779B97F4A7C15ull * (uint64_t) round;
    int ch = round * 7;
    const double t0 = now_s();
    for (int blk = 0; blk < blocks; ++blk)
    {
      block_events be = make_block(kBlock, density, ch, shuffle);
      for (size_t k = 0; k < be.offsets.size(); ++k)
        push(mq, be.offsets[k], 0, be.msgs[k].data(), 3);
      drain(mq, st, left, right, kBlock);
    }
    const double dt = now_s() - t0;
    if (dt < best) { best = dt; fills_pb = st.fills; }
  }
}

} // namespace

int main(int argc, char** argv)
{
  std::string roms = argc > 1 ? argv[1] : "roms";
#ifdef _WIN32
  _putenv_s("S_MU2000_ROMS", roms.c_str());
#endif

  smu2000::vst3::engine eng;
  eng.set_output_rate(44100.0);
  eng.start(true);
  if (eng.state() != smu2000::vst3::status::ready)
  {
    std::fprintf(stderr, "engine failed to boot: %s\n", eng.message().c_str());
    return 1;
  }
  eng.set_processing(true);

  smu2000::midi::queue mq;
  mq.reserve(8192, 8192 * 8);

  const int kBlocks = 240;  // ~2.8 s of audio per scenario
  const int kRounds = 3;    // min-of-3: engine DSP (~10 ms/blk) swamps plumbing deltas,
                            // and sequential runs drift ~6%; interleave + min tames both.
  struct scen { const char* name; double density; int shuffle; };
  const scen scenaria[] = {
    {"dense (sorted)",     0.50, 0},
    {"dense (shuffled)",   0.50, 1},
    {"dense (reversed)",   0.50, 2},
    {"sparse",             0.01, 0},
  };

  auto push_new = [](smu2000::midi::queue& q, int off, int port, const uint8_t* p, uint32_t n)
                  { q.push(off, port, p, n); };
  auto drain_new = [](smu2000::midi::queue& q, auto& c, float* l, float* r, int n)
                   { smu2000::midi::drain_fixed_window(q, c, l, r, n); };
  auto drain_old = [](smu2000::midi::queue& q, auto& c, float* l, float* r, int n)
                   { drain_legacy(q, c, l, r, n); };

  std::printf("midi_bench: block=%d samples, %d blocks x %d rounds/scenario (min shown)\n",
              kBlock, kBlocks, kRounds);
  std::printf("ENGINE = emulator+MIDI plumbing (A/B interleaved, same event stream)\n");
  std::printf("PLUMBING = MIDI path only, DSP stubbed out\n");

  for (const auto& s : scenaria)
  {
    std::printf("%s:\n", s.name);
    auto r = run_ab(eng, mq, kBlocks, kRounds, s.density, s.shuffle,
                    push_append, drain_old, push_new, drain_new);
    std::printf("  ENGINE   OLD %7.2f ms/blk (fills %4.1f)  NEW %7.2f ms/blk (fills %4.1f)  ev/blk %4.1f | %.3f%%\n",
                r.a * 1000.0 / kBlocks, (double) r.fills_a,
                r.b * 1000.0 / kBlocks, (double) r.fills_b, (double) r.events,
                (r.a - r.b) * 100.0 / r.a);

    double pa, pb; long fpa, fpb;
    run_plumbing(mq, kBlocks, kRounds, s.density, s.shuffle,
                 push_append, drain_old, pa, fpa);
    run_plumbing(mq, kBlocks, kRounds, s.density, s.shuffle,
                 push_new, drain_new, pb, fpb);
    std::printf("  PLUMBING OLD %7.1f us/blk (fills %4.1f)  NEW %7.1f us/blk (fills %4.1f)  | %.2fx\n",
                pa * 1e6 / kBlocks, (double) fpa / kBlocks,
                pb * 1e6 / kBlocks, (double) fpb / kBlocks, pa / pb);
    std::fflush(stdout);
  }
  return 0;
}
