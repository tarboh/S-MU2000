#pragma once

// SMU2000_VST2 MIDI input queue — shared between the plugin (ProcessMidiMsg/
// ProcessSysEx -> ProcessBlock) and tools/midi_bench.cpp, so the benchmark
// exercises the exact shipped code path.
//
// Design = iplug::MidiSynth's pattern, adapted to the serial-line engine:
//
//   * Ordering is maintained AT INSERTION (queue::push), mirroring
//     IMidiQueueBase::Add (iPlug2/IPlug/IPlugMidi.h): scan back from the tail
//     and memmove only when the new offset is out of order. Hosts deliver
//     events time-sorted (VST2 effProcessEvents contract; CLAP events.h:344
//     "The host will deliver these sorted in sample order"), so the hot path
//     degenerates to a plain append — one load + one branch. ProcessBlock
//     therefore pays NO sort and even NO sortedness scan on normal hosts;
//     only a pathologically disorderly stream trips kDisorderLimit, and then
//     the next drain's queue::heal() does exactly ONE std::sort — the OLD
//     plugin's worst case, never worse.
//
//   * Consumption is in FIXED WINDOWS (drain_fixed_window), mirroring
//     MidiSynth::ProcessBlock's kDefaultBlockSize (= 32) sub-blocks: clock
//     every event whose offset falls inside the window onto the serial line,
//     then fill() the window once. engine::fill() takes the m_machine mutex
//     per call (src/vst3/engine.cpp), so the previous per-event slicing made
//     the number of mutex round-trips scale with event density; windowing
//     caps it at nFrames/32 per block regardless of density.
//     Timing cost: an event fires at up to (window-1) samples early. That is
//     within the engine's own granularity — the emulated 31250 bps line
//     already smears a 3-byte message over ~42 samples @44.1k (44100/3125).
//
//   * Storage is POD + append-only byte arena (unchanged from the original
//     perf fix): push_back and the rare disorder memmove are raw copies, and
//     reserve() in the ctor makes the steady state allocation-free. clear()
//     keeps capacity; iPlug2 drains every offset of the coming block per
//     ProcessBlock, so no ring/spill logic is needed.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace smu2000 {
namespace midi {

// Consumption window in samples — same value as iplug::MidiSynth::kDefaultBlockSize.
constexpr int kWindow = 32;

// Per-block insert-shift budget. Past this the host is not merely out of
// order, it is adversarial (reverse-sorted delivery) and insert-at-tail would
// be O(n^2); push() then degrades to plain append and the next drain pays one
// std::sort to restore the invariant — i.e. worst case is exactly the OLD
// plugin's per-block sort, never worse.
constexpr uint32_t kDisorderLimit = 4096;

struct event  // 16 bytes, trivially copyable
{
  int      offset;  // sample offset within the coming ProcessBlock()
  uint32_t pos;     // start of this event's bytes in the arena
  uint32_t len;
  uint8_t  port;    // engine port (0 = parts 1-16; channel is in the status byte)
};
static_assert(std::is_trivially_copyable<event>::value, "memmove-friendly queue");

class queue
{
public:
  void reserve(std::size_t events, std::size_t bytes)
  {
    q.reserve(events);
    arena.reserve(bytes);
  }

  bool  empty() const { return q.empty(); }
  std::size_t size() const { return q.size(); }
  const event& at(std::size_t i) const { return q[i]; }
  const uint8_t* bytes(const event& ev) const { return arena.data() + ev.pos; }
  void clear() { q.clear(); arena.clear(); disorder = 0; chaotic = false; }

  // One-shot repair after push() went into chaotic append mode (see
  // kDisorderLimit). Restores the chronological invariant drain relies on,
  // with the pos tie-break keeping equal-offset arrival order.
  void heal()
  {
    if (chaotic)
    {
      std::sort(q.begin(), q.end(), [](const event& a, const event& b)
                { return a.offset != b.offset ? a.offset < b.offset : a.pos < b.pos; });
      chaotic = false;
      disorder = 0;
    }
  }

  // Appends an event, keeping `q` sorted by offset. Equal offsets keep arrival
  // order (the scan stops at `>` ), reproducing the old stable_sort semantics;
  // arena order is arrival order by construction.
  void push(int offset, int port, const uint8_t* b, uint32_t n)
  {
    const uint32_t pos = (uint32_t) arena.size();
    if (n)
      arena.insert(arena.end(), b, b + n);

    if (!chaotic && !q.empty() && offset < q.back().offset)
    {
      // Host disorder (rare): insert-at-tail, IMidiQueueBase::Add's trick.
      event ev{offset, pos, n, (uint8_t) port};
      q.push_back(ev);
      std::size_t i = q.size() - 1;
      while (i > 0 && q[i - 1].offset > offset) { q[i] = q[i - 1]; --i; }
      q[i] = ev;
      disorder += (uint32_t) (q.size() - 1 - i);
      if (disorder > kDisorderLimit) chaotic = true;  // next drain heals
    }
    else
    {
      q.push_back(event{offset, pos, n, (uint8_t) port});
    }
  }

  // Public so the benchmark's legacy A/B path can replay the queue.
  std::vector<event>   q;
  std::vector<uint8_t> arena;
  uint32_t disorder = 0;
  bool     chaotic  = false;
};

// Fixed-window consumption (the shipped path). Per iteration the render
// window ends at produced+kWindow, EXCEPT:
//   * the next pending event is further away — the window jumps straight to
//     it and the event is then clocked exactly on-sample (so sparse MIDI pays
//     one big fill between clusters and loses zero timing accuracy);
//   * past-the-block offsets (>= nFrames, the old code's clamped tail) do not
//     drive mid-block windowing; they are clocked after the last render.
// Dense clusters inside one window are all clocked at the window start:
// ≤kWindow-1 samples early, which is MidiSynth's accepted model and below
// the serial line's own byte spacing x2.
// Engine duck-type: midi(const uint8_t*, size_t, int) / fill(float*, float*, int, ...).
template <class Engine>
inline void drain_fixed_window(queue& mq, Engine& eng, float* left, float* right, int nFrames)
{
  mq.heal();  // no-op unless push() hit the disorder budget (then: one std::sort)
  std::size_t i = 0;
  const std::size_t n = mq.size();
  int produced = 0;

  while (produced < nFrames && i < n)
  {
    int winEnd = produced + kWindow;
    if (winEnd > nFrames) winEnd = nFrames;

    const int next = mq.at(i).offset;
    if (next > produced && next > winEnd)
      winEnd = next < nFrames ? next : nFrames;  // event-free gap: one big fill

    // Everything scheduled inside this window goes onto the line before the
    // window renders. The 31250 bps model then spaces the bytes.
    while (i < n && mq.at(i).offset < winEnd)
    {
      const event& ev = mq.at(i);
      eng.midi(mq.bytes(ev), ev.len, ev.port);
      ++i;
    }

    // Only past-the-block events (or none) remain: render the tail in one
    // call and stop windowing.
    if (i == n || mq.at(i).offset >= nFrames)
      break;

    eng.fill(left + produced, right + produced, winEnd - produced, nullptr, nullptr);
    produced = winEnd;
  }

  if (produced < nFrames)
    eng.fill(left + produced, right + produced, nFrames - produced, nullptr, nullptr);

  // Past-the-block tail (old code clamped these to the block end).
  while (i < n)
  {
    const event& ev = mq.at(i);
    eng.midi(mq.bytes(ev), ev.len, ev.port);
    ++i;
  }

  mq.clear();
}

} // namespace midi
} // namespace smu2000
