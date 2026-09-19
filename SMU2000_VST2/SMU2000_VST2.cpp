#include "SMU2000_VST2.h"

#include <cstddef>
#include <cstdint>

#if defined(CLAP_API) && (defined(__GNUC__) || defined(__clang__))
// MinGW/Clang portability fix (no iPlug2 submodule edit; mirrors ../sw10_plug). GCC/Clang
// reject __attribute__((dllexport)) on a namespace-scope `const` definition even with a
// prior extern declaration (unlike MSVC, which tolerates it). Upstream defines the CLAP
// entry points as `CLAP_EXPORT const clap_plugin_*_t x = {...}` in
// IPlug_include_in_plug_src.h (next include). Locally expand CLAP_EXPORT to
// `extern __attribute__((dllexport))` (external linkage + export) so those definitions
// are valid and exported (clap_entry). All of the CLAP SDK's own CLAP_EXPORT uses were
// already processed through the header chain above (header-guarded), so this stays
// local to the entry points. MSVC path unaffected (guard excludes it); VST2 path
// unaffected (CLAP_API undefined).
#undef  CLAP_EXPORT
#define CLAP_EXPORT extern __attribute__((dllexport))
#endif

#include "IPlug_include_in_plug_src.h"

#if defined(CLAP_API) && (defined(__GNUC__) || defined(__clang__))
// Restore the SDK spelling for anything compiled after this point in this TU.
#undef CLAP_EXPORT
#if defined(_WIN32) || defined(__CYGWIN__)
#define CLAP_EXPORT __attribute__((dllexport))
#else
#define CLAP_EXPORT __attribute__((visibility("default")))
#endif
#endif

static_assert(sizeof(sample) == 4, "target must define SAMPLE_TYPE_FLOAT (engine::fill is float)");

// P7 guard: SMU2000_ENABLE_GUI (GUI-ON build) must survive into PLUG_HAS_UI, or the host
// silently gets no editor (HasUI()==false -> effEditGetRect/effEditOpen are no-ops). If
// this trips, config.h's PLUG_HAS_UI guard is desynced from the per-target define.
#if defined(SMU2000_ENABLE_GUI)
static_assert(PLUG_HAS_UI == 1, "SMU2000_ENABLE_GUI set but PLUG_HAS_UI != 1 (config desync)");
#endif

SMU2000_VST2::SMU2000_VST2(const InstanceInfo& info)
  // Qualified on purpose: CLAP's base clap::helpers::Plugin injects the name
  // "Plugin" into class scope (C2614 for the unqualified alias). Same spelling
  // as upstream Examples/IPlugEffect.cpp; correct for both VST2_API and CLAP_API.
  : iplug::Plugin(info, MakeConfig(kNumParams, kNumPresets))
{
  m_engine = std::make_unique<smu2000::vst3::engine>();
  m_engine->set_output_rate(GetSampleRate());
  // Warm the MIDI queue heap once, off the audio path (see midi_queue.h): dense MIDI
  // must never malloc on the audio thread. clear() keeps capacity, so after the first
  // block this is the steady-state footprint (~128 KB + 64 KB).
  m_midi_q.reserve(8192, 8192 * 8);
  // Boot synchronously here: the host starts its timeline the moment the instance
  // exists and a plug-in cannot pause it, so the old async boot streamed the first
  // 2-5 s of every song out as silence with the song-start MIDI queued behind it
  // (render.exe avoids this by running the boot loop before feeding MIDI from
  // position 0). The constructor runs before audio streaming, so this is where
  // "delay the streaming start until live" has to happen — state() is ready (or
  // failed) when the ctor returns. The midi() queue + fill() silence stay as the
  // safety net (and for the async opt-out: plugin.ini boot=async / SMU2000_SYNC_BOOT=0).
  // A post-boot snapshot (bootcache.bin) makes only the first cold instance pay.
  m_engine->start(true);

#if PLUG_HAS_UI  // native GDI editor (SMU2000_ENABLE_GUI=ON); untouched when OFF. Attaches to
                 // the host HWND on OpenWindow (effEditOpen / guiSetParent). No IGraphics.
  m_editor = std::make_unique<smu2000::editor>(*m_engine);
  SetEditorSize(m_editor->width(), m_editor->height());
#endif
}

void SMU2000_VST2::OnReset()
{
  m_engine->set_output_rate(GetSampleRate());
  m_engine->set_processing(true);
}

void SMU2000_VST2::ProcessBlock(sample** inputs, sample** outputs, int nFrames)
{
  (void) inputs;  // PLUG_CHANNEL_IO "0-2": no audio inputs

  // No MIDI this block: render it in one call (the common, lowest-overhead case).
  if (m_midi_q.empty())
  {
    m_engine->fill(outputs[0], outputs[1], nFrames, nullptr, nullptr);
    return;
  }

  // Fixed-window drain (see midi_queue.h): the queue is chronologically sorted by
  // construction (queue::push), so there is NO sorting here at all. Events inside a
  // 32-sample window go onto the serial line at window start (isolated events stay
  // exactly on-sample); engine->fill() runs at most ceil(nFrames/32) times per block
  // whatever the event density — each call is an m_machine lock round-trip, which is
  // what the old per-distinct-offset slicing multiplied into a mutex storm on dense
  // MIDI. fill() releases m_machine on return, so midi() between fills stays lock-safe.
  smu2000::midi::drain_fixed_window(m_midi_q, *m_engine, outputs[0], outputs[1], nFrames);
}

void SMU2000_VST2::ProcessMidiMsg(const IMidiMsg& msg)
{
  const int nibble = msg.mStatus & 0xF0;
  const int n = (nibble == 0xC0 || nibble == 0xD0) ? 2 : 3;  // ProgramChange/ChannelAT are 2 bytes
  const uint8_t bytes[3] = {msg.mStatus, msg.mData1, msg.mData2};
  // offset = sample offset into the coming ProcessBlock(); port 0 = parts 1-16, channel
  // lives in the status byte. Hot path is a plain append for time-sorted hosts (the
  // VST2/CLAP delivery contract); only actual disorder pays the insert-at-tail memmove.
  m_midi_q.push(msg.mOffset, 0, bytes, (uint32_t) n);
}

void SMU2000_VST2::ProcessSysEx(const ISysEx& msg)
{
  m_midi_q.push(msg.mOffset, 0, reinterpret_cast<const uint8_t*>(msg.mData), (uint32_t) msg.mSize);
}

bool SMU2000_VST2::SerializeState(IByteChunk& chunk) const
{
  const std::vector<uint8_t> blob = m_engine->save_state();
  chunk.PutBytes(blob.data(), (int) blob.size());
  return true;
}

int SMU2000_VST2::UnserializeState(const IByteChunk& chunk, int startPos)
{
  const int n = chunk.Size() - startPos;
  if (n > 0)
  {
    std::vector<uint8_t> blob(n);
    chunk.GetBytes(blob.data(), n, startPos);
    m_engine->load_state(blob.data(), (size_t) n);
  }
  return chunk.Size();
}



