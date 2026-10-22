Accepted by the user 2026-10-13:
- all bridge changes are under mcp/ (where MCP1/MCP2 put the bridge), a path the brief's allowed-path list omits (it names src/mcp/, which does not exist); ratify will flag mcp/ files
- n2_play_script pauses a playing game before its ops and leaves it paused (Step needs a paused game; makes scripts repeatable)
- play budgets (20000 frames, 50 pictures) are counted per agent session like the write budget; each Step chunk, SetPaused, SendInput and the snapshot also spend host write commands
- resource reads return the read tool's text (summary + n2-data envelope) as text/plain, with n2/untrusted and n2/source in _meta
- subscriptions/listen is observed through a transport tap on the SDK stdio entry (serveBridge), since the SDK serves listen streams outside the server instance
- n2_playtest prompt is listed only when the play tools are (--launch)
- README.md: besides the new MCP3 section, the MCP intro sentence and 'Not there yet' were updated so they no longer say play/resources/prompts are missing
- review fix: n2://scene and n2://history are gated on n2_status (no dedicated read tool); a timed-out call's queue successor waits at most one more callTimeoutMs for the abandoned handler
