#pragma once

#include "IControl.h"
#include <cctype>
#include <string>
#include <vector>

using namespace iplug;
using namespace igraphics;

// Word-wrapped prose control for the Quick Guide tab, supporting a lightweight "**bold**" inline
// markup so a paragraph can call out specific tab/parameter names without needing a full rich-text
// engine. IGraphics's own IMultiLineTextControl (NanoVG's DrawMultiLineText/nvgTextBreakLines,
// see that class) only ever draws one font per control, so mixing regular and bold words on the
// same wrapped line needs its own word-by-word layout instead -- this measures/draws one token at
// a time via plain IGraphics::MeasureText/DrawText, switching between `text`'s font (regular) and
// `boldFontID` per token.
//
// MeasureHeight runs the identical layout without drawing, so mLayoutFunc can size each guide-text
// block to its own actual rendered height (and stack the next block right after it) instead of
// guessing a fixed box per block and leaving dead space when the text falls short of it.
class GuideTextControl : public IControl
{
public:
  // `align` is this control's own line-by-line horizontal alignment (Near/Center/Far), independent
  // of `text`'s -- Layout always lays out left-to-right internally (it has to, to know each word's
  // running x before it knows the line's total width) and only applies `align` as a whole-line
  // offset once a line is complete, so `text.mAlign` is never consulted.
  GuideTextControl(const IRECT& bounds, const char* markupStr, const IText& text, const char* boldFontID, EAlign align = EAlign::Near)
  : IControl(bounds)
  , mMarkup(markupStr)
  , mBoldFontID(boldFontID)
  , mAlign(align)
  {
    mText = text.WithVAlign(EVAlign::Top);
    mIgnoreMouse = true;
  }

  static float MeasureHeight(IGraphics& g, const char* markupStr, const IText& text, const char* boldFontID, float width)
  {
    const IText regular = text.WithVAlign(EVAlign::Top);
    const IText bold = regular.WithFont(boldFontID);
    float bottom = 0.f;
    Layout(g, markupStr, regular, bold, width, EAlign::Near, [&](const IText&, const char*, float, float lineY) {
      bottom = lineY + regular.mSize * kLineHeightMult;
    });
    return bottom;
  }

  void Draw(IGraphics& g) override
  {
    const IText regular = mText;
    const IText bold = regular.WithFont(mBoldFontID);
    Layout(g, mMarkup.c_str(), regular, bold, mRECT.W(), mAlign, [&](const IText& style, const char* word, float x, float y) {
      g.DrawText(style, word, mRECT.L + x, mRECT.T + y);
    });
  }

private:
  // ~Fraction of font size a Fredoka space glyph advances -- MeasureText (see below) can't supply
  // this itself, so it's a tuned constant rather than a measurement.
  static constexpr float kSpaceWidthEm = 0.27f;
  static constexpr float kLineHeightMult = 1.32f;

  // NanoVG rasterizes/hints glyphs at the canvas's *live* draw scale (see nvg__getFontScale in
  // nanovg.c), then divides the measured extents back down into logical units -- so the same word,
  // at the same logical font size, can measure a fraction of a px differently at two different
  // zoom levels. During a smooth drag on the corner resizer (which continuously changes that
  // scale, see CLAUDE.md's "Aspect ratio is locked") a word sitting right at the wrap boundary can
  // measure on either side of it from one frame to the next, visibly hopping down to the next line
  // and back. This applies to every GuideTextControl alike (Bullets included -- it's the same
  // Layout call as everything else, just with a narrower/indented box), so a longer, denser line
  // with more glyphs -- and, for Bullets specifically, a run of **bold** words rasterized from a
  // second font face -- accumulates more jitter across the line than a short one. 4px, then 16px,
  // both still left a visible spot; the margin needs to comfortably outrun the worst realistic
  // line, not just a typical one. Line-breaking this far before the true edge is a deliberately
  // wasteful trade: it costs some trailing whitespace on every line (the box is wider than the
  // text strictly needs), in exchange for wrap decisions that stay put across ordinary jitter
  // instead of flipping frame to frame.
  static constexpr float kWrapSlackPx = 28.f;

