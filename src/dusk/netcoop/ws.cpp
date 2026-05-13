#ifdef DUSK_NETCOOP

#include "ws.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using sock_native_t = SOCKET;
#  define WS_SOCK_INVALID INVALID_SOCKET
#  define WS_SOCK_ERR     SOCKET_ERROR
#  define WS_LAST_ERR()   WSAGetLastError()
#  define WS_EINPROGRESS  WSAEWOULDBLOCK
#  define WS_EAGAIN       WSAEWOULDBLOCK
   typedef int socklen_t_compat;
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <unistd.h>
   using sock_native_t = int;
#  define WS_SOCK_INVALID (-1)
#  define WS_SOCK_ERR    (-1)
#  define WS_LAST_ERR()  errno
#  define WS_EINPROGRESS EINPROGRESS
#  define WS_EAGAIN      EAGAIN
   typedef socklen_t socklen_t_compat;
#endif

namespace dusk::netcoop::ws {

namespace {

std::atomic<int> g_init_count{0};

inline sock_native_t to_native(socket_t s) { return static_cast<sock_native_t>(s); }
inline socket_t     to_handle(sock_native_t s) { return static_cast<socket_t>(s); }

// SHA-1 — we need exactly one hash for the Sec-WebSocket-Accept value.
// 100% RFC 3174 textbook impl; do not reuse for anything that matters.
void Sha1(const uint8_t* msg, size_t len, uint8_t out[20]) {
    auto rotl = [](uint32_t x, int n) -> uint32_t {
        return (x << n) | (x >> (32 - n));
    };
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    uint64_t bitlen = static_cast<uint64_t>(len) * 8;

    std::vector<uint8_t> buf(msg, msg + len);
    buf.push_back(0x80);
    while (buf.size() % 64 != 56) buf.push_back(0);
    for (int i = 7; i >= 0; --i) buf.push_back(static_cast<uint8_t>(bitlen >> (i * 8)));

    for (size_t i = 0; i < buf.size(); i += 64) {
        uint32_t w[80];
        for (int t = 0; t < 16; ++t) {
            w[t] = (uint32_t(buf[i + 4 * t]) << 24) | (uint32_t(buf[i + 4 * t + 1]) << 16) |
                   (uint32_t(buf[i + 4 * t + 2]) << 8) | uint32_t(buf[i + 4 * t + 3]);
        }
        for (int t = 16; t < 80; ++t) {
            w[t] = rotl(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int t = 0; t < 80; ++t) {
            uint32_t f, k;
            if (t < 20)      { f = (b & c) | ((~b) & d);         k = 0x5A827999u; }
            else if (t < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
            else if (t < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }
            uint32_t temp = rotl(a, 5) + f + e + k + w[t];
            e = d; d = c; c = rotl(b, 30); b = a; a = temp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; ++i) {
        out[4 * i]     = uint8_t(h[i] >> 24);
        out[4 * i + 1] = uint8_t(h[i] >> 16);
        out[4 * i + 2] = uint8_t(h[i] >> 8);
        out[4 * i + 3] = uint8_t(h[i]);
    }
}

std::string Base64(const uint8_t* data, size_t len) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = uint32_t(data[i]) << 16;
        if (i + 1 < len) v |= uint32_t(data[i + 1]) << 8;
        if (i + 2 < len) v |= uint32_t(data[i + 2]);
        out.push_back(tbl[(v >> 18) & 0x3F]);
        out.push_back(tbl[(v >> 12) & 0x3F]);
        out.push_back(i + 1 < len ? tbl[(v >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < len ? tbl[v & 0x3F]       : '=');
    }
    return out;
}

bool ReadAll(socket_t s, void* buf, size_t len) {
    auto* p = static_cast<uint8_t*>(buf);
    size_t got = 0;
    while (got < len) {
#ifdef _WIN32
        int n = recv(to_native(s), reinterpret_cast<char*>(p + got), int(len - got), 0);
#else
        ssize_t n = recv(to_native(s), p + got, len - got, 0);
#endif
        if (n <= 0) return false;
        got += size_t(n);
    }
    return true;
}

bool WriteAll(socket_t s, const void* buf, size_t len) {
    auto* p = static_cast<const uint8_t*>(buf);
    size_t sent = 0;
    while (sent < len) {
#ifdef _WIN32
        int n = send(to_native(s), reinterpret_cast<const char*>(p + sent), int(len - sent), 0);
#else
        ssize_t n = send(to_native(s), p + sent, len - sent, 0);
#endif
        if (n <= 0) return false;
        sent += size_t(n);
    }
    return true;
}

// Best-effort read of an HTTP header section terminated by \r\n\r\n.
// Caps at 8 KiB; for localhost peers any larger header is malformed.
bool ReadHttpHeader(socket_t s, std::string* out) {
    out->clear();
    out->reserve(512);
    char c;
    while (out->size() < 8192) {
#ifdef _WIN32
        int n = recv(to_native(s), &c, 1, 0);
#else
        ssize_t n = recv(to_native(s), &c, 1, 0);
#endif
        if (n <= 0) return false;
        out->push_back(c);
        if (out->size() >= 4 &&
            out->compare(out->size() - 4, 4, "\r\n\r\n") == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

void GlobalInit() {
    if (g_init_count.fetch_add(1) == 0) {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    }
}

void GlobalShutdown() {
    if (g_init_count.fetch_sub(1) == 1) {
#ifdef _WIN32
        WSACleanup();
#endif
    }
}

int LastError() { return WS_LAST_ERR(); }

socket_t ListenLoopback(uint16_t base, uint16_t count, uint16_t* out_port) {
    sock_native_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == WS_SOCK_INVALID) return kInvalidSocket;

    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&one), sizeof(one));

    for (uint16_t i = 0; i < count; ++i) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(uint16_t(base + i));
        if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            if (listen(s, 1) == 0) {
                if (out_port) *out_port = uint16_t(base + i);
                return to_handle(s);
            }
        }
    }
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
    return kInvalidSocket;
}

socket_t ConnectLoopback(uint16_t port) {
    sock_native_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == WS_SOCK_INVALID) return kInvalidSocket;

    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&one), sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
        return kInvalidSocket;
    }
    return to_handle(s);
}

