#pragma once

#include "IControls.h"
#include "IPlugParameter.h"
#include "Palette.h"

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
//
// Subclasses IVKnobControl (rather than a plain IControl) purely so its chrome -- handle fill,
// press/shadow/frame, indicator track, pointer -- comes from the exact same DrawIndicatorTrack/
// DrawHandle/DrawPointer code every other knob in this project uses (see the Knob-kind branch of
// the attach loop in Sparkles.cpp), driven by the same IVStyle a plain IVKnobControl gets. Only
// DrawWidget's *angle* and SetDirty's value text are overridden below -- everything else (sizing,
// colors, label/value bands) comes straight from IVectorBase, so this knob is visually
// indistinguishable from a regular one.
class TimeMagnitudeControl : public IVKnobControl
{
public:
  struct NoteValue
  {
    const char* name;
    double beats;
  };

  // 11 divisions (128th note .. 4 whole notes/"bars") x {triplet, straight, dotted}, GLOBALLY
  // sorted ascending by actual duration -- not grouped by division -- since a bigger division's
  // triplet can be a shorter duration than a smaller division's dotted value (e.g. "4T" sits
  // between "2" and "3" below, not next to "4"), so a per-division grouping wouldn't actually be
  // ascending. Triplet = 2/3 x the straight value (3 of them fill the time 2 normally would);
  // dotted = 1.5x (adds half its own value again); both applied uniformly to every division.
  //
  // Two combinations are mathematically redundant with others already in the list and are
  // omitted entirely rather than left in as same-valued entries: a tripleted 3-bar
  // (2/3 x 12 = 8 beats) exactly equals the straight 2-bar value, and a dotted 2-bar
  // (1.5 x 8 = 12 beats) exactly equals the straight 3-bar value. Both collisions are exact, not
  // float-rounding noise, and only happen in the whole-note-and-up tier, where bars are counted
  // linearly (1, 2, 3, 4) instead of the doubling pattern every division below the whole note
  // follows -- doubling never lands a triplet/dotted value back on another named division, linear
  // counting does. A same-valued entry left in would be unreachable by name anyway:
  // NearestIndex()'s tie-break always favors whichever of a tied pair comes first, so the later
  // one could be dragged/scrolled to (its beats value stored correctly) but could never actually
  // show its own name afterward -- the display would snap to the earlier duplicate's name instead.
  //
  // Plus a leading "None" entry so Pre Delay's default of exactly 0 is reachable by scrolling/
  // dragging down.
  static constexpr std::array<NoteValue, 32> kNoteValues = { {
    { "None", 0.0 },
    { "1/128T", 2.0 / 3.0 * 1.0 / 32.0 },
    { "1/128", 1.0 / 32.0 },
    { "1/64T", 2.0 / 3.0 * 1.0 / 16.0 },
    { "1/128.", 1.5 * 1.0 / 32.0 },
    { "1/64", 1.0 / 16.0 },
    { "1/32T", 2.0 / 3.0 * 1.0 / 8.0 },
    { "1/64.", 1.5 * 1.0 / 16.0 },
    { "1/32", 1.0 / 8.0 },
    { "1/16T", 2.0 / 3.0 * 1.0 / 4.0 },
    { "1/32.", 1.5 * 1.0 / 8.0 },
    { "1/16", 1.0 / 4.0 },
    { "1/8T", 2.0 / 3.0 * 1.0 / 2.0 },
    { "1/16.", 1.5 * 1.0 / 4.0 },
    { "1/8", 1.0 / 2.0 },
    { "1/4T", 2.0 / 3.0 * 1.0 },
    { "1/8.", 1.5 * 1.0 / 2.0 },
    { "1/4", 1.0 },
    { "1/2T", 2.0 / 3.0 * 2.0 },
    { "1/4.", 1.5 * 1.0 },
    { "1/2", 2.0 },
    { "1T", 2.0 / 3.0 * 4.0 },
    { "1/2.", 1.5 * 2.0 },
    { "1", 4.0 },
    { "2T", 2.0 / 3.0 * 8.0 },
    { "1.", 1.5 * 4.0 },
    { "2", 8.0 },
    { "4T", 2.0 / 3.0 * 16.0 },
    { "3", 12.0 },
    { "4", 16.0 },
    { "3.", 1.5 * 12.0 },
    { "4.", 1.5 * 16.0 },
  } };

  // Pixels of drag (or one wheel notch) per step through kNoteValues in Beats mode.
  static constexpr float kPixelsPerStep = 14.f;
  // Drag distance (px) covering the full 0-1 normalized range in ms mode.
  static constexpr float kMsDragRangePx = 200.f;

  TimeMagnitudeControl(const IRECT& bounds, int magnitudeParamIdx, int unitParamIdx, const char* label,
                        const IVStyle& style)
  : IVKnobControl(bounds, magnitudeParamIdx, label, style)
  , mUnitParamIdx(unitParamIdx)
  {
  }

  // Identical to IVKnobControl::DrawWidget (same DrawIndicatorTrack/DrawHandle/DrawPointer calls,
  // inherited unchanged) except for `angle`. GetValue() (the param's real normalized value) only
  // ever covers a tiny sliver of the underlying 0-6000 range in Beats mode, so drawing off it
  // barely deflects the knob no matter which note value is picked -- draw off position-in-the-
  // note-list instead so the dial always sweeps its full travel from "None" to "4" (see
  // kNoteValues). In ms mode `normalized` is just GetValue(), so this is pixel-identical to the
  // base class there.
  void DrawWidget(IGraphics& g) override
  {
    const float widgetRadius = GetRadius();
    const float cx = GetWidgetBounds().MW(), cy = GetWidgetBounds().MH();
    const double normalized = IsBeatsMode()
      ? static_cast<double>(NearestIndex(GetParam()->Value())) / static_cast<double>(kNoteValues.size() - 1)
      : GetValue();
    const float angle = mAngle1 + static_cast<float>(normalized) * (mAngle2 - mAngle1);

    const IRECT knobHandleBounds = GetWidgetBounds().GetCentredInside((widgetRadius - mTrackToHandleDistance) * 2.f);
    DrawIndicatorTrack(g, angle, cx, cy, widgetRadius);
    DrawHandle(g, knobHandleBounds);
    DrawPointer(g, angle, cx, cy, knobHandleBounds.W() / 2.f);
  }

  // IVKnobControl::SetDirty populates mValueStr straight from the bound IParam's own display text
  // (GetDisplayWithLabel) -- correct in ms mode (plain milliseconds), but in Beats mode the IParam
  // has no notion of note names, so it would show the raw beats number instead of e.g. "1/8". Calls
  // IKnobControlBase::SetDirty directly (skipping IVKnobControl::SetDirty) so our own FormatValue
  // isn't immediately overwritten.
  void SetDirty(bool push, int valIdx = kNoValIdx) override
  {
    IKnobControlBase::SetDirty(push);
    FormatValue(mValueStr);
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
  float mMouseDownY = 0.f;
  double mMouseDownNormalized = 0.0;
  int mMouseDownIndex = 0;
};
