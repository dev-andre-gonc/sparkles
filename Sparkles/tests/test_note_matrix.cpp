#include "../core/NoteMatrix.h"
#include "test_framework.h"

using namespace sparkle_core;

namespace
{
  // One octave, one MIDI note per pitch class, trigger note at the low end (index 0 of the
  // eligible list when the matrix is left at its default all-ON state).
  constexpr int kRangeMin = 60; // C4
  constexpr int kRangeMax = 71; // B4
  constexpr int kStartNote = 60;
}

TEST(NoteMatrix_EmptyColumn_ReturnsDead)
{
  // Column toggled off entirely: dead regardless of individual cells.
  {
    NoteMatrix matrix;
    matrix.SetColumnEnabled(PitchClassOf(kStartNote), false);

    CHECK(matrix.Walk(kStartNote, 1, kRangeMin, kRangeMax, WrapMode::Stop) == std::nullopt);
    CHECK(matrix.Walk(kStartNote, 1, kRangeMin, kRangeMax, WrapMode::Mirror) == std::nullopt);
    CHECK(matrix.Walk(kStartNote, 1, kRangeMin, kRangeMax, WrapMode::Around) == std::nullopt);
  }

  // Column enabled but every cell in it is off: zero eligible rows, also dead.
  {
    NoteMatrix matrix;
    const int column = PitchClassOf(kStartNote);
    for (int row = 0; row < kNumPitchClasses; ++row)
      matrix.SetCell(column, row, false);

    CHECK(matrix.EligibleNotes(kStartNote, kRangeMin, kRangeMax).empty());
    CHECK(matrix.Walk(kStartNote, 0, kRangeMin, kRangeMax, WrapMode::Stop) == std::nullopt);
  }

  // Column enabled, cells on, but every row toggled off: also zero eligible rows.
  {
    NoteMatrix matrix;
    for (int row = 0; row < kNumPitchClasses; ++row)
      matrix.SetRowEnabled(row, false);

    CHECK(matrix.Walk(kStartNote, 0, kRangeMin, kRangeMax, WrapMode::Around) == std::nullopt);
  }
}

TEST(NoteMatrix_SingleEligibleNote)
{
  // Restrict the matrix so only the trigger's own pitch class is an eligible row, making the
  // eligible-note list exactly [kStartNote].
  NoteMatrix matrix;
  const int column = PitchClassOf(kStartNote);
  for (int row = 0; row < kNumPitchClasses; ++row)
  {
    if (row != column)
      matrix.SetRowEnabled(row, false);
  }

  const auto notes = matrix.EligibleNotes(kStartNote, kRangeMin, kRangeMax);
  CHECK(notes.size() == 1);
  CHECK(notes[0] == kStartNote);

  // Zero steps always lands back on the only note.
  CHECK(matrix.Walk(kStartNote, 0, kRangeMin, kRangeMax, WrapMode::Stop) == kStartNote);

  // Stop: any nonzero step immediately runs past the single-element list -> dead.
  CHECK(matrix.Walk(kStartNote, 1, kRangeMin, kRangeMax, WrapMode::Stop) == std::nullopt);
  CHECK(matrix.Walk(kStartNote, -1, kRangeMin, kRangeMax, WrapMode::Stop) == std::nullopt);

  // Mirror: nowhere else to bounce to, so it keeps landing on the only note.
  CHECK(matrix.Walk(kStartNote, 5, kRangeMin, kRangeMax, WrapMode::Mirror) == kStartNote);
  CHECK(matrix.Walk(kStartNote, -5, kRangeMin, kRangeMax, WrapMode::Mirror) == kStartNote);

  // Around: wrapping to "the opposite end" of a single-element list is itself.
  CHECK(matrix.Walk(kStartNote, 5, kRangeMin, kRangeMax, WrapMode::Around) == kStartNote);
  CHECK(matrix.Walk(kStartNote, -3, kRangeMin, kRangeMax, WrapMode::Around) == kStartNote);
}

TEST(NoteMatrix_Mirror_BouncesAtBothBoundaries)
{
  // Default matrix: every pitch class eligible, so [kRangeMin, kRangeMax] gives the full 12-note
  // eligible list with kStartNote at index 0 (the low boundary).
  NoteMatrix matrix;
  CHECK(matrix.EligibleNotes(kStartNote, kRangeMin, kRangeMax).size() == 12);

  // Walking up to exactly the high boundary (index 11) doesn't reflect yet.
  CHECK(matrix.Walk(kStartNote, 11, kRangeMin, kRangeMax, WrapMode::Mirror) == 71);

  // One step further reflects off the high boundary without repeating it (index 10, not 11).
  CHECK(matrix.Walk(kStartNote, 12, kRangeMin, kRangeMax, WrapMode::Mirror) == 70);

  // A full up-and-back bounce (2 * (n-1) = 22 steps) returns to the start.
  CHECK(matrix.Walk(kStartNote, 22, kRangeMin, kRangeMax, WrapMode::Mirror) == 60);

  // Stepping down from the low boundary (index 0) reflects immediately, landing on index 1.
  CHECK(matrix.Walk(kStartNote, -1, kRangeMin, kRangeMax, WrapMode::Mirror) == 61);
}

