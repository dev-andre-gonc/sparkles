#pragma once

#include "IControl.h"
#include "ISender.h"
#include "IPlugStructs.h"
#include "Palette.h"
#include "core/NoteMatrix.h"

#include <algorithm>
#include <chrono>
#include <functional>

using namespace iplug;
using namespace igraphics;

// Interactive 12x12 note-eligibility grid (docs/SPEC.md §5), plus its 12 column and 12 row toggle
// headers, as a single composite control rather than 144+24 separate IVButtonControls -- every
// cell reads/writes the same shared sparkle_core::NoteMatrix directly (it isn't IParam-backed, see
// params/ParamSnapshot.h's header comment), and one control keeps resize/tag bookkeeping to the
// single tag mLayoutFunc already reserves for it.
//
// mMatrix is written here on the UI thread and read directly by ProcessBlock/SparkleGenerator on
// the audio thread with no synchronization -- same precedent as Sparkles::RebuildNoteCandidates()
// mutating its candidate table from OnParamChange. Acceptable here too since NoteMatrix::Walk()
// only runs once per trigger (not per sample), so a torn read at worst affects a single in-flight
// sprinkle, and every field involved is a plain bool.
//
// A 13th "trigger row" (TriggerRowRect) sits above the column headers -- 12 piano-key-style
// buttons, one per pitch class, that let a user fire a sprinkle by hand via the `onTrigger`
// callback (bound to Sparkles::PushManualTrigger) rather than only ever reading/writing mMatrix.
class NoteMatrixControl : public IControl
{
public:
  // Height of the "play" row drawn above the column headers (see TriggerRowRect) -- public so
  // mLayoutFunc can size this control's bounds tall enough to fit it without duplicating the
  // number. Same height as kHeaderSize below, just a separate named constant since they mean
  // different things (a note-matrix header vs. a piano-style trigger button).
  static constexpr float kTriggerRowSize = 22.f;

  // `onTrigger`, if set, is called with a pitch-class index (0=A..11=G#) when the user clicks one
  // of the 12 trigger-row buttons above the grid -- lets the plugin be played by hand, exactly as
  // if the trigger arrived via audio or MIDI (see Sparkles::PushManualTrigger, which the caller
  // typically binds this to).
  NoteMatrixControl(const IRECT& bounds, sparkle_core::NoteMatrix* matrix, std::function<void(int)> onTrigger = nullptr)
  : IControl(bounds)
  , mMatrix(matrix)
  , mOnTrigger(std::move(onTrigger))
  {
  }

