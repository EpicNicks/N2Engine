# Third-party notices

N2Engine itself is MIT-licensed (see `LICENSE.md`). This file holds the notices that third-party
components require in a product's documentation. Vendored code under `external/` keeps its own licence
files, and the bundled default font's licence is `engine/assets/fonts/OFL.txt`.

## FreeType (optional)

Only builds configured with `-DN2ENGINE_TEXT_FREETYPE=ON` contain FreeType: CMake fetches
FreeType 2.13.3 and links it into the `text` library (see `docs/text.html#freetype`). FreeType is used
under the FreeType License (FTL), which requires this credit in the documentation of any product
that includes it:

> Portions of this software are copyright © 2024 The FreeType Project (www.freetype.org).
> All rights reserved.

The full licence is `docs/FTL.TXT` in the FreeType source, also at
<https://gitlab.freedesktop.org/freetype/freetype/-/blob/VER-2-13-3/docs/FTL.TXT>.
Builds with the option off (the default) contain no FreeType code and don't need this notice.