TEST(NoteMatrix_Around_WrapsToOppositeEnd)
{
  NoteMatrix matrix;

  // One step below the low boundary (index 0) circular-wraps to the high boundary (index 11),
  // continuing in the same (downward) direction rather than reflecting.
  CHECK(matrix.Walk(kStartNote, -1, kRangeMin, kRangeMax, WrapMode::Around) == 71);

  // A full circle (n = 12 steps) returns to the start.
  CHECK(matrix.Walk(kStartNote, 12, kRangeMin, kRangeMax, WrapMode::Around) == 60);

  // One step past a full circle continues from the start in the same direction.
  CHECK(matrix.Walk(kStartNote, 13, kRangeMin, kRangeMax, WrapMode::Around) == 61);
}

TEST(NoteMatrix_Stop_DiesPastBoundary)
{
  NoteMatrix matrix;

  // Still in range at the high boundary.
  CHECK(matrix.Walk(kStartNote, 11, kRangeMin, kRangeMax, WrapMode::Stop) == 71);

  // One step past the high boundary kills this ray's sparkle.
  CHECK(matrix.Walk(kStartNote, 12, kRangeMin, kRangeMax, WrapMode::Stop) == std::nullopt);

  // Stepping below the low boundary (already at index 0) also dies.
  CHECK(matrix.Walk(kStartNote, -1, kRangeMin, kRangeMax, WrapMode::Stop) == std::nullopt);
}

TEST(NoteMatrix_ToggleColumn_AnyOnTurnsAllOff)
{
  // Default matrix: every cell ON, so any column has at least one ON cell -- toggling must clear
  // the whole column, not just the one cell that happened to trigger the "any on" check.
  NoteMatrix matrix;
  matrix.SetCell(0, 5, false); // still leaves 11 other cells ON in column 0

  matrix.ToggleColumn(0);
  for (int row = 0; row < kNumPitchClasses; ++row)
    CHECK(matrix.GetCell(0, row) == false);

  // Other columns untouched.
  CHECK(matrix.GetCell(1, 0) == true);
}

TEST(NoteMatrix_ToggleColumn_AllOffTurnsAllOn)
{
  NoteMatrix matrix;
  for (int row = 0; row < kNumPitchClasses; ++row)
    matrix.SetCell(3, row, false);

  matrix.ToggleColumn(3);
  for (int row = 0; row < kNumPitchClasses; ++row)
    CHECK(matrix.GetCell(3, row) == true);
}

TEST(NoteMatrix_ToggleColumn_IsDestructive)
{
  // Toggling off then back on does NOT restore the original mixed pattern -- it resets to all ON,
  // by explicit design (unlike SetColumnEnabled, which preserves cells underneath).
  NoteMatrix matrix;
  matrix.SetCell(0, 0, false);
  matrix.SetCell(0, 1, false);

  matrix.ToggleColumn(0); // any on (rows 2-11) -> all off
  for (int row = 0; row < kNumPitchClasses; ++row)
    CHECK(matrix.GetCell(0, row) == false);

  matrix.ToggleColumn(0); // all off -> all on
  for (int row = 0; row < kNumPitchClasses; ++row)
    CHECK(matrix.GetCell(0, row) == true); // NOT restored to the original false/false/true.../true
}

TEST(NoteMatrix_ToggleRow_MirrorsToggleColumn)
{
  NoteMatrix matrix;
  for (int col = 0; col < kNumPitchClasses; ++col)
    matrix.SetCell(col, 7, false);

  matrix.ToggleRow(7);
  for (int col = 0; col < kNumPitchClasses; ++col)
    CHECK(matrix.GetCell(col, 7) == true);

  matrix.ToggleRow(7);
  for (int col = 0; col < kNumPitchClasses; ++col)
    CHECK(matrix.GetCell(col, 7) == false);

  // Other rows untouched.
  CHECK(matrix.GetCell(0, 0) == true);
}

