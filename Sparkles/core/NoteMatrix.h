#pragma once

#include <algorithm>
#include <array>
#include <optional>
#include <vector>

// Note eligibility matrix and eligible-note stepping, see docs/SPEC.md §5-§7.5. Deliberately free
// of iPlug2/IGraphics dependencies (like tests/test_framework.h) so it can be included by both the
// plugin and the standalone test binary.
namespace sparkle_core
{
  constexpr int kNumPitchClasses = 12;

  // Matrix index order fixed by the spec (§5): A, A#, B, C, C#, D, D#, E, F, F#, G, G#. This does
  // NOT match the standard MIDI convention where note % 12 == 0 is C — use PitchClassOf() to
  // convert a MIDI note into this order rather than indexing with note % 12 directly.
  enum PitchClass
  {
    kA = 0,
    kASharp,
    kB,
    kC,
    kCSharp,
    kD,
    kDSharp,
    kE,
    kF,
    kFSharp,
    kG,
    kGSharp
  };

  // Converts a MIDI note number to its matrix pitch-class index (0=A .. 11=G#).
  constexpr int PitchClassOf(int midiNote)
  {
    const int semitone = ((midiNote % kNumPitchClasses) + kNumPitchClasses) % kNumPitchClasses; // 0=C .. 11=B
    return (semitone + 3) % kNumPitchClasses;
  }

  enum class WrapMode
  {
    Mirror,
    Around,
    Stop
  };

  // Scale choices for the §5.1 key/scale quick-fill, which regenerates the whole matrix from
  // music theory. Order matches params/ParamList.h's kParamKeyScale option strings exactly.
  enum class Scale
  {
    Ionian, // Major
    Dorian,
    Phrygian,
    Lydian,
    Mixolydian,
    Aeolian, // Minor
    Locrian,
    HarmonicMinor,
    MelodicMinor,
    MajorPentatonic,
    MinorPentatonic,
    Blues,
    Chromatic
  };

  // 12x12 pitch-class eligibility grid (§5). Columns = trigger pitch class, rows = sparkle pitch
  // class. Cell(column, row) ON means a sparkle may land on `row`'s pitch class when the trigger
  // note's pitch class is `column`. The column/row toggles gate an entire column/row regardless of
  // individual cell state.
  class NoteMatrix
  {
  public:
    NoteMatrix()
    {
      for (auto& column : mCells)
        column.fill(true);
      mColumnEnabled.fill(true);
      mRowEnabled.fill(true);
    }

    void SetCell(int column, int row, bool on) { mCells[column][row] = on; }
    bool GetCell(int column, int row) const { return mCells[column][row]; }

    void SetColumnEnabled(int column, bool enabled) { mColumnEnabled[column] = enabled; }
    bool IsColumnEnabled(int column) const { return mColumnEnabled[column]; }

    void SetRowEnabled(int row, bool enabled) { mRowEnabled[row] = enabled; }
    bool IsRowEnabled(int row) const { return mRowEnabled[row]; }

    // Every MIDI note in [rangeMin, rangeMax] whose pitch class is an eligible sparkle row for
    // `startNote`'s column, sorted ascending (§5). Empty if the column is disabled or has no
    // eligible rows (cell off, or row toggled off).
    std::vector<int> EligibleNotes(int startNote, int rangeMin, int rangeMax) const
    {
      std::vector<int> notes;

      const int column = PitchClassOf(startNote);
      if (!IsColumnEnabled(column))
        return notes;

      for (int note = rangeMin; note <= rangeMax; ++note)
      {
        const int row = PitchClassOf(note);
        if (mCells[column][row] && IsRowEnabled(row))
          notes.push_back(note);
      }

      return notes;
    }

    // Walks `steps` positions (§7.5) through the eligible-note list for `startNote`, applying
    // `wrapMode` at the list boundaries, and returns the resulting MIDI note. Returns
    // std::nullopt ("dead") if `startNote`'s column is ineligible or has no eligible rows, or —
    // under WrapMode::Stop only — if the walk runs past a list boundary (§6, §8 #2: this kills
    // only the current ray, a concern left to the caller iterating sparkle-by-sparkle).
    //
    // `startNote`'s position in the list is its own index if it happens to be a member, otherwise
    // the index it would be inserted at (the list is sorted ascending by MIDI note).
    std::optional<int> Walk(int startNote, int steps, int rangeMin, int rangeMax, WrapMode wrapMode) const
    {
      const std::vector<int> notes = EligibleNotes(startNote, rangeMin, rangeMax);
      const int n = static_cast<int>(notes.size());
      if (n == 0)
        return std::nullopt;

      const auto it = std::lower_bound(notes.begin(), notes.end(), startNote);
      int startIdx = static_cast<int>(it - notes.begin());
      if (startIdx >= n)
        startIdx = n - 1;

      switch (wrapMode)
      {
        case WrapMode::Stop:
        {
          const int target = startIdx + steps;
          if (target < 0 || target >= n)
            return std::nullopt;
          return notes[target];
        }

        case WrapMode::Around:
        {
          int target = (startIdx + steps) % n;
          if (target < 0)
            target += n;
          return notes[target];
        }

        case WrapMode::Mirror:
        default:
        {
          if (n == 1)
            return notes[0];

          // Reflect at both boundaries without repeating the boundary note (§6, §8 #3): a
          // triangle wave over index space with period 2*(n-1).
          const int period = 2 * (n - 1);
          int m = (startIdx + steps) % period;
          if (m < 0)
            m += period;
          const int target = (m < n) ? m : (period - m);
          return notes[target];
        }
      }
    }

  private:
    std::array<std::array<bool, kNumPitchClasses>, kNumPitchClasses> mCells; // [column][row]
    std::array<bool, kNumPitchClasses> mColumnEnabled;
    std::array<bool, kNumPitchClasses> mRowEnabled;
  };
}
