Accepted by the user 2026-10-13:
- Review low (docs line 462) fixed: wording now states the inner Poll drain is bounded (maxDatagramsPerPoll for UDP)
- A pending (pre-auth) session whose final packet is queued now authenticates in the drain before the handshake-timeout check, so a late packet can save it; timeout values unchanged
- Test uses the SecurePair virtual clock (no real sleeps)
