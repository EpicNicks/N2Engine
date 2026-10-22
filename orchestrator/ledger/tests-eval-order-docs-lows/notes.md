Accepted by the user 2026-10-13:
- Also split two extra multi-call expressions found by the grep: EditorTlsTests std::random_device random()+random() and PbdParticleBackendTests EXPECT_NE(NextParticleBackendGeneration() x2)
- N2NetFuzzTests Mutate packet[NextBelow] ^= NextBelow was already C++17-sequenced; split anyway in the same (right-first) order
- Ternaries with draws in condition and branches and a.Next()!=b.Next() on two separate generators left unchanged (sequenced / independent)
- PR #206 badge put inline in the Stage 1 verified sentence; no design-decision callout added (nothing to decide)