  struct Token { std::string text; bool bold; bool sticky; bool newline; };

  // Splits on whitespace (each '\n' becomes its own newline token, forcing a line break -- so
  // "\n\n" in a markup string reads as one blank row, same as IMultiLineTextControl's convention).
  // A "**" pair toggles bold starting at that exact point, including mid-word (e.g. "On**:" keeps
  // ":" attached to "On" with no space, both via `sticky`) and across word boundaries.
  static std::vector<Token> Tokenize(const char* str)
  {
    std::vector<Token> tokens;
    std::string word;
    bool bold = false;
    auto flush = [&]() {
      if (word.empty())
        return;
      std::vector<std::string> parts;
      size_t start = 0, pos;
      while ((pos = word.find("**", start)) != std::string::npos) {
        parts.push_back(word.substr(start, pos - start));
        start = pos + 2;
      }
      parts.push_back(word.substr(start));
      // sticky must be false for the *last visible* part, not just the last element of `parts` --
      // a word ending in a marker (e.g. "Detection**") leaves a trailing empty part after it, which
      // would otherwise make the real last word ("Detection") wrongly sticky and swallow the space
      // that's supposed to follow it before the next raw word.
      size_t lastNonEmpty = std::string::npos;
      for (size_t i = 0; i < parts.size(); i++)
        if (!parts[i].empty())
          lastNonEmpty = i;
      for (size_t i = 0; i < parts.size(); i++) {
        if (i > 0)
          bold = !bold;
        if (!parts[i].empty())
          tokens.push_back({ parts[i], bold, i != lastNonEmpty, false });
      }
      word.clear();
    };
    for (const char* p = str; *p; p++) {
      if (*p == '\n') { flush(); tokens.push_back({ "", false, false, true }); }
      else if (std::isspace((unsigned char) *p)) { flush(); }
      else word += *p;
    }
    flush();
    return tokens;
  }

  // Shared by Draw and MeasureHeight so they can never drift apart -- `emit` is called once per
  // visible token at its final (x, y) origin (relative to the control's own top-left), in the
  // style (regular/bold) it should render in. Two passes: the first lays every token out flush
  // left to find each line's word breaks and total width (needed before a line's own width is
  // known, since alignment other than Near shifts the whole line by an offset that depends on it),
  // the second re-emits every token with that offset added.
  template <typename F>
  static void Layout(IGraphics& g, const char* markupStr, const IText& regular, const IText& bold, float width, EAlign align, F&& emit)
  {
    struct Placed { const IText* style; std::string text; int line; float x; };
    const float lineH = regular.mSize * kLineHeightMult;
    const float spaceW = regular.mSize * kSpaceWidthEm;
    std::vector<Placed> placed;
    std::vector<float> lineWidths(1, 0.f);
    float x = 0.f;
    int line = 0;
    bool atLineStart = true;
    for (const Token& tok : Tokenize(markupStr)) {
      if (tok.newline) { x = 0.f; line++; lineWidths.push_back(0.f); atLineStart = true; continue; }
      const IText& style = tok.bold ? bold : regular;
      IRECT measured;
      const float w = g.MeasureText(style, tok.text.c_str(), measured);
      if (!atLineStart && x + w > width - kWrapSlackPx) { x = 0.f; line++; lineWidths.push_back(0.f); }
      placed.push_back({ &style, tok.text, line, x });
      x += w + (tok.sticky ? 0.f : spaceW);
      lineWidths.back() = x;
      atLineStart = false;
    }
    for (const Placed& p : placed) {
      const float lineOffset = align == EAlign::Center ? (width - lineWidths[p.line]) * 0.5f
                              : align == EAlign::Far    ? (width - lineWidths[p.line])
                              : 0.f;
      emit(*p.style, p.text.c_str(), p.x + lineOffset, p.line * lineH);
    }
  }

  std::string mMarkup;
  const char* mBoldFontID;
  EAlign mAlign;
};