  void Draw(IGraphics& g) override
  {
    const IRECT triggerRow = TriggerRowRect();
    const IRECT header = HeaderRect();
    const IRECT sideHeader = SideHeaderRect();
    const IRECT grid = GridRect();
    const IRECT corner = CornerRect();

    g.FillRect(sparkle_palette::kLinesOuter.WithOpacity(0.3f), mRECT);

    // Corner square has no on/off state of its own -- reads as part of the header language, so it
    // takes the same "off" fill DrawHeaderCell uses for a row/column with nothing on.
    g.FillRect(sparkle_palette::kLinesInterior.WithOpacity(0.55f), corner);

    for (int col = 0; col < sparkle_core::kNumPitchClasses; col++)
    {
      // Trigger-row cells share the column header's own flash state (HeaderFlashBrightness) rather
      // than tracking their own -- a manual click fires a real FireSprinkle, which pushes exactly
      // the same header-flash message a trigger arriving via audio/MIDI would (see
      // OnMsgFromDelegate's row < 0 case), so the two light up together for free.
      const float flash = HeaderFlashBrightness(col);
      DrawTriggerCell(g, triggerRow.GetGridCell(0, col, 1, sparkle_core::kNumPitchClasses), flash);
      DrawHeaderCell(g, header.GetGridCell(0, col, 1, sparkle_core::kNumPitchClasses), AnyOnInColumn(col), AllOnInColumn(col), kPitchClassNames[col], flash);
    }

    for (int row = 0; row < sparkle_core::kNumPitchClasses; row++)
      DrawHeaderCell(g, sideHeader.GetGridCell(row, 0, sparkle_core::kNumPitchClasses, 1), AnyOnInRow(row), AllOnInRow(row), kPitchClassNames[row], 0.f);

    for (int col = 0; col < sparkle_core::kNumPitchClasses; col++)
    {
      for (int row = 0; row < sparkle_core::kNumPitchClasses; row++)
      {
        const IRECT cell = grid.GetGridCell(row, col, sparkle_core::kNumPitchClasses, sparkle_core::kNumPitchClasses);
        const bool on = mMatrix->GetCell(col, row);
        const IColor base = on ? sparkle_palette::kAquaChrome : sparkle_palette::kLinesInterior.WithOpacity(0.0f);
        const float flash = FlashBrightness(col, row);
        g.FillRect(flash > 0.f ? IColor::LinearInterpolateBetween(base, sparkle_palette::kPearlFrost, flash) : base, cell);
      }
    }

    // Interior cell-divider lines -- same color as the outer border (kLinesOuter) but at the
    // unselected tab pills' thickness (kLineThickness), so the grid reads as one consistent
    // linework style with the rest of the UI rather than relying on gaps revealing whatever's
    // behind (no longer safe now that this control's own backing fill is translucent, not solid).
    DrawInteriorGridLines(g, triggerRow, sparkle_core::kNumPitchClasses, 1);
    DrawInteriorGridLines(g, header, sparkle_core::kNumPitchClasses, 1);
    DrawInteriorGridLines(g, sideHeader, 1, sparkle_core::kNumPitchClasses);
    DrawInteriorGridLines(g, grid, sparkle_core::kNumPitchClasses, sparkle_core::kNumPitchClasses);

    // Outer borders draw at kLineThicknessSelected (1.8x kLineThickness) -- same relationship as a
    // selected tab pill's frame vs. an unselected one -- so the matrix's own outline reads bolder
    // than its interior gridlines.
    g.DrawRect(sparkle_palette::kLinesOuter, triggerRow, nullptr, sparkle_palette::kLineThickness * 1.1f);
    g.DrawRect(sparkle_palette::kLinesOuter, header, nullptr, sparkle_palette::kLineThickness * 1.1f);
    g.DrawRect(sparkle_palette::kLinesOuter, sideHeader, nullptr, sparkle_palette::kLineThickness * 1.1f);
    g.DrawRect(sparkle_palette::kLinesOuter, grid, nullptr, sparkle_palette::kLineThickness * 1.1f);
    g.DrawRect(sparkle_palette::kLinesOuter, corner, nullptr, sparkle_palette::kLineThickness * 1.1f);
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    const IRECT triggerRow = TriggerRowRect();
    const IRECT header = HeaderRect();
    const IRECT sideHeader = SideHeaderRect();
    const IRECT grid = GridRect();

    if (triggerRow.Contains(x, y))
    {
      // No matrix state to mutate and no SetDirty needed -- the real FireSprinkle this triggers
      // (see Sparkles::PushManualTrigger) round-trips back through mNoteMatrixFlashSender and
      // starts the header-flash animation itself, same as an audio/MIDI-triggered column.
      if (mOnTrigger)
        mOnTrigger(IndexAt(triggerRow, x, true));
      return;
    }
    else if (header.Contains(x, y))
    {
      mMatrix->ToggleColumn(IndexAt(header, x, true));
    }
    else if (sideHeader.Contains(x, y))
    {
      mMatrix->ToggleRow(IndexAt(sideHeader, y, false));
    }
    else if (grid.Contains(x, y))
    {
      const int col = IndexAt(grid, x, true);
      const int row = IndexAt(grid, y, false);
      mMatrix->SetCell(col, row, !mMatrix->GetCell(col, row));
    }
    else
      return;

    SetDirty(false);
  }

