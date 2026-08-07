#pragma once

#include "IControl.h"
#include "core/NoteMatrix.h"

#include <algorithm>

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
      DrawHeaderCell(g, header.GetGridCell(0, col, 1, sparkle_core::kNumPitchClasses), AnyOnInColumn(col), AllOnInColumn(col), kPitchClassNames[col]);

    for (int row = 0; row < sparkle_core::kNumPitchClasses; row++)
      DrawHeaderCell(g, sideHeader.GetGridCell(row, 0, sparkle_core::kNumPitchClasses, 1), AnyOnInRow(row), AllOnInRow(row), kPitchClassNames[row]);

    for (int col = 0; col < sparkle_core::kNumPitchClasses; col++)
    {
      for (int row = 0; row < sparkle_core::kNumPitchClasses; row++)
      {
        const IRECT cell = grid.GetGridCell(row, col, sparkle_core::kNumPitchClasses, sparkle_core::kNumPitchClasses).GetPadded(-1.f);
        const bool on = mMatrix->GetCell(col, row);
        g.FillRect(on ? COLOR_GREEN : COLOR_DARK_GRAY, cell);
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

private:
  static constexpr float kHeaderSize = 22.f;

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

  void DrawHeaderCell(IGraphics& g, const IRECT& cell, bool anyOn, bool allOn, const char* label)
  {
    const IColor fill = allOn ? COLOR_GREEN : anyOn ? COLOR_ORANGE : COLOR_DARK_GRAY;
    const IRECT padded = cell.GetPadded(-1.f);
    g.FillRect(fill, padded);
    g.DrawText(IText(11.f, COLOR_WHITE), label, padded);
  }

  sparkle_core::NoteMatrix* mMatrix;
};
