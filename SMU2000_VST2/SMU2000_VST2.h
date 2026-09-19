#pragma once

#include "IPlug_include_in_plug_hdr.h"

#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

#include "vst3/engine.h"
#include "midi_queue.h"

#if PLUG_HAS_UI  // GUI-ON only (SMU2000_ENABLE_GUI). Native GDI editor, NO IGraphics.
#include "ui/SMU2000Editor.h"
#endif

const int kNumPresets = 1;
const int kNumParams = 0;  // MIDI-driven instrument, no host params (ledger P2)

using namespace iplug;

class SMU2000_VST2 final : public Plugin
{
public:
  SMU2000_VST2(const InstanceInfo& info);

  void ProcessBlock(sample** inputs, sample** outputs, int nFrames) override;
  void ProcessMidiMsg(const IMidiMsg& msg) override;
  void ProcessSysEx(const ISysEx& msg) override;
  void OnReset() override;

  bool SerializeState(IByteChunk& chunk) const override;
  int UnserializeState(const IByteChunk& chunk, int startPos) override;

#if PLUG_HAS_UI  // native (non-IGraphics) editor: hosts drive these via effEditOpen/Close (VST2)
                 // and guiSetParent/guiDestroy (CLAP). See ui/SMU2000Editor.h.
  void* OpenWindow(void* pParent) override { return m_editor ? m_editor->open(pParent) : nullptr; }
  void  CloseWindow() override { if (m_editor) m_editor->close(); }
  void  OnParentWindowResize(int width, int height) override
  {
    if (m_editor) m_editor->set_size(width, height);
  }
#endif

private:
  // Incoming MIDI is stamped with a sample offset (IMidiMsg::mOffset / ISysEx::mOffset),
  // but smu2000::engine::midi() has no time argument — it just queues bytes onto the
  // emulated 31250 bps serial line. So events are parked here and interleaved into
  // ProcessBlock() at their offset; injecting everything at block start is what made note
  // timing jitter by up to one block (worse at large buffer sizes). Audio-thread only:
  // iPlug2 calls ProcessMidiMsg() just before ProcessBlock() on the same thread.
  //
  // smu2000::midi::queue (midi_queue.h) = iplug::MidiSynth's pattern adapted to this
  // engine: ordering is maintained AT INSERTION (IMidiQueueBase::Add's insert-at-tail —
  // a plain append for time-sorted hosts, which is the VST2 effProcessEvents / CLAP
  // contract), so ProcessBlock NEVER sorts; it just consumes in fixed 32-sample windows
  // (MidiSynth::kDefaultBlockSize), capping engine::fill()/m_machine-mutex calls at
  // nFrames/32 per block regardless of event density (jumping whole event-free gaps).
  // See midi_queue.h for the full rationale and the sparse-exact / dense-early semantics.
  std::unique_ptr<smu2000::vst3::engine> m_engine;
  smu2000::midi::queue                   m_midi_q;
#if PLUG_HAS_UI
  std::unique_ptr<smu2000::editor> m_editor;
#endif
};
