#pragma once

#include "IControl.h"
#include "ISender.h"
#include "IPlugStructs.h"
#include "core/NoteMatrix.h"

#include <algorithm>
#include <chrono>

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
class NoteMatrixControl : public IControl
{
public:
  NoteMatrixControl(const IRECT& bounds, sparkle_core::NoteMatrix* matrix)
  : IControl(bounds)
  , mMatrix(matrix)
  {
  }

  void Draw(IGraphics& g) override
  {
    const IRECT header = HeaderRect();
    const IRECT sideHeader = SideHeaderRect();
    const IRECT grid = GridRect();

    g.FillRect(COLOR_BLACK, mRECT);

    for (int col = 0; col < sparkle_core::kNumPitchClasses; col++)
      DrawHeaderCell(g, header.GetGridCell(0, col, 1, sparkle_core::kNumPitchClasses), AnyOnInColumn(col), AllOnInColumn(col), kPitchClassNames[col], HeaderFlashBrightness(col));

    for (int row = 0; row < sparkle_core::kNumPitchClasses; row++)
      DrawHeaderCell(g, sideHeader.GetGridCell(row, 0, sparkle_core::kNumPitchClasses, 1), AnyOnInRow(row), AllOnInRow(row), kPitchClassNames[row], 0.f);

    for (int col = 0; col < sparkle_core::kNumPitchClasses; col++)
    {
      for (int row = 0; row < sparkle_core::kNumPitchClasses; row++)
      {
        const IRECT cell = grid.GetGridCell(row, col, sparkle_core::kNumPitchClasses, sparkle_core::kNumPitchClasses).GetPadded(-1.f);
        const bool on = mMatrix->GetCell(col, row);
        const IColor base = on ? COLOR_GREEN : COLOR_DARK_GRAY;
        const float flash = FlashBrightness(col, row);
        g.FillRect(flash > 0.f ? IColor::LinearInterpolateBetween(base, COLOR_WHITE, flash) : base, cell);
      }
    }

    g.DrawRect(COLOR_WHITE, header);
    g.DrawRect(COLOR_WHITE, sideHeader);
    g.DrawRect(COLOR_WHITE, grid);
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    const IRECT header = HeaderRect();
    const IRECT sideHeader = SideHeaderRect();
    const IRECT grid = GridRect();

    if (header.Contains(x, y))
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

  IRECT HeaderRect() const { return mRECT.GetFromTop(kHeaderSize).GetReducedFromLeft(kHeaderSize); }
  IRECT SideHeaderRect() const { return mRECT.GetFromLeft(kHeaderSize).GetReducedFromTop(kHeaderSize); }
  IRECT GridRect() const { return mRECT.GetReducedFromTop(kHeaderSize).GetReducedFromLeft(kHeaderSize); }

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

  void DrawHeaderCell(IGraphics& g, const IRECT& cell, bool anyOn, bool allOn, const char* label, float flash)
  {
    const IColor base = allOn ? COLOR_GREEN : anyOn ? COLOR_ORANGE : COLOR_DARK_GRAY;
    const IColor fill = flash > 0.f ? IColor::LinearInterpolateBetween(base, COLOR_WHITE, flash) : base;
    const IRECT padded = cell.GetPadded(-1.f);
    g.FillRect(fill, padded);
    g.DrawText(IText(11.f, COLOR_WHITE), label, padded);
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
  std::array<std::array<Clock::time_point, sparkle_core::kNumPitchClasses>, sparkle_core::kNumPitchClasses> mFlashStart{};
  std::array<Clock::time_point, sparkle_core::kNumPitchClasses> mHeaderFlashStart{};
};
