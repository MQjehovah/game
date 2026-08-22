#pragma once

// UDP socket abstraction over Winsock (Windows) / BSD sockets (POSIX).
//
// The socket is always non-blocking, so Recv() never blocks the caller: it
// returns Ok(0) ("empty") when no datagram is pending and Err only on real
// failures. v1 is a fixed single-peer model: the app calls SetPeer() with the
// destination address and Send() goes there; the server side can learn the
// client address from the first RecvFrom() and store it via SetPeer().
//
// Nothing here spawns threads. Winsock is started once (reference-counted) on
// the first UdpSocket::Create() and torn down when the last socket closes, so
// a loop-driven game that creates sockets for its lifetime never calls
// WSACleanup behind the app's back.

#include <cstddef>
#include <cstdint>
#include <string>

#include "neon/core/result.hpp"

namespace neon::net {

// An IPv4 endpoint. host is a dotted-quad address ("127.0.0.1"); DNS name
// resolution is deliberately out of scope for v1.
struct NetAddress {
    std::string host;
    uint16_t port = 0;

    bool Valid() const { return !host.empty() && port != 0; }
};

// One received datagram plus its sender address.
struct RecvPacket {
    size_t size = 0;       // bytes copied into the caller's buffer
    NetAddress from;       // sender; invalid when size == 0
};

class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();

    // Move-only: socket handles are not copyable.
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // Creates a non-blocking UDP socket bound to nothing. Calls EnsureNetStartup()
    // on Windows (WSAStartup once). Err on OS failure.
    static core::Result<UdpSocket> Create();

    // Binds to any local interface (or 127.0.0.1 via BindLoopback) on the given
    // port; port 0 asks the OS for an ephemeral port (query it via Port()).
    core::Status Bind(uint16_t port);
    core::Status BindLoopback(uint16_t port);

    // The single peer this socket sends to. Setting it is mandatory before Send().
    core::Status SetPeer(const NetAddress& addr);
    const NetAddress& Peer() const;

    // Bound local port (useful when Bind(0) was used). 0 when not bound.
    uint16_t Port() const;

    // Sends one datagram to the peer. Ok(len) on success; Ok(0) if the send was
    // deferred (should not happen for UDP); Err on failure or missing peer.
    core::Result<size_t> Send(const uint8_t* data, size_t len);

    // Receives one datagram, dropping the sender address. Ok(n) with n > 0 on
    // data, Ok(0) when nothing is pending, Err on a real socket error.
    core::Result<size_t> Recv(uint8_t* out, size_t cap);

    // Receives one datagram and reports its sender. Same empty/error semantics
    // as Recv(); on data the address is also recorded so a server can later
    // SetPeer() to reply to whoever just spoke.
    core::Result<RecvPacket> RecvFrom(uint8_t* out, size_t cap);

    void Close();
    bool Valid() const;

private:
    // SOCKET (Windows) or int fd (POSIX); both fit in intptr_t. kInvalid is -1
    // (INVALID_SOCKET is all-ones, which is -1 as a signed intptr_t on both
    // 32- and 64-bit Windows).
    intptr_t fd_ = -1;
    NetAddress peer_;
};

// Network subsystem lifecycle. On Windows this WSAStartup/WSACleanups with a
// reference count; on POSIX it is a no-op. Create() calls it automatically, so
// most callers never touch these.
core::Status EnsureNetStartup();
void ShutdownNet();

} // namespace neon::net