  // Fed by Sparkles::mNoteMatrixFlashSender, two distinct events sharing one {column, row} pair:
  //  - {column, row >= 0}: one distinct cell a trigger's sparkle notes actually landed on, pushed
  //    sample-accurately as each note actually fires (see Sparkles::ProcessBlock's flash flush
  //    loop) -- this is the per-cell "playing" flash.
  //  - {column, -1}: pushed immediately when a trigger creates a sparkle for that column (see
  //    FireSprinkle), well before any of its notes' cell flashes above land -- flashes only the
  //    column header, giving instant feedback that a trigger was recognized even if the sparkle's
  //    notes haven't started sounding yet (or never do, e.g. an empty column under the current
  //    matrix). Row < 0 is the signal to route to mHeaderFlashStart instead of mFlashStart, rather
  //    than adding a second ISender -- both share the same {column, row}-pair shape already.
  //
  // Records the hit time and (re)starts the fade-out animation; unlike TriggerLightControl's
  // single light, an arbitrary number of cells/headers can be fading at once, so brightness is
  // computed per-cell from wall-clock time elapsed since its own last hit (FlashBrightness /
  // HeaderFlashBrightness) rather than from IControl's built-in GetAnimationProgress(), which only
  // tracks one start time per control.
  void OnMsgFromDelegate(int msgTag, int dataSize, const void* pData) override
  {
    if (msgTag != ISender<>::kUpdateMessage)
      return;

    IByteStream stream(pData, dataSize);
    ISenderData<2> d;
    stream.Get(&d, 0);

    const int col = static_cast<int>(d.vals[0]);
    const int row = static_cast<int>(d.vals[1]);
    if (row < 0)
      mHeaderFlashStart[col] = Clock::now();
    else
      mFlashStart[col][row] = Clock::now();

    // Animation func just needs to keep redrawing every frame while any cell/header is still
    // fading; FlashBrightness/HeaderFlashBrightness (read from Draw()) do the actual per-cell decay
    // math against mFlashStart/mHeaderFlashStart, so re-triggering something that's already fading
    // needs no special handling here -- the write above already picked up the new start time.
    if (!GetAnimationFunction())
    {
      SetAnimation([this](IControl* pCaller) {
        pCaller->SetDirty(false);

        const auto now = Clock::now();
        for (const auto& column : mFlashStart)
          for (const auto& t : column)
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - t).count() < kFlashDurationMs)
              return;
        for (const auto& t : mHeaderFlashStart)
          if (std::chrono::duration_cast<std::chrono::milliseconds>(now - t).count() < kFlashDurationMs)
            return;

        pCaller->OnEndAnimation();
      }, kFlashDurationMs);
    }
  }

