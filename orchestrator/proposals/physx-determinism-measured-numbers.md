# PhysX determinism: measured numbers (PR #161, head 09fc190)

Source: the `[DETERMINISM]` lines in the "PhysX determinism numbers" step of the CI jobs for commit
09fc190c18699515e04ee651d0fec17eefe04c6c: `linux (Release, x86_64, Clang)` (job 116702389752) and
`windows (Release, PhysX ON)` (job 116702389916). 9/9 tests passed on both; 10/10 checks green.
Setup: dt = 1/60 s, 600 steps (10 s). Unless stated, enhanced determinism on and 0 worker threads. A +1 ulp
`nextafter` nudge goes on the body's largest-magnitude initial coordinate. Distances are position only.
"bit-identical-steps" is the number of steps before the first step whose full state hash (position, rotation,
velocities) differs.

The pile is now the same scene on both platforms: each `lcg.Next()` draw is its own statement (commit 477d404),
and body 0 is perturbed on axis x from start -2.756168 on both Linux and Windows (body 50: axis y, start 4.298112
on both). The earlier run (head 23c347d) built different piles on MSVC and Clang (Windows body 0 was axis z), so
its Windows pile numbers came from a different scene and are superseded by the ones below. The Linux numbers are
unchanged from that run (same hashes), so Clang had already built the pile in the now-fixed order.
Free fall and box stack never used the generator and are unchanged from the earlier run on both platforms.

## 1-ulp perturbation

| Platform | Scene | Body | Axis | Start | bit-identical-steps | first >1 um | first >1 mm | first >1 cm | max divergence (m) | final hash |
|---|---|---|---|---|---|---|---|---|---|---|
| Linux | free fall (1 sphere) | 0 | y | 10.000000 | 0 | step 294 (4.90 s) | never | never | 0.000002 | b4f21a3b456267e3 |
| Windows | free fall (1 sphere) | 0 | y | 10.000000 | 0 | step 222 (3.70 s) | never | never | 0.000004 | 5d7c208f69932b57 |
| Linux | box stack (10) | 0 | y | 0.500000 | 0 | step 13 (0.22 s) | never | never | 0.000016 | 493e58c429cfaba3 |
| Windows | box stack (10) | 0 | y | 0.500000 | 0 | step 48 (0.80 s) | never | never | 0.000001 | 4ec4893bb17a767b |
| Linux | box stack (10) | 5 | y | 5.600000 | 0 | never | never | never | 0.000001 | 9bf55a62e2aaa1fd |
| Windows | box stack (10) | 5 | y | 5.600000 | 0 | step 13 (0.22 s) | never | never | 0.000015 | 67807114596424fb |
| Linux | pile (100) | 0 | x | -2.756168 | 0 | step 41 (0.68 s) | step 51 (0.85 s) | step 66 (1.10 s) | 12.429876 | 97a3dbe9ea4c2bac |
| Windows | pile (100) | 0 | x | -2.756168 | 0 | step 32 (0.53 s) | step 49 (0.82 s) | step 63 (1.05 s) | 8.885763 | 3540fc7916d5ea11 |
| Linux | pile (100) | 50 | y | 4.298112 | 0 | step 41 (0.68 s) | step 54 (0.90 s) | step 68 (1.13 s) | 4.271470 | 2251b508484f00c2 |
| Windows | pile (100) | 50 | y | 4.298112 | 0 | step 43 (0.72 s) | step 59 (0.98 s) | step 64 (1.07 s) | 6.809616 | 3617ade1e562c03e |

## Thread count, option off/on, final hashes

| Scene | Platform | Baseline final hash (on, 0 threads) | Trajectory hash | 0 vs 4 threads (flag on) | Option off: run vs run | Option off vs on | Off final hash |
|---|---|---|---|---|---|---|---|
| free fall | Linux | 584023f76efd653e | ef1b78b1a6ef2d16 | bit-identical | bit-identical | bit-identical | 584023f76efd653e |
| free fall | Windows | 9b334a56f4e89dc3 | d34c934d21d91cc0 | bit-identical | bit-identical | bit-identical | 9b334a56f4e89dc3 |
| box stack | Linux | 6e89c85453515d8a | 6b6329e105c6e127 | bit-identical | bit-identical | DIFFERENT | da64df2f64fb2046 |
| box stack | Windows | 4f170e60ab9b374b | 241fa5b8727471e0 | bit-identical | bit-identical | DIFFERENT | 8a29175d62279561 |
| pile | Linux | 32988b946794f033 | 89a29043fa5b09fe | bit-identical | bit-identical | DIFFERENT | ade862626ed72ce9 |
| pile | Windows | bf984f7445e5630f | 4cbc99369c6455d1 | bit-identical | bit-identical | DIFFERENT | 5288ea4633c06e59 |

Linux vs Windows final hashes: they differ for every scene, including the pile now that it is the same scene on
both, and including free fall and box stack (baseline, perturbed and option-off hashes all differ). Within one
platform, 0 vs 4 worker threads (flag on) and option-off run vs run were bit-identical for all three scenes.
1-ulp nudge: bit-identical-steps is 0 in every case (the full state hash differs from step 1).
