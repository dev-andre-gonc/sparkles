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

    // Bulk column/row overwrite driven by the UI's column/row toggle button (NoteMatrixControl) --
    // distinct from SetColumnEnabled/SetRowEnabled above, which gate cells without touching them.
    // If any cell in the column/row is ON, this turns all 12 OFF; otherwise it turns all 12 ON.
    // Deliberately destructive: re-enabling does not restore whatever pattern was there before
    // (explicit product choice, see the note-matrix UI work item), unlike the enabled-gate.
    void ToggleColumn(int column)
    {
      bool anyOn = false;
      for (int row = 0; row < kNumPitchClasses; ++row)
        anyOn |= mCells[column][row];
      for (int row = 0; row < kNumPitchClasses; ++row)
        mCells[column][row] = !anyOn;
    }

    void ToggleRow(int row)
    {
      bool anyOn = false;
      for (int column = 0; column < kNumPitchClasses; ++column)
        anyOn |= mCells[column][row];
      for (int column = 0; column < kNumPitchClasses; ++column)
        mCells[column][row] = !anyOn;
    }

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

  namespace detail
  {
    struct ScalePattern
    {
      int numDegrees;
      int semitones[kNumPitchClasses];
    };

    // Semitone offsets from the root, order matching the Scale enum above exactly. Expressed as
    // plain mod-12 offsets rather than real MIDI semitones -- the whole/half-step pattern of a
    // scale is transposition-invariant, so this works directly in matrix pitch-class-index space
    // (0=A..11=G#) without ever converting to/from real MIDI note numbers.
    constexpr ScalePattern kScalePatterns[] = {
      { 7, { 0, 2, 4, 5, 7, 9, 11 } },      // Ionian (Major)
      { 7, { 0, 2, 3, 5, 7, 9, 10 } },      // Dorian
      { 7, { 0, 1, 3, 5, 7, 8, 10 } },      // Phrygian
      { 7, { 0, 2, 4, 6, 7, 9, 11 } },      // Lydian
      { 7, { 0, 2, 4, 5, 7, 9, 10 } },      // Mixolydian
      { 7, { 0, 2, 3, 5, 7, 8, 10 } },      // Aeolian (Minor)
      { 7, { 0, 1, 3, 5, 6, 8, 10 } },      // Locrian
      { 7, { 0, 2, 3, 5, 7, 8, 11 } },      // Harmonic Minor
      { 7, { 0, 2, 3, 5, 7, 9, 11 } },      // Melodic Minor (ascending)
      { 5, { 0, 2, 4, 7, 9 } },             // Major Pentatonic
      { 5, { 0, 3, 5, 7, 10 } },            // Minor Pentatonic
      { 6, { 0, 3, 5, 6, 7, 10 } },         // Blues
      { 12, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 } }, // Chromatic
    };
  }

  // §5.1 key/scale quick-fill: regenerates the whole matrix from music theory, overwriting whatever
  // was there before (one-time overwrite, not a standing constraint -- individual cells/columns/rows
  // can still be hand-edited afterward, see docs/SPEC.md §5.1). `keyRoot` is a PitchClass index
  // (0=A..11=G#).
  //
  // Deliberately drives only SetCell, never SetColumnEnabled/SetRowEnabled: those are a separate
  // gate that ANDs with the cells without touching them (see NoteMatrix::EligibleNotes), and
  // EligibleNotes early-returns on a disabled column *before* ever consulting the cells. If this
  // function used that gate to blank out-of-scale columns/rows, hand-toggling one of those cells
  // back ON afterward would have no effect -- the gate would keep killing the column regardless of
  // cell state, silently ignoring the manual edit. Driving cells only means every trigger column's
  // eligibility is always exactly "whatever its cells currently say", which is what NoteMatrixControl
  // -- and every other write path -- also assumes.
  inline void ApplyKeyScale(NoteMatrix& matrix, int keyRoot, Scale scale)
  {
    const detail::ScalePattern& pattern = detail::kScalePatterns[static_cast<int>(scale)];

    std::array<bool, kNumPitchClasses> inScale{};
    for (int i = 0; i < pattern.numDegrees; ++i)
      inScale[(keyRoot + pattern.semitones[i]) % kNumPitchClasses] = true;

    for (int column = 0; column < kNumPitchClasses; ++column)
      for (int row = 0; row < kNumPitchClasses; ++row)
        matrix.SetCell(column, row, inScale[column] && inScale[row]);
  }

  // §5.1 key/scale quick-fill, "Trigger Note" root mode: instead of one fixed root shared by every
  // column, each column uses its own pitch class as the root -- i.e. the scale is built fresh
  // relative to whatever note triggered that column, rather than a single dialed-in key. Cell
  // (column, row) is ON when (row - column) mod 12 is one of the scale's degree offsets; a scale's
  // pattern always includes offset 0, so every column is trivially in its own scale and no
  // column-level gating is needed the way ApplyKeyScale's `inScale[column]` term provides.
  // Same cells-only rationale as ApplyKeyScale above applies here.
  inline void ApplyKeyScalePerColumn(NoteMatrix& matrix, Scale scale)
  {
    const detail::ScalePattern& pattern = detail::kScalePatterns[static_cast<int>(scale)];

    std::array<bool, kNumPitchClasses> offsetInScale{};
    for (int i = 0; i < pattern.numDegrees; ++i)
      offsetInScale[pattern.semitones[i]] = true;

    for (int column = 0; column < kNumPitchClasses; ++column)
      for (int row = 0; row < kNumPitchClasses; ++row)
        matrix.SetCell(column, row, offsetInScale[(row - column + kNumPitchClasses) % kNumPitchClasses]);
  }
}
