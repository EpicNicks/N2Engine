Accepted by the user 2026-10-13:
- Disconnect only forgets the peer locally and sends no packet (no transport-level framing), so ITransport's 'other side sees PeerDisconnected' does not hold for UDP; the other side relies on its idle timeout.
- Connect to a non-loopback address is refused by the same fail-closed rule as a non-loopback bind (the design names only the bind).
- UdpTransport reads std::chrono::steady_clock internally for the peer idle timeout (Connection and the simulator still take time as a parameter); docs say so.
- Only numeric bind/connect addresses are accepted (host names refused as InvalidAddress) so a name can never pass the loopback check.
- UdpTransport lives in the n2net library (compiled out under Emscripten, ws2_32 linked on Windows) rather than a separate target.
- Send returns Sent when the OS drops the datagram (full buffer or send error); the failure is visible in Stats(), per datagram-transport semantics.
