Commits tests/golden/physics-determinism.json, the first golden of the Jolt determinism corpus (J7, #221).

Source: Tests run 39686335873 (pull_request, head 2620de5641f12d61d00b8cbf32a8408d18cf4d08), job "determinism (cross-leg physics hashes)" succeeded with "RESULT: all 5 legs are equal (no golden to compare with yet)". No master run after the J7 merge had finished when this was made, so the PR run was used. Artifact physics-determinism-golden-candidate (id 12388798280) was compared byte for byte with the output of `python3 scripts/physics-determinism.py golden` run on the linux-x86_64-clang leg artifact (id 12388265333): identical.

The file has 122 records: 10 scenes, state at every 60th tick (10 each), plus events (triggers_layers) 10, queries 10, snapshot_h1 and snapshot_h2 once each; ticks 600, interval 60, backend jolt.

Docs: the sentences in docs/physics.html and docs/testing.html that said the first run has no golden and the golden is to be committed now say that it is committed.

The first CI run on this PR is the check that the golden matches every leg: the determinism job compares all five legs with the golden, and it must be green.

Not verified: that every leg matches the golden (that is this PR's CI run); nothing builds locally.
