#pragma once

#include "IControl.h"
#include "IPlugParameter.h"

#include <algorithm>
#include <array>
#include <cmath>

using namespace iplug;
using namespace igraphics;

// Knob for the four tempo-synced magnitude params (Pre Delay, Duration, Ray Delay, Delay --
// docs/SPEC.md §7.2/§7.4) that pairs with a sibling `*Unit` enum param (Beats=0, ms=1, see
// params/ParamList.h). The magnitude IParam itself always stores a plain continuous number --
// beats when the sibling unit is Beats, milliseconds when it's ms -- exactly as
// params/ParamSnapshot.h's ReadTimeParam already assumes; this control only changes how dragging
// it *feels* and what it displays, never what gets stored, so ray/sparkle multiplier math
// downstream (which reads this same continuous value) is never rounded off.
//
// In Beats mode, drag/scroll snaps exactly onto one of kNoteValues below (named divisions from a
// 128th note up to a whole note, each straight/dotted/triplet) and displays its name. In ms mode,
// it behaves like a plain continuous knob over the param's own (curved) range and displays raw
// milliseconds. Which mode is active is re-read from the sibling unit param on every interaction/
// draw, rather than cached, so flipping the *Unit dropdown updates this control's feel immediately
// without needing to be re-attached.
class TimeMagnitudeControl : public IControl
{
public:
  struct NoteValue
  {
    const char* name;
    double beats;
  };

  // 10 divisions (128th .. 4x whole note) x {triplet, straight, dotted}, sorted ascending,
  // dropping the one variant that would exceed the 16-beat ceiling (4-whole-dotted = 24 beats),
  // plus a leading "None" entry so Pre Delay's default of exactly 0 is reachable by scrolling/
  // dragging down.
  static constexpr std::array<NoteValue, 34> kNoteValues = { {
    { "None", 0.0 },
    { "1/128", 1.0 / 32.0 },
    { "1/128T", 1.0 / 32.0 * 1.0 / 3.0 },
    { "1/128.", 1.5 / 32.0 },
    { "1/64", 1.0 / 16.0 },
    { "1/64T", 1.0 / 3.0 * 1.0 / 16.0 },
    { "1/64.", 1.5 * 1.0 / 16.0 },
    { "1/32", 1.0 / 8.0 },
    { "1/32T", 1.0 / 3.0 * 1.0 / 8.0 },
    { "1/32.", 1.5 * 1.0 / 8.0 },
    { "1/16", 1.0 / 4.0 },
    { "1/16T", 1.0 / 3.0 * 1.0 / 4.0 },
    { "1/16.", 1.5 * 1.0 / 4.0 },
    { "1/8", 1.0 / 2.0 },
    { "1/8T", 1.0 / 3.0 * 1.0 / 2.0 },
    { "1/8.", 1.5 * 1.0 / 2.0 },
    { "1/4", 1.0 },
    { "1/4T", 1.0 / 3.0 * 1.0 },
    { "1/4.", 1.5 * 1.0 },
    { "1/2", 2.0},
    { "1/2T", 1.0 / 3.0 * 2.0},
    { "1/2.", 1.5 * 2.0},
    { "1", 4.0 },
    { "1T", 1.0 / 3.0 * 4.0 },
    { "1.", 1.5 * 4.0 },
    { "2", 8.0 },
    { "2T", 1.0 / 3.0 * 8.0 },
    { "2.", 1.5 * 8.0 },
    { "3", 16.0 },
    { "3T", 1.0 / 3.0 * 16.0 },
    { "3.", 1.5 * 16.0 },
    { "4", 32.0 },
    { "4T", 1.0 / 3.0 * 32.0 },
    { "4.", 1.5 * 32.0 },
  } };

  // Pixels of drag (or one wheel notch) per step through kNoteValues in Beats mode.
  static constexpr float kPixelsPerStep = 14.f;
  // Drag distance (px) covering the full 0-1 normalized range in ms mode.
  static constexpr float kMsDragRangePx = 200.f;

  TimeMagnitudeControl(const IRECT& bounds, int magnitudeParamIdx, int unitParamIdx, const char* label)
  : IControl(bounds, magnitudeParamIdx)
  , mUnitParamIdx(unitParamIdx)
  , mLabel(label)
  {
    mText = IText(10.f);
  }