TEST(NoteMatrix_ApplyKeyScale_CMajorRestrictsToWhiteKeys)
{
  // C major (Ionian, root C): scale pitch classes are C D E F G A B, i.e. matrix indices
  // {kC, kD, kE, kF, kG, kA, kB} = {3, 5, 7, 8, 10, 0, 2}.
  NoteMatrix matrix;
  ApplyKeyScale(matrix, kC, Scale::Ionian);

  const bool inScale[kNumPitchClasses] = {
    /*A*/ true, /*A#*/ false, /*B*/ true, /*C*/ true, /*C#*/ false, /*D*/ true,
    /*D#*/ false, /*E*/ true, /*F*/ true, /*F#*/ false, /*G*/ true, /*G#*/ false
  };

  for (int col = 0; col < kNumPitchClasses; ++col)
    for (int row = 0; row < kNumPitchClasses; ++row)
      CHECK(matrix.GetCell(col, row) == (inScale[col] && inScale[row]));
}

TEST(NoteMatrix_ApplyKeyScale_ChromaticEnablesEveryCell)
{
  NoteMatrix matrix;
  matrix.SetCell(0, 0, false);

  ApplyKeyScale(matrix, kA, Scale::Chromatic);

  for (int col = 0; col < kNumPitchClasses; ++col)
    for (int row = 0; row < kNumPitchClasses; ++row)
      CHECK(matrix.GetCell(col, row) == true);
}

TEST(NoteMatrix_ApplyKeyScale_DoesNotTouchTheEnabledGate)
{
  // Regression test for the exact bug this behavior was built to avoid: ApplyKeyScale must drive
  // only cells, never the separate SetColumnEnabled/SetRowEnabled gate -- otherwise a manual cell
  // edit on an out-of-scale pitch class after applying a scale would have no effect, since
  // EligibleNotes early-returns on a disabled column before ever consulting the cells.
  NoteMatrix matrix;
  ApplyKeyScale(matrix, kC, Scale::Ionian); // A# is out of scale

  CHECK(matrix.IsColumnEnabled(kASharp) == true);
  CHECK(matrix.IsRowEnabled(kASharp) == true);

  // Hand-editing an out-of-scale cell back ON must actually make that note eligible.
  matrix.SetCell(kASharp, kASharp, true);
  CHECK(matrix.EligibleNotes(70 /* A#4 */, 60, 71) == std::vector<int>{ 70 });
}

TEST(NoteMatrix_ApplyKeyScale_Triad_BuildsChordFromColumnsOwnDegree)
{
  // §5.1 Mode: C major, Triad -- the D column's eligible rows are D, F, A (whatever triad fits
  // within C major starting on D), not the full C major scale.
  NoteMatrix matrix;
  ApplyKeyScale(matrix, kC, Scale::Ionian, ChordMode::Triad);

  for (int row = 0; row < kNumPitchClasses; ++row)
    CHECK(matrix.GetCell(kD, row) == (row == kD || row == kF || row == kA));
}

TEST(NoteMatrix_ApplyKeyScale_PowerChord_OmitsTheThird)
{
  NoteMatrix matrix;
  ApplyKeyScale(matrix, kC, Scale::Ionian, ChordMode::PowerChord);

  for (int row = 0; row < kNumPitchClasses; ++row)
    CHECK(matrix.GetCell(kD, row) == (row == kD || row == kA));
}

TEST(NoteMatrix_ApplyKeyScale_Root_OnlyTriggersOwnPitchClass)
{
  NoteMatrix matrix;
  ApplyKeyScale(matrix, kC, Scale::Ionian, ChordMode::Root);

  const bool inScale[kNumPitchClasses] = {
    /*A*/ true, /*A#*/ false, /*B*/ true, /*C*/ true, /*C#*/ false, /*D*/ true,
    /*D#*/ false, /*E*/ true, /*F*/ true, /*F#*/ false, /*G*/ true, /*G#*/ false
  };

  for (int column = 0; column < kNumPitchClasses; ++column)
    for (int row = 0; row < kNumPitchClasses; ++row)
      CHECK(matrix.GetCell(column, row) == (inScale[column] && row == column));
}

TEST(NoteMatrix_ApplyKeyScale_OutOfScaleColumnStaysEmptyRegardlessOfChordMode)
{
  // A column outside the scale gets zero eligible rows no matter how wide the chord extension --
  // same gate as ChordMode::FullScale's inScale[column] term.
  NoteMatrix matrix;
  ApplyKeyScale(matrix, kC, Scale::Ionian, ChordMode::Thirteenth); // A# is out of C major

  for (int row = 0; row < kNumPitchClasses; ++row)
    CHECK(matrix.GetCell(kASharp, row) == false);
}

