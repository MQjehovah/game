#include "neon/net/socket.hpp"

#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace neon::net {
namespace {

// Winsock lifecycle. Single-threaded assumption: the reliable layer is driven
// synchronously by the game loop, so no lock is needed around the refcount.
struct WsaState {
    int refs = 0;
    bool ok = false;
};

WsaState& Wsa() {
    static WsaState state;
    return state;
}

int LastError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

// Errors that mean "nothing there yet" (or a stale ICMP rejection) rather than
// a hard failure; these map to Ok(0).
bool WouldBlock(int err) {
#ifdef _WIN32
    return err == WSAEWOULDBLOCK || err == WSAECONNRESET;
#else
    return err == EAGAIN || err == EWOULDBLOCK || err == ECONNREFUSED;
#endif
}

bool IsInvalid(intptr_t fd) {
#ifdef _WIN32
    return static_cast<SOCKET>(fd) == INVALID_SOCKET;
#else
    return static_cast<int>(fd) < 0;
#endif
}

core::Status BindImpl(intptr_t fd, const char* host, uint16_t port) {
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    unsigned long raw = ::inet_addr(host);
    if (raw == INADDR_NONE)
        return core::Status::Err("net: bad bind address " + std::string(host));
    sin.sin_addr.s_addr = raw;
    sin.sin_port = htons(port);
#ifdef _WIN32
    int rc = ::bind(static_cast<SOCKET>(fd), reinterpret_cast<const sockaddr*>(&sin),
                    static_cast<int>(sizeof(sin)));
#else
    int rc = ::bind(static_cast<int>(fd), reinterpret_cast<const sockaddr*>(&sin),
                    sizeof(sin));
#endif
    if (rc != 0)
        return core::Status::Err("net: bind failed (port " + std::to_string(port) + ")");
    return core::Status::Ok(true);
}

} // namespace

core::Status EnsureNetStartup() {
#ifdef _WIN32
    if (Wsa().ok) {
        ++Wsa().refs;
        return core::Status::Ok(true);
    }
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        return core::Status::Err("net: WSAStartup failed");
    Wsa().ok = true;
    Wsa().refs = 1;
#else
    (void)0;
#endif
    return core::Status::Ok(true);
}

void ShutdownNet() {
#ifdef _WIN32
    if (Wsa().ok && --Wsa().refs <= 0) {
        WSACleanup();
        Wsa().ok = false;
        Wsa().refs = 0;
    }
#else
    (void)0;
#endif
}

UdpSocket::UdpSocket() = default;

UdpSocket::~UdpSocket() { Close(); }

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : fd_(other.fd_), peer_(std::move(other.peer_)) {
    other.fd_ = -1;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
        Close();
        fd_ = other.fd_;
        peer_ = std::move(other.peer_);
        other.fd_ = -1;
    }
    return *this;
}

core::Result<UdpSocket> UdpSocket::Create() {
    core::Status init = EnsureNetStartup();
    if (!init.Ok()) return core::Result<UdpSocket>::Err(init.Error());

    intptr_t fd = -1;
#ifdef _WIN32
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        ShutdownNet();
        return core::Result<UdpSocket>::Err("net: socket() failed");
    }
    u_long mode = 1; // FIONBIO: non-blocking
    if (::ioctlsocket(s, FIONBIO, &mode) != 0) {
        ::closesocket(s);
        ShutdownNet();
        return core::Result<UdpSocket>::Err("net: ioctlsocket(FIONBIO) failed");
    }
    fd = static_cast<intptr_t>(s);
#else
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        ShutdownNet();
        return core::Result<UdpSocket>::Err("net: socket() failed");
    }
    int flags = ::fcntl(s, F_GETFL, 0);
    if (flags < 0 || ::fcntl(s, F_SETFL, flags | O_NONBLOCK) != 0) {
        ::close(s);
        ShutdownNet();
        return core::Result<UdpSocket>::Err("net: fcntl(O_NONBLOCK) failed");
    }
    fd = static_cast<intptr_t>(s);
#endif

    UdpSocket sock;
    sock.fd_ = fd;
    return core::Result<UdpSocket>::Ok(std::move(sock));
}

core::Status UdpSocket::Bind(uint16_t port) {
    if (IsInvalid(fd_)) return core::Status::Err("net: bind on closed socket");
    return BindImpl(fd_, "0.0.0.0", port);
}

core::Status UdpSocket::BindLoopback(uint16_t port) {
    if (IsInvalid(fd_)) return core::Status::Err("net: bind on closed socket");
    return BindImpl(fd_, "127.0.0.1", port);
}

core::Status UdpSocket::SetPeer(const NetAddress& addr) {
    if (IsInvalid(fd_)) return core::Status::Err("net: SetPeer on closed socket");
    if (!addr.Valid()) return core::Status::Err("net: SetPeer requires host + port");
    peer_ = addr;
    return core::Status::Ok(true);
}

const NetAddress& UdpSocket::Peer() const { return peer_; }

