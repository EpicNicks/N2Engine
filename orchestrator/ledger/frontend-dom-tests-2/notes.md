Accepted by the user 2026-10-13:
- store.test.ts had a class FakeApi (not a fakeStoreApi function): moved it plus FakeTimers, FakeHostEvents, log, settle, basenameOf unchanged and exported, and added a one-line fakeStoreApi() factory
- Host-location problem cases override api.host.location in the test rather than extending the fake
