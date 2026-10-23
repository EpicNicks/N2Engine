Accepted by the user 2026-10-13:
- Drive-letter protection removed entirely: a folder starting with '/' is POSIX, so 'k:/srv/game' and 'D:/srv/game' are now redacted (fixes review medium; earlier drive-protection test expectations flipped)
- Percent-decoding runs twice so %255C/%252F match (review low); matching runs twice per folder (single backslash, then doubled runs as one)
- docs/testing.html row also updated earlier (not named in the plan)
