// Minimal RFC 6455 WebSocket framing + HTTP upgrade helpers.
//
// Localhost-only; no TLS, no permessage-deflate, no fragmentation reassembly
// beyond a single fragmented binary message. Both endpoints are trusted Dusk
// processes, so we skip validation that a public WS server would need.

#pragma once

#ifdef DUSK_NETCOOP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dusk::netcoop::ws {

// Socket handle. On POSIX an int fd; on Windows a SOCKET (uintptr_t).
// Stored as intptr_t to keep this header free of <winsock2.h>.
using socket_t = intptr_t;

constexpr socket_t kInvalidSocket = -1;

// Process-level init/cleanup of the OS socket subsystem (no-op on POSIX,
// WSAStartup/WSACleanup on Windows). Safe to call multiple times.
void GlobalInit();
void GlobalShutdown();

// Returns errno (POSIX) or WSAGetLastError() (Windows) on the last call.
int LastError();

// Bind+listen on the first free port in [base, base+count). Returns the
// listening socket and writes the actual port into *out_port. On failure
// returns kInvalidSocket.
socket_t ListenLoopback(uint16_t base, uint16_t count, uint16_t* out_port);

// Non-blocking connect to 127.0.0.1:port. Returns kInvalidSocket on
// immediate failure (refused/timeout). Caller polls connect completion.
socket_t ConnectLoopback(uint16_t port);

// Single non-blocking accept attempt on the listening socket. Returns
// kInvalidSocket if no pending connection is ready (caller should retry).
// The returned socket is back in blocking mode.
socket_t AcceptNonBlocking(socket_t listen_sock);

// Closes a socket and sets *s = kInvalidSocket. Tolerant of already-invalid.
void Close(socket_t* s);

// Server-side handshake: read the HTTP upgrade, send the 101 response.
// Returns true on success. Blocking; do not call from the game thread.
bool ServerHandshake(socket_t s);

// Client-side handshake: send the upgrade, read the 101. Returns true on
// success. Blocking.
bool ClientHandshake(socket_t s, const char* host_header);

// Read one complete binary message into *out. Returns true on success.
// Returns false on close, error, or non-binary frame (we discard those).
// Blocking.
bool ReadBinaryMessage(socket_t s, std::vector<uint8_t>* out);

// Send a single unfragmented binary message. Returns true on full write.
// Blocking. On a localhost loopback this is effectively a memcpy.
bool SendBinaryMessage(socket_t s, const void* data, size_t len);

// Send an unmasked PING (server-side) or masked PING (client-side).
// Used for keepalive when the read loop hasn't seen traffic in a while.
bool SendPing(socket_t s, bool client_role);

}  // namespace dusk::netcoop::ws

#endif  // DUSK_NETCOOP
