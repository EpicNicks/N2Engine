# Default font

`NotoSans-Regular.ttf` is the engine's built-in font: what `Text::Font::GetDefault()` returns, and the
font the text tests run against. The build embeds it in the `text` library (see
`text/cmake/EmbedBinary.cmake`), so programs don't need this file at run time.

- **Font:** Noto Sans Regular, version 2.015, unhinted TrueType.
- **Source:** `NotoSans/unhinted/ttf/NotoSans-Regular.ttf` from the `NotoSans-v2.015` release of
  https://github.com/notofonts/latin-greek-cyrillic
  (https://github.com/notofonts/latin-greek-cyrillic/releases/download/NotoSans-v2.015/NotoSans-v2.015.zip).
- **Licence:** SIL Open Font License 1.1, in `OFL.txt` (copied from the same release). The licence
  declares no Reserved Font Name.
- **Changes:** subset to printable ASCII (U+0020-U+007E), Latin-1 Supplement (U+00A0-U+00FF) and
  U+FFFD, with hinting removed and the GPOS pair kerning converted to a legacy `kern` table, so every
  font backend reads the same kerning. `subset_noto_sans.py` reproduces it (fontTools 4.66.1). The
  result is about 23 KB.
