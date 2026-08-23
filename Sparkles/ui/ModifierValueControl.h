#pragma once

#include "IControl.h"
#include "IPlugParameter.h"
#include "Palette.h"

#include <algorithm>
#include <cmath>

using namespace iplug;
using namespace igraphics;

// Condensed Rm/Sm modifier display: "x1.20 p/ray" / "x0.90 p/sparkle", drawn as plain text with
// no knob graphic at all -- the whole rect is draggable (vertical drag, like a knob with no
// visible face) and scrollable, exactly like ui/TimeMagnitudeControl.h's ms-mode branch, just
// without ever drawing a shell. Lives beside its cluster's base knob (see mLayoutFunc's per-
// cluster attach loop in Sparkles.cpp) rather than stacked as a small square knob -- the base
// control already names the property, so this only ever needs to show the multiplier value.
// Text is left-aligned (EAlign::Near) and the control's own rect starts right beside the base
// knob -- centering it in the wider chip rect (kChipWRay/kChipWSparkle) made the text read as if
// it were floating far from the knob it modifies, even though the box itself was already close.
//
// Ray Rotation's Rm ("Keep"/"Invert") is the one enum-valued exception -- rather than a
// continuous multiplier, GetParam()->Value() there is just 0 or 1, so it's shown as "x1"/"x-1"
// (NDisplayTexts() == 2 is the tell) instead of a misleading "x0.00"/"x1.00".
//
// That same NDisplayTexts() == 2 tell also switches the *interaction* model, not just the display
// text: a continuous multiplier's drag/scroll (100+px of drag or dozens of scroll notches to cross
// the 0.5 rounding threshold, see IPlugParameter's kFlagStepped Constrain()) reads as completely
// unresponsive for a param that only ever has two real states. Every other 2-option control in this
// whole UI (Wrap, Panning, the *Unit toggles, even Ray Rotation's own base "L"/"R" -- all
// IVSwitchControl/ISwitchControlBase) flips instantly on a single click instead, so a 2-option Rm
// matches that here too: click (mouse down) or any scroll notch flips it outright, and drag is a
// no-op rather than fighting the click that already happened on mouse-down.
class ModifierValueControl : public IControl
{
public:
  // Pixels of vertical drag covering the param's full normalized 0-1 range.
  static constexpr float kDragRangePx = 200.f;
  // Normalized value change per wheel notch.
  static constexpr double kWheelStep = 0.01;

  ModifierValueControl(const IRECT& bounds, int paramIdx, const char* suffix)
  : IControl(bounds, paramIdx)
  , mSuffix(suffix)
  {
    mText = IText(10.f, sparkle_palette::kLinesInterior, sparkle_palette::kFontFredokaMedium, EAlign::Near);
  }

  void Draw(IGraphics& g) override
  {
    WDL_String str;
    if (GetParam()->NDisplayTexts() == 2)
      str.SetFormatted(32, "x%d %s", GetParam()->Value() < 0.5 ? 1 : -1, mSuffix.Get());
    else
      str.SetFormatted(32, "x%.2f %s", GetParam()->Value(), mSuffix.Get());
    g.DrawText(mText, str.Get(), mRECT);
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    mMouseDownY = y;
    mMouseDownNormalized = GetValue();

    if (IsToggleParam()) {
      SetValue(GetValue() < 0.5 ? 1.0 : 0.0);
      SetDirty(true);
    }
  }

  void OnMouseDrag(float x, float y, float dX, float dY, const IMouseMod& mod) override
  {
    // Already flipped outright on the mouse-down that started this drag -- see class comment.
    if (IsToggleParam())
      return;

    const float pixelsDragged = mMouseDownY - y;
    const double newNormalized = std::clamp(mMouseDownNormalized + pixelsDragged / kDragRangePx, 0.0, 1.0);
    SetValue(newNormalized);
    SetDirty(true);
  }

  void OnMouseWheel(float x, float y, const IMouseMod& mod, float d) override
  {
    if (IsToggleParam()) {
      SetValue(GetValue() < 0.5 ? 1.0 : 0.0);
      SetDirty(true);
      return;
    }

    const double newNormalized = std::clamp(GetValue() + (d > 0.f ? kWheelStep : -kWheelStep), 0.0, 1.0);
    SetValue(newNormalized);
    SetDirty(true);
  }

  void OnMouseDblClick(float x, float y, const IMouseMod& mod) override
  {
    SetValueToDefault();
  }

private:
  bool IsToggleParam() const { return GetParam()->NDisplayTexts() == 2; }

  WDL_String mSuffix;
  float mMouseDownY = 0.f;
  double mMouseDownNormalized = 0.0;
};