TEST(NoteMatrix_ApplyKeyScalePerColumn_Triad_UsesColumnAsOwnRoot)
{
  // "Trigger Note" root mode + Triad: every column builds a triad using its own pitch class as
  // root -- e.g. Ionian rooted at D gives D, F#, A (D major triad), unlike ApplyKeyScale's
  // fixed-C-major D triad (D, F, A) above.
  NoteMatrix matrix;
  ApplyKeyScalePerColumn(matrix, Scale::Ionian, ChordMode::Triad);

  for (int row = 0; row < kNumPitchClasses; ++row)
    CHECK(matrix.GetCell(kD, row) == (row == kD || row == kFSharp || row == kA));
}

TEST(NoteMatrix_ApplyKeyScalePerColumn_EachColumnUsesItsOwnRoot)
{
  // Major (Ionian) degree offsets from the root: {0, 2, 4, 5, 7, 9, 11}. With "Trigger Note" as
  // root, every column's eligible rows are its own pitch class plus those offsets -- not a single
  // shared scale intersected across both axes like ApplyKeyScale.
  NoteMatrix matrix;
  ApplyKeyScalePerColumn(matrix, Scale::Ionian);

  const bool degreeOffset[kNumPitchClasses] = {
    /*0*/ true, /*1*/ false, /*2*/ true, /*3*/ false, /*4*/ true, /*5*/ true,
    /*6*/ false, /*7*/ true, /*8*/ false, /*9*/ true, /*10*/ false, /*11*/ true
  };

  for (int column = 0; column < kNumPitchClasses; ++column)
    for (int row = 0; row < kNumPitchClasses; ++row)
      CHECK(matrix.GetCell(column, row) == degreeOffset[(row - column + kNumPitchClasses) % kNumPitchClasses]);

  // Every column is in its own scale (offset 0 is always a degree), unlike ApplyKeyScale where a
  // column outside the fixed key is entirely OFF.
  for (int column = 0; column < kNumPitchClasses; ++column)
    CHECK(matrix.GetCell(column, column) == true);
}

TEST(NoteMatrix_ApplyKeyScalePerColumn_ChromaticEnablesEveryCell)
{
  NoteMatrix matrix;
  matrix.SetCell(0, 0, false);

  ApplyKeyScalePerColumn(matrix, Scale::Chromatic);

  for (int col = 0; col < kNumPitchClasses; ++col)
    for (int row = 0; row < kNumPitchClasses; ++row)
      CHECK(matrix.GetCell(col, row) == true);
}

TEST(NoteMatrix_ApplyKeyScalePerColumn_DoesNotTouchTheEnabledGate)
{
  NoteMatrix matrix;
  matrix.SetColumnEnabled(kC, false);
  matrix.SetRowEnabled(kD, false);

  ApplyKeyScalePerColumn(matrix, Scale::Ionian);

  CHECK(matrix.IsColumnEnabled(kC) == false);
  CHECK(matrix.IsRowEnabled(kD) == false);
}

TEST(NoteMatrix_Walk_TwoNoteOverload_ColumnAndAnchorAreIndependent)
{
  // Two-note Walk() overload (Sparkles' §7.2 Pre Interval): `columnNote` picks which column's
  // eligibility rules apply, `anchorNote` only picks where `steps` starts counting from -- they
  // can disagree, and the column note's own column must be the one consulted.
  NoteMatrix matrix;
  matrix.SetColumnEnabled(PitchClassOf(67) /* G */, false);

  // anchorNote=67 (G) would be dead if its own column were consulted -- but columnNote=60 (C) is
  // enabled, so the walk succeeds and steps from G4's position in C4's own eligible list.
  CHECK(matrix.Walk(/*columnNote=*/60, /*anchorNote=*/67, /*steps=*/0, 60, 71, WrapMode::Stop) == 67);

  // Disabling the column note's own column still kills it, regardless of anchorNote.
  matrix.SetColumnEnabled(PitchClassOf(60) /* C */, false);
  CHECK(matrix.Walk(/*columnNote=*/60, /*anchorNote=*/67, /*steps=*/0, 60, 71, WrapMode::Stop) == std::nullopt);
}

TEST(NoteMatrix_Walk_TwoNoteOverload_MatchesSingleNoteOverloadWhenSame)
{
  // The single-note Walk() is exactly the two-note overload called with columnNote==anchorNote --
  // covering both here guards against the shim and the real implementation drifting apart.
  NoteMatrix matrix;
  for (int row = 0; row < kNumPitchClasses; ++row)
    if (row != PitchClassOf(kStartNote)) matrix.SetRowEnabled(row, false);

  for (int steps = -3; steps <= 3; ++steps)
    CHECK(matrix.Walk(kStartNote, steps, kRangeMin, kRangeMax, WrapMode::Around) ==
          matrix.Walk(kStartNote, kStartNote, steps, kRangeMin, kRangeMax, WrapMode::Around));
}