socket_t AcceptNonBlocking(socket_t listen_sock) {
    sock_native_t ls = to_native(listen_sock);
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(ls, FIONBIO, &nb);
    SOCKET incoming = accept(ls, nullptr, nullptr);
    u_long blocking = 0;
    ioctlsocket(ls, FIONBIO, &blocking);
    if (incoming == INVALID_SOCKET) return kInvalidSocket;
    ioctlsocket(incoming, FIONBIO, &blocking);
    return to_handle(incoming);
#else
    int flags = fcntl(ls, F_GETFL, 0);
    fcntl(ls, F_SETFL, flags | O_NONBLOCK);
    int incoming = accept(ls, nullptr, nullptr);
    int saved_err = errno;
    fcntl(ls, F_SETFL, flags);
    if (incoming < 0) { errno = saved_err; return kInvalidSocket; }
    int iflags = fcntl(incoming, F_GETFL, 0);
    fcntl(incoming, F_SETFL, iflags & ~O_NONBLOCK);
    return to_handle(incoming);
#endif
}

void Close(socket_t* s) {
    if (!s || *s == kInvalidSocket) return;
#ifdef _WIN32
    closesocket(to_native(*s));
#else
    close(to_native(*s));
#endif
    *s = kInvalidSocket;
}

bool ServerHandshake(socket_t s) {
    std::string req;
    if (!ReadHttpHeader(s, &req)) return false;

    // Find Sec-WebSocket-Key (case-sensitive header name per RFC).
    const char* k = "Sec-WebSocket-Key:";
    auto pos = req.find(k);
    if (pos == std::string::npos) return false;
    pos += std::strlen(k);
    while (pos < req.size() && (req[pos] == ' ' || req[pos] == '\t')) ++pos;
    auto end = req.find("\r\n", pos);
    if (end == std::string::npos) return false;
    std::string key = req.substr(pos, end - pos);

    // Accept = base64(sha1(key + magic))
    static const char magic[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string joined = key + magic;
    uint8_t hash[20];
    Sha1(reinterpret_cast<const uint8_t*>(joined.data()), joined.size(), hash);
    std::string accept = Base64(hash, 20);

    char resp[256];
    int n = std::snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n",
        accept.c_str());
    return n > 0 && WriteAll(s, resp, size_t(n));
}

