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
