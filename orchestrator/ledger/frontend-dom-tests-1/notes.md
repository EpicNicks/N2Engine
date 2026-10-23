# Notes (frontend-dom-tests-1, 2026-10-13)
Approved deviations (accepted by the user):
- Added @types/jsdom devDependency (setup typing needed it)
- tsconfig.json edited to exclude src/test-dom from the Node build (tsc -p tsconfig.json otherwise compiled it)
- setup split into dom-globals.ts + setup.ts because Testing Library reads global document at import, so the window must exist first
- Did not extract fakeStoreApi (tests use a stub store instead)
- bundle-tests.js runs as a separate step in npm test, not in npm run build
- The repo has CI (.github/workflows/ci.yml, Node 22, ubuntu+windows); not edited, works unchanged with engines, bundle-tests step and test script
- npm audit: 7 (4 moderate, 3 high, 0 critical), identical before and after, all in existing electron tree
- Transitive dev licences are permissive but not all MIT (MIT-0, ISC, BSD, Apache-2.0, BlueOak, CC0)
- Review fix: @testing-library/dom pinned to ^8.20.1 (deduped, user-event now shares preact act wrappers); engines.node >=22.22.2 + README
- Review fix: npm test runs dist/test-dom in a separate node --test after dist/test; README restored to CRLF (diff vs master is 5+/1- lines)
Follow-ups: PR 2 (store-driven welcome behaviour, shared fakeStoreApi), PR 3 (assets/hierarchy); the 7 existing npm audit findings (3 high) are in the electron tree and unchanged.
Unverified: viewport panel and drag and drop (out of scope)