bool ClientHandshake(socket_t s, const char* host_header) {
    // Generate 16 random bytes for the key.
    uint8_t key_bytes[16];
    std::random_device rd;
    for (auto& b : key_bytes) b = uint8_t(rd());
    std::string key = Base64(key_bytes, 16);

    char req[512];
    int n = std::snprintf(req, sizeof(req),
        "GET /dusk HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n",
        host_header, key.c_str());
    if (n <= 0 || !WriteAll(s, req, size_t(n))) return false;

    std::string resp;
    if (!ReadHttpHeader(s, &resp)) return false;
    return resp.compare(0, 12, "HTTP/1.1 101") == 0;
}

bool ReadBinaryMessage(socket_t s, std::vector<uint8_t>* out) {
    out->clear();
    bool first_frame = true;
    while (true) {
        uint8_t hdr[2];
        if (!ReadAll(s, hdr, 2)) return false;
        bool fin    = (hdr[0] & 0x80) != 0;
        uint8_t op  = hdr[0] & 0x0F;
        bool masked = (hdr[1] & 0x80) != 0;
        uint64_t plen = hdr[1] & 0x7F;
        if (plen == 126) {
            uint8_t ext[2];
            if (!ReadAll(s, ext, 2)) return false;
            plen = (uint64_t(ext[0]) << 8) | ext[1];
        } else if (plen == 127) {
            uint8_t ext[8];
            if (!ReadAll(s, ext, 8)) return false;
            plen = 0;
            for (int i = 0; i < 8; ++i) plen = (plen << 8) | ext[i];
        }

        uint8_t mask[4]{};
        if (masked && !ReadAll(s, mask, 4)) return false;

        size_t off = out->size();
        out->resize(off + size_t(plen));
        if (plen > 0 && !ReadAll(s, out->data() + off, size_t(plen))) return false;
        if (masked) {
            for (size_t i = 0; i < size_t(plen); ++i) {
                (*out)[off + i] ^= mask[i & 3];
            }
        }

        if (op == 0x8) return false;             // close
        if (op == 0x9 || op == 0xA) {            // ping/pong — ignore payload, keep reading
            out->resize(off);
            continue;
        }
        if (first_frame && op != 0x2) {          // we only accept binary
            out->resize(off);
            // Drain non-binary; keep loop going.
            if (fin) { /* discard */ } else continue;
            out->clear();
            return false;
        }
        first_frame = false;
        if (fin) return true;
    }
}

bool SendBinaryMessage(socket_t s, const void* data, size_t len) {
    uint8_t hdr[10];
    size_t hlen = 0;
    hdr[0] = 0x82;  // FIN | opcode=binary
    if (len < 126) {
        hdr[1] = uint8_t(len);
        hlen = 2;
    } else if (len < 65536) {
        hdr[1] = 126;
        hdr[2] = uint8_t(len >> 8);
        hdr[3] = uint8_t(len);
        hlen = 4;
    } else {
        hdr[1] = 127;
        for (int i = 0; i < 8; ++i) hdr[2 + i] = uint8_t(len >> ((7 - i) * 8));
        hlen = 10;
    }
    if (!WriteAll(s, hdr, hlen)) return false;
    return len == 0 || WriteAll(s, data, len);
}

bool SendPing(socket_t s, bool client_role) {
    uint8_t hdr[6];
    hdr[0] = 0x89;  // FIN | opcode=ping
    hdr[1] = uint8_t(client_role ? 0x80 : 0x00);
    size_t hlen = 2;
    if (client_role) {
        hdr[2] = hdr[3] = hdr[4] = hdr[5] = 0;
        hlen = 6;
    }
    return WriteAll(s, hdr, hlen);
}

}  // namespace dusk::netcoop::ws

#endif  // DUSK_NETCOOP