private:
  static constexpr float kHeaderSize = 22.f;
  static constexpr int kFlashDurationMs = 400;
  using Clock = std::chrono::steady_clock;

  // Matrix index order fixed by sparkle_core::PitchClass (0=A..11=G#).
  static constexpr const char* kPitchClassNames[sparkle_core::kNumPitchClasses] = {
    "A", "A#", "B", "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#"
  };

  // Trigger row sits above everything else, spanning the same x-range as the column headers/grid
  // (reduced from left by kHeaderSize so it lines up with them, not the side-header column) --
  // everything below is then reduced from the top by kTriggerRowSize on top of its previous offset.
  IRECT TriggerRowRect() const { return mRECT.GetFromTop(kTriggerRowSize).GetReducedFromLeft(kHeaderSize); }
  IRECT HeaderRect() const { return mRECT.GetReducedFromTop(kTriggerRowSize).GetFromTop(kHeaderSize).GetReducedFromLeft(kHeaderSize); }
  IRECT SideHeaderRect() const { return mRECT.GetFromLeft(kHeaderSize).GetReducedFromTop(kTriggerRowSize + kHeaderSize); }
  IRECT GridRect() const { return mRECT.GetReducedFromTop(kTriggerRowSize + kHeaderSize).GetReducedFromLeft(kHeaderSize); }

  // The square above SideHeaderRect and left of TriggerRowRect/HeaderRect -- none of those three
  // rects reach it (each is reduced away from this corner so it doesn't overlap the others), so
  // without its own rect this square was never filled or bordered, leaving a gap in the outline.
  IRECT CornerRect() const { return mRECT.GetFromLeft(kHeaderSize).GetFromTop(kTriggerRowSize + kHeaderSize); }

  // `horizontal` picks whether the 12-way split is along x (column headers/grid columns) or y (row
  // headers/grid rows).
  static int IndexAt(const IRECT& r, float pos, bool horizontal)
  {
    const float frac = horizontal ? (pos - r.L) / r.W() : (pos - r.T) / r.H();
    return std::clamp(static_cast<int>(frac * sparkle_core::kNumPitchClasses), 0, sparkle_core::kNumPitchClasses - 1);
  }

  bool AnyOnInColumn(int col) const
  {
    for (int row = 0; row < sparkle_core::kNumPitchClasses; row++)
      if (mMatrix->GetCell(col, row)) return true;
    return false;
  }

  bool AllOnInColumn(int col) const
  {
    for (int row = 0; row < sparkle_core::kNumPitchClasses; row++)
      if (!mMatrix->GetCell(col, row)) return false;
    return true;
  }

  bool AnyOnInRow(int row) const
  {
    for (int col = 0; col < sparkle_core::kNumPitchClasses; col++)
      if (mMatrix->GetCell(col, row)) return true;
    return false;
  }

  bool AllOnInRow(int row) const
  {
    for (int col = 0; col < sparkle_core::kNumPitchClasses; col++)
      if (!mMatrix->GetCell(col, row)) return false;
    return true;
  }

  // Piano-key-style "play" button -- unlike DrawHeaderCell's on/off/mixed states (there's no
  // matrix state to represent here), always the same accent color, just brightened on its shared
  // header flash like a key lighting up as it's played. Deliberately unlabeled (unlike every other
  // cell in this control) -- the column header directly below already names the pitch class.
  void DrawTriggerCell(IGraphics& g, const IRECT& cell, float flash)
  {
    const IColor base = sparkle_palette::kPaleOrange;
    const IColor fill = flash > 0.f ? IColor::LinearInterpolateBetween(base, sparkle_palette::kPearlFrost, flash) : base;
    g.FillRect(fill, cell);
  }

  // Draws the lines between adjacent cells of a `cols` x `rows` region -- excludes the region's own
  // outer edge (i.e. i/j run 1..cols-1 / 1..rows-1) since Draw() already strokes that separately, at
  // a heavier thickness (kLineThicknessSelected vs. this function's kLineThickness).
  static void DrawInteriorGridLines(IGraphics& g, const IRECT& r, int cols, int rows)
  {
    for (int i = 1; i < cols; i++)
    {
      const float x = r.L + r.W() * (static_cast<float>(i) / static_cast<float>(cols));
      g.DrawLine(sparkle_palette::kLinesOuter, x, r.T, x, r.B, nullptr, sparkle_palette::kLineThickness);
    }
    for (int j = 1; j < rows; j++)
    {
      const float y = r.T + r.H() * (static_cast<float>(j) / static_cast<float>(rows));
      g.DrawLine(sparkle_palette::kLinesOuter, r.L, y, r.R, y, nullptr, sparkle_palette::kLineThickness);
    }
  }

  // allOn uses the same accent color (kAmethyst, the Presets button's fill) for both column and
  // row headers -- "full on" reads as one consistent state regardless of which axis it's on.
  void DrawHeaderCell(IGraphics& g, const IRECT& cell, bool anyOn, bool allOn, const char* label, float flash)
  {
    const IColor base = allOn ? sparkle_palette::kCustomGreen : anyOn ? sparkle_palette::kMintSheen : sparkle_palette::kLinesInterior.WithOpacity(0.55f);
    const IColor fill = flash > 0.f ? IColor::LinearInterpolateBetween(base, sparkle_palette::kPearlFrost, flash) : base;
    g.FillRect(fill, cell);
    g.DrawText(IText(11.f, sparkle_palette::kLinesOuter, sparkle_palette::kFontFredokaMedium), label, cell.GetPadded(-1.f));
  }

  // Elapsed-since-hit brightness for one cell (0 = not flashing / fully faded), read by Draw() and
  // driven by the animation started in OnMsgFromDelegate. Default-constructed time_points sit at
  // the clock epoch, arbitrarily far in the past, so an untouched cell already reads brightness 0.
  float FlashBrightness(int col, int row) const
  {
    const auto elapsedMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - mFlashStart[col][row]).count();
    return elapsedMs < kFlashDurationMs ? 1.f - static_cast<float>(elapsedMs) / kFlashDurationMs : 0.f;
  }

  // Same decay math as FlashBrightness, against the column-header-only hit time set when a trigger
  // creates a sparkle for that column (row < 0 case in OnMsgFromDelegate) rather than when one of
  // its notes actually sounds.
  float HeaderFlashBrightness(int col) const
  {
    const auto elapsedMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - mHeaderFlashStart[col]).count();
    return elapsedMs < kFlashDurationMs ? 1.f - static_cast<float>(elapsedMs) / kFlashDurationMs : 0.f;
  }

  sparkle_core::NoteMatrix* mMatrix;
  std::function<void(int)> mOnTrigger;
  std::array<std::array<Clock::time_point, sparkle_core::kNumPitchClasses>, sparkle_core::kNumPitchClasses> mFlashStart{};
  std::array<Clock::time_point, sparkle_core::kNumPitchClasses> mHeaderFlashStart{};
};
