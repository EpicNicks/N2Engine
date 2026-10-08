# MikkTSpace

`include/mikktspace/mikktspace.h` and `mikktspace.c` are Morten S. Mikkelsen's MikkTSpace tangent space
generator, copied unmodified from https://github.com/mmikk/MikkTSpace at commit
`3e895b49d05ea07e4c2133156cfa94369e19e409` (the master branch, 2020-03-25). zlib licence: `LICENSE` here, and the
notice at the top of both files.

MikkTSpace is the tangent space that glTF specifies for normal maps whose file has no TANGENT attribute, and the
one Blender and most baking tools use. It is compiled once, as C, in
`assetimport/src/assetimport/MikktspaceImplementation.c`. Only
`assetimport/src/assetimport/TangentGeneration.cpp` includes the header: the `assetimport` library's public API
exposes no MikkTSpace type.