uint16_t UdpSocket::Port() const {
    if (IsInvalid(fd_)) return 0;
    sockaddr_in sin{};
#ifdef _WIN32
    int len = static_cast<int>(sizeof(sin));
    if (::getsockname(static_cast<SOCKET>(fd_), reinterpret_cast<sockaddr*>(&sin), &len) != 0)
        return 0;
#else
    socklen_t len = sizeof(sin);
    if (::getsockname(static_cast<int>(fd_), reinterpret_cast<sockaddr*>(&sin), &len) != 0)
        return 0;
#endif
    return static_cast<uint16_t>(ntohs(sin.sin_port));
}

core::Result<size_t> UdpSocket::Send(const uint8_t* data, size_t len) {
    if (IsInvalid(fd_)) return core::Result<size_t>::Err("net: send on closed socket");
    if (!peer_.Valid())
        return core::Result<size_t>::Err("net: send before SetPeer");

    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    unsigned long raw = ::inet_addr(peer_.host.c_str());
    if (raw == INADDR_NONE)
        return core::Result<size_t>::Err("net: bad peer address '" + peer_.host + "'");
    sin.sin_addr.s_addr = raw;
    sin.sin_port = htons(peer_.port);

#ifdef _WIN32
    int rc = ::sendto(static_cast<SOCKET>(fd_),
                      reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
                      reinterpret_cast<const sockaddr*>(&sin), static_cast<int>(sizeof(sin)));
#else
    ssize_t rc = ::sendto(static_cast<int>(fd_), data, len, 0,
                          reinterpret_cast<const sockaddr*>(&sin), sizeof(sin));
#endif
    if (rc < 0) {
        int err = LastError();
        if (WouldBlock(err)) return core::Result<size_t>::Ok(0);
        return core::Result<size_t>::Err("net: sendto failed");
    }
    return core::Result<size_t>::Ok(static_cast<size_t>(rc));
}

core::Result<size_t> UdpSocket::Recv(uint8_t* out, size_t cap) {
    if (IsInvalid(fd_)) return core::Result<size_t>::Err("net: recv on closed socket");
    if (out == nullptr || cap == 0) return core::Result<size_t>::Err("net: recv bad buffer");

    sockaddr_in sin{};
#ifdef _WIN32
    int addrLen = static_cast<int>(sizeof(sin));
    int rc = ::recvfrom(static_cast<SOCKET>(fd_), reinterpret_cast<char*>(out),
                        static_cast<int>(cap), 0,
                        reinterpret_cast<sockaddr*>(&sin), &addrLen);
#else
    socklen_t addrLen = sizeof(sin);
    ssize_t rc = ::recvfrom(static_cast<int>(fd_), out, cap, 0,
                            reinterpret_cast<sockaddr*>(&sin), &addrLen);
#endif
    if (rc < 0) {
        if (WouldBlock(LastError())) return core::Result<size_t>::Ok(0);
        return core::Result<size_t>::Err("net: recvfrom failed");
    }
    return core::Result<size_t>::Ok(static_cast<size_t>(rc));
}

core::Result<RecvPacket> UdpSocket::RecvFrom(uint8_t* out, size_t cap) {
    if (IsInvalid(fd_)) return core::Result<RecvPacket>::Err("net: recv on closed socket");
    if (out == nullptr || cap == 0)
        return core::Result<RecvPacket>::Err("net: recv bad buffer");

    sockaddr_in sin{};
#ifdef _WIN32
    int addrLen = static_cast<int>(sizeof(sin));
    int rc = ::recvfrom(static_cast<SOCKET>(fd_), reinterpret_cast<char*>(out),
                        static_cast<int>(cap), 0,
                        reinterpret_cast<sockaddr*>(&sin), &addrLen);
#else
    socklen_t addrLen = sizeof(sin);
    ssize_t rc = ::recvfrom(static_cast<int>(fd_), out, cap, 0,
                            reinterpret_cast<sockaddr*>(&sin), &addrLen);
#endif
    if (rc < 0) {
        if (WouldBlock(LastError())) return core::Result<RecvPacket>::Ok(RecvPacket{0, NetAddress{}});
        return core::Result<RecvPacket>::Err("net: recvfrom failed");
    }

    RecvPacket pkt;
    pkt.size = static_cast<size_t>(rc);
    pkt.from.host = ::inet_ntoa(sin.sin_addr);
    pkt.from.port = static_cast<uint16_t>(ntohs(sin.sin_port));
    return core::Result<RecvPacket>::Ok(std::move(pkt));
}

void UdpSocket::Close() {
    if (IsInvalid(fd_)) return;
#ifdef _WIN32
    ::closesocket(static_cast<SOCKET>(fd_));
    ShutdownNet();
#else
    ::close(static_cast<int>(fd_));
#endif
    fd_ = -1;
}

bool UdpSocket::Valid() const { return !IsInvalid(fd_); }

} // namespace neon::net
