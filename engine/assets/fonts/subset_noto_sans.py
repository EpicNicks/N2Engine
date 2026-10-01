"""Rebuilds NotoSans-Regular.ttf (see README.md) from the upstream release.

Usage: python subset_noto_sans.py <NotoSans/unhinted/ttf/NotoSans-Regular.ttf> <output.ttf>
Needs fontTools (pip install fonttools).

Keeps printable ASCII, Latin-1 Supplement and U+FFFD, and converts the GPOS pair kerning into a
legacy 'kern' table (format 0). stb_truetype reads GPOS only in some value formats, and FreeType's
FT_Get_Kerning reads only 'kern', so one 'kern' table gives every backend the same pairs.
"""
import os
import sys

from fontTools import subset
from fontTools.ttLib import TTFont, newTable
from fontTools.ttLib.tables._k_e_r_n import KernTable_format_0


def flatten_kerning(font):
    """GPOS 'kern' pair adjustments as {(left, right): xAdvance}, with OpenType's first-match rule."""
    gpos = font["GPOS"].table
    glyphs = font.getGlyphOrder()
    lookups = set()
    for record in gpos.FeatureList.FeatureRecord:
        if record.FeatureTag == "kern":
            lookups.update(record.Feature.LookupListIndex)

    pairs = {}
    for index in sorted(lookups):
        lookup = gpos.LookupList.Lookup[index]
        if lookup.LookupType != 2:
            raise SystemExit(f"unexpected kern lookup type {lookup.LookupType}")
        settled = set()  # pairs an earlier subtable of this lookup already applied to
        for table in lookup.SubTable:
            if table.Format == 1:
                for i, left in enumerate(table.Coverage.glyphs):
                    for record in table.PairSet[i].PairValueRecord:
                        key = (left, record.SecondGlyph)
                        if key in settled:
                            continue
                        settled.add(key)
                        value = getattr(record.Value1, "XAdvance", 0) if record.Value1 else 0
                        if value:
                            pairs[key] = pairs.get(key, 0) + value
            elif table.Format == 2:
                classes1 = table.ClassDef1.classDefs
                classes2 = table.ClassDef2.classDefs
                for left in table.Coverage.glyphs:
                    row = table.Class1Record[classes1.get(left, 0)]
                    for right in glyphs:
                        key = (left, right)
                        if key in settled:
                            continue
                        settled.add(key)
                        record = row.Class2Record[classes2.get(right, 0)]
                        value = getattr(record.Value1, "XAdvance", 0) if record.Value1 else 0
                        if value:
                            pairs[key] = pairs.get(key, 0) + value
    return pairs


def main(source, output):
    options = subset.Options()
    options.layout_features = ["kern"]
    options.notdef_outline = True
    options.name_IDs = ["*"]
    options.name_languages = ["*"]
    options.glyph_names = False
    options.hinting = False
    options.drop_tables += ["GSUB"]

    font = TTFont(source)
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=list(range(0x20, 0x7F)) + list(range(0xA0, 0x100)) + [0xFFFD])
    subsetter.subset(font)

    pairs = flatten_kerning(font)
    if len(pairs) >= 10920:
        raise SystemExit("too many pairs for one format 0 subtable")

    kern = newTable("kern")
    kern.version = 0
    table = KernTable_format_0()
    table.version = 0
    table.coverage = 1  # horizontal
    table.format = 0
    table.tupleIndex = None
    table.kernTable = pairs
    kern.kernTables = [table]
    font["kern"] = kern
    del font["GPOS"]
    if "GDEF" in font:
        del font["GDEF"]

    font.save(output)
    print(f"{output}: {os.path.getsize(output)} bytes, {len(font.getGlyphOrder())} glyphs, {len(pairs)} kerning pairs")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    main(sys.argv[1], sys.argv[2])