  void Draw(IGraphics& g) override
  {
    const float cx = mRECT.MW();
    const float cy = mRECT.T + mRECT.W() * 0.5f;
    const float r = mRECT.W() * 0.5f - 2.f;

    constexpr float kMinAngle = -135.f, kMaxAngle = 135.f;
    // GetValue() is normalized against the underlying param's full curved range (0-6000, shared
    // with ms mode) -- in Beats mode the reachable values only go up to kNoteValues' 4-beat ceiling,
    // a tiny sliver of that range, so drawing straight off GetValue() barely deflects the knob no
    // matter the note value picked. Draw off position-in-the-note-list instead so the dial always
    // sweeps its full travel from "None" to "Whole".
    const double normalized = IsBeatsMode()
      ? static_cast<double>(NearestIndex(GetParam()->Value())) / static_cast<double>(kNoteValues.size() - 1)
      : GetValue();
    const float angle = kMinAngle + static_cast<float>(normalized) * (kMaxAngle - kMinAngle);

    g.DrawArc(COLOR_MID_GRAY, cx, cy, r, kMinAngle, kMaxAngle);
    g.DrawArc(COLOR_BLACK, cx, cy, r, kMinAngle, angle, nullptr, 2.f);
    g.DrawCircle(COLOR_BLACK, cx, cy, r);

    const float radians = angle * (3.14159265358979f / 180.f);
    g.DrawLine(COLOR_BLACK, cx, cy, cx + r * std::sin(radians), cy - r * std::cos(radians), nullptr, 2.f);

    WDL_String valueStr;
    FormatValue(valueStr);

    const IRECT labelRect = mRECT.GetFromTop(11.f);
    const IRECT valueRect = mRECT.GetFromBottom(11.f);
    g.DrawText(mText, mLabel.Get(), labelRect);
    g.DrawText(mText, valueStr.Get(), valueRect);
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    mMouseDownY = y;
    mMouseDownNormalized = GetValue();
    mMouseDownIndex = IsBeatsMode() ? NearestIndex(GetParam()->Value()) : 0;
  }

  void OnMouseDrag(float x, float y, float dX, float dY, const IMouseMod& mod) override
  {
    if (IsBeatsMode())
    {
      const float pixelsDragged = mMouseDownY - y;
      const int steps = static_cast<int>(std::round(pixelsDragged / kPixelsPerStep));
      const int newIndex = std::clamp(mMouseDownIndex + steps, 0, static_cast<int>(kNoteValues.size()) - 1);
      SetToIndex(newIndex);
    }
    else
    {
      const float pixelsDragged = mMouseDownY - y;
      const double newNormalized = std::clamp(mMouseDownNormalized + pixelsDragged / kMsDragRangePx, 0.0, 1.0);
      SetValue(newNormalized);
      SetDirty(true);
    }
  }

  void OnMouseWheel(float x, float y, const IMouseMod& mod, float d) override
  {
    if (IsBeatsMode())
    {
      const int currentIndex = NearestIndex(GetParam()->Value());
      const int newIndex = std::clamp(currentIndex + (d > 0.f ? 1 : -1), 0, static_cast<int>(kNoteValues.size()) - 1);
      SetToIndex(newIndex);
    }
    else
    {
      const double newNormalized = std::clamp(GetValue() + (d > 0.f ? 0.01 : -0.01), 0.0, 1.0);
      SetValue(newNormalized);
      SetDirty(true);
    }
  }

  void OnMouseDblClick(float x, float y, const IMouseMod& mod) override
  {
    SetValueToDefault();
  }

private:
  bool IsBeatsMode()
  {
    const IParam* unitParam = GetDelegate()->GetParam(mUnitParamIdx);
    return unitParam == nullptr || unitParam->Int() == 0; // 0 = Beats, 1 = ms
  }

  static int NearestIndex(double beats)
  {
    int best = 0;
    double bestDist = std::abs(kNoteValues[0].beats - beats);
    for (int i = 1; i < static_cast<int>(kNoteValues.size()); ++i)
    {
      const double dist = std::abs(kNoteValues[i].beats - beats);
      if (dist < bestDist)
      {
        bestDist = dist;
        best = i;
      }
    }
    return best;
  }

  void SetToIndex(int index)
  {
    const double realValue = kNoteValues[index].beats;
    SetValue(GetParam()->ToNormalized(realValue));
    SetDirty(true);
  }

  void FormatValue(WDL_String& str)
  {
    if (IsBeatsMode())
    {
      str.Set(kNoteValues[NearestIndex(GetParam()->Value())].name);
    }
    else
    {
      const double ms = GetParam()->Value();
      str.SetFormatted(32, "%.0f ms", ms);
    }
  }

  int mUnitParamIdx;
  WDL_String mLabel;
  float mMouseDownY = 0.f;
  double mMouseDownNormalized = 0.0;
  int mMouseDownIndex = 0;
};
