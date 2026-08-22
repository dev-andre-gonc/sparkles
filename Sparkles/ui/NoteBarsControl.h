#pragma once

#include "IControl.h"
#include "ISender.h"
#include "IPlugStructs.h"
#include "Palette.h"
#include "../params/ParamRanges.h"
#include <array>
#include <cstdio>

using namespace iplug;
using namespace igraphics;

// Display-only strip of per-detection-note confidence bars along the bottom of the UI, fed from
// the audio thread via ISender<kNumTriggerableNotes> (see mNoteBarsSender in Sparkles.h). One bar
// per note of the full triggerable span (params/ParamRanges.h), lowest note leftmost, starting
// flush at the plugin's left edge; notes outside the currently configured detect range simply
// read zero confidence and sit at the resting height.
//
// This control's bounds are the FULL plugin bounds: bars grow upward from the bottom edge, from
// kMinBarFrac to kMaxBarFrac of the plugin height as confidence goes 0 -> 1. It's attached first
// (right after the panel background) so everything else draws over it, and it ignores mouse so
// it never steals interaction from the controls above it.
class NoteBarsControl : public IControl
{
public:
  static constexpr int kNumBars = sparkle_params::kNumTriggerableNotes;
  static constexpr float kMinBarFrac = 0.1f; // bar height at zero confidence, fraction of plugin height
  static constexpr float kMaxBarFrac = 0.9f; // bar height at full confidence

  NoteBarsControl(const IRECT& bounds)
  : IControl(bounds)
  {
    SetIgnoreMouse(true);
  }

  void Draw(IGraphics& g) override
  {
    // Very low alpha and a neutral hue lifted from the background artwork's own linework, so the
    // strip reads as an almost-subliminal texture along the bottom edge rather than a distinct
    // control -- "very discreet" per the redesign brief -- while confident notes still visibly
    // rise above the resting stub. Black-key notes get a touch more alpha than white-key ones
    // (rather than a different hue) so the keyboard layout is still faintly legible up close.
    const IColor whiteKeyColor = sparkle_palette::kLinesInterior.WithOpacity(0.10f);
    const IColor blackKeyColor = sparkle_palette::kLinesInterior.WithOpacity(0.18f);
    const IText labelText(9.f, sparkle_palette::kLinesInterior.WithOpacity(0.35f), sparkle_palette::kFontFredokaRegular, EAlign::Center, EVAlign::Bottom);
    const float barWidth = mRECT.W() / static_cast<float>(kNumBars);

    for (int i = 0; i < kNumBars; i++)
    {
      const float conf = mConfidences[i];
      const float heightFrac = kMinBarFrac + (kMaxBarFrac - kMinBarFrac) * conf;
      const float left = mRECT.L + barWidth * static_cast<float>(i);
      const IRECT bar(left, mRECT.B - mRECT.H() * heightFrac, left + barWidth, mRECT.B);

      const int note = sparkle_params::kMinTriggerableNote + i;
      const int pitchClass = note % 12; // 0 = C, since MIDI note 0 is a C
      const bool isBlackKey =
        pitchClass == 1 || pitchClass == 3 || pitchClass == 6 || pitchClass == 8 || pitchClass == 10;

      // Half-pixel inset keeps adjacent bars visually separate.
      g.FillRect(isBlackKey ? blackKeyColor : whiteKeyColor, bar.GetPadded(-0.5f));

      // Octave anchors: label just the Cs, over the bottom of the bar (the resting stub always
      // covers this zone, so the label always sits on a bar, never on bare background).
      if (pitchClass == 0)
      {
        char label[8];
        std::snprintf(label, sizeof(label), "C%d", note / 12 - 1);
        const IRECT labelRect(left, mRECT.B - 16.f, left + barWidth, mRECT.B - 3.f);
        g.DrawText(labelText, label, labelRect);
      }
    }
  }

  void OnMsgFromDelegate(int msgTag, int dataSize, const void* pData) override
  {
    if (msgTag == ISender<>::kUpdateMessage)
    {
      IByteStream stream(pData, dataSize);
      ISenderData<kNumBars> d;
      stream.Get(&d, 0);

      mConfidences = d.vals;
      SetDirty(false);
    }
  }

private:
  std::array<float, kNumBars> mConfidences{};
};
