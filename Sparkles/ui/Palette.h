#pragma once

#include "IGraphicsStructs.h"

using namespace iplug;
using namespace igraphics;

// Named colors/fonts/style helpers for the "unicorn foil" look established by resources/img/
// background.png (see CLAUDE.md) -- shared by Sparkles.cpp's layout and every ui/*.h control that
// draws its own colors directly (NoteBarsControl, NoteMatrixControl, EnvelopeMeterControl, etc.)
// rather than through an IVStyle. Hex values are the palette the background artwork was made
// from; kept as named constants here so a control only has to say what a color *means*
// (kTabDetection, kTextOnAmethyst) rather than repeating a hex triplet.
namespace sparkle_palette
{
  // -- Background palette (original 8) --
  const IColor kPearlFrost(255, 243, 237, 247);
  const IColor kRoseChrome(255, 240, 175, 198);
  const IColor kPeachFoil(255, 255, 207, 163);
  const IColor kChampagne(255, 231, 200, 120);
  const IColor kMintSheen(255, 165, 230, 200);
  const IColor kAquaChrome(255, 121, 213, 232);
  const IColor kPeriwinkleFoil(255, 166, 180, 238);
  const IColor kAmethyst(255, 142, 98, 196);

  // -- Added later --
  const IColor kCobaltSheen(255, 90, 120, 220);
  const IColor kJadeFoil(255, 47, 184, 140);
  const IColor kFuchsiaChrome(255, 224, 85, 155);
  const IColor kAmethystLifted(255, 154, 112, 204);
  const IColor kPeriwinkleWarmed(255, 179, 172, 238);

  // -- Unicorn linework --
  const IColor kLinesOuter(255, 46, 27, 69);
  const IColor kLinesInterior(255, 107, 74, 130);

  // -- Text --
  const IColor kTextOnPeach(255, 74, 27, 12);
  const IColor kTextOnPeriwinkle(255, 38, 33, 92);
  const IColor kTextOnRose(255, 75, 21, 40);
  const IColor kTextOnAmethyst(255, 238, 237, 254);

  // Not in the reference swatch list -- a warm coral between kRoseChrome and kPeachFoil, for the
  // one tab (Detection) that needs its own hue distinct from its neighbors on either side.
  const IColor kCoral(255, 242, 140, 111);

  // One fill color per left-column tab pill (Quick Guide, General, Detection, Pitch/Timing, Note
  // Matrix, Synth, Presets, in that order -- see Sparkles.cpp's kParamGroups/EUITab), each paired
  // with a legible text color from the swatch list above. Ordered/colored to read the same way as
  // the reference mock's 7-pill stack top to bottom.
  struct TabColor { IColor fill, text; };
  constexpr int kNumTabColors = 7;
  inline const TabColor kTabColors[kNumTabColors] = {
    { kRoseChrome,     kTextOnRose },
    { kPeachFoil,      kTextOnPeach },
    { kCoral,          kTextOnPeach },
    { kMintSheen,      kLinesOuter },
    { kAquaChrome,     kLinesOuter },
    { kPeriwinkleFoil, kTextOnPeriwinkle },
    { kAmethyst,       kTextOnAmethyst },
  };

  // Translucent "frosted glass" card fill used behind the middle (tab content) and right
  // (persistent indicators) columns, so small text stays legible over the gradient background
  // regardless of which part of the gradient sits behind it -- see ui/CardPanelControl.h.
  const IColor kCardFill(220, 250, 246, 252);
  const IColor kCardFrame(90, 142, 98, 196);

  // Font family names, as passed to both IGraphics::LoadFont's first arg and IText's fontID --
  // must match the *_FN file macros in config.h 1:1 (LoadFont's name -> file mapping happens once
  // in mLayoutFunc's initial-attach branch).
  constexpr const char* kFontBungee = "Bungee-Regular";
  constexpr const char* kFontRighteous = "Righteous-Regular";
  constexpr const char* kFontFredokaLight = "Fredoka-Light";
  constexpr const char* kFontFredokaRegular = "Fredoka-Regular";
  constexpr const char* kFontFredokaMedium = "Fredoka-Medium";
  constexpr const char* kFontFredokaSemiBold = "Fredoka-SemiBold";
  constexpr const char* kFontFredokaBold = "Fredoka-Bold";
  constexpr const char* kFontRoboto = "Roboto-Regular";
}
