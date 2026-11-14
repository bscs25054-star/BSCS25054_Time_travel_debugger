#pragma once
 
#include <string>
#include <vector>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
using namespace std;
 
// ---------------------------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------------------------
// These two MUST be identical in the server and the client (the client rebuilds Snapshot from the file).
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
 
const int IO_BUFFER_SIZE = 64 * 1024;       // fixed buffer for streaming files to and from a socket
const uint8_t STATUS_OK = 0;                // payload is the .tdbg file (or, for a request, the source file)
const uint8_t STATUS_ERROR = 1;             // payload is an error message
const int CHECKSUM_BYTES = 4;               // size of the CRC-32 trailer
 
const int MAX_QUEUED_CLIENTS = 8;           // load balancer: at most 8 clients may wait
const int CLIENT_TIMEOUT_SEC = 30;          // client gives up after 30 s of silence; LB drops clients that waited longer
const int BACKEND_CONNECT_RETRIES = 50;     // load balancer: 50 tries ...
const int BACKEND_RETRY_DELAY_MS = 100;     // ... 100 ms apart  (= about 5 seconds)
const int DEFAULT_LB_PORT = 5000;           // port the load balancer listens on unless you give a third argument
 
const int32_t TDBG_VERSION = 1;
const int64_t TDBG_HEADER_BYTES = 4 + 4 + 4 + 8; // magic + version + stepCount + indexOffset = 20 bytes on disk
 
// The exact error texts from the project's error table (so server, LB and client use the same words).
const char *const MSG_SERVER_BUSY = "server busy, try again later";
const char *const MSG_BACKEND_FAILED = "backend server failed to start";
const char *const MSG_UPLOAD_CORRUPT = "upload checksum mismatch";
const char *const MSG_RESPONSE_CORRUPT = "checksum mismatch, response corrupted";
const char *const MSG_BAD_TDBG = "server sent a damaged .tdbg file";
 

struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH]; 
    int32_t stackDepth;
};
// NOTE: a Snapshot is ~85 KB, so always create it with `new` (heap), never as a local variable.
struct TTDBHeader
{
    char magic[4]; 
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
 
// resolve.bin bookkeeping (server only for now, but the project asks for these to live in helper.h)
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};
 
// ---------------------------------------------------------------------------------------------
// Checksum: CRC-32 (the same polynomial zip/PNG use).  Streaming: call update() for every chunk.
// ---------------------------------------------------------------------------------------------
class CRC32
{
    uint32_t state;
 
    // 256-entry lookup table, built once. A function-local static is initialised thread-safely (C++11),
    // which matters because the load balancer has two threads.
    static const array<uint32_t, 256> &table()
    {
        static const array<uint32_t, 256> t = []
        {
            array<uint32_t, 256> tab{};
            for (uint32_t i = 0; i < 256; i++)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; k++){
                    c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                }
                tab[i] = c;
            }
            return tab;
        }();
        return t;
    }
 
public:
    CRC32() : state(0xFFFFFFFFu) {}
    void update(const void *data, size_t n)
    {
        const unsigned char *p = static_cast<const unsigned char *>(data);
        const array<uint32_t, 256> &t = table();
        for (size_t i = 0; i < n; i++)
            state = t[(state ^ p[i]) & 0xFF] ^ (state >> 8);
    }
    uint32_t value() const { return state ^ 0xFFFFFFFFu; }
};
 
// The checksum travels as 4 bytes, most significant byte first (big-endian), so every machine agrees.
inline void putU32BE(uint32_t v, unsigned char out[4])
{
    out[0] = (unsigned char)(v >> 24);
    out[1] = (unsigned char)(v >> 16);
    out[2] = (unsigned char)(v >> 8);
    out[3] = (unsigned char)(v);
}
inline uint32_t getU32BE(const unsigned char in[4])
{
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) | ((uint32_t)in[2] << 8) | (uint32_t)in[3];
}
 
// ---------------------------------------------------------------------------------------------
// TrailerChecker (receiver side).
// Problem: while bytes stream in, we cannot know which 4 bytes are the trailer until the stream ENDS.
// Solution: always HOLD BACK the last 4 bytes seen. Everything older is certainly message body, so it
// is released to the caller (and added to the CRC). When the stream ends, the 4 held bytes ARE the
// trailer: compare them with the CRC of everything released.
// ---------------------------------------------------------------------------------------------
class TrailerChecker
{
    CRC32 crc;
    unsigned char held[CHECKSUM_BYTES]; // the newest (up to) 4 bytes received so far
    int heldLen;
    uint64_t bodyBytes; // bytes released so far (status byte included)
 
public:
    TrailerChecker() : heldLen(0), bodyBytes(0) {}
 
    // Feed n freshly received bytes. The bytes now known to be body are written to `out`
    // (capacity must be at least n) and their count is returned. Status byte is part of the body.
    size_t feed(const char *inbytes, size_t n, char *outbytes)
    {
        size_t total = (size_t)heldLen + n; // conceptual stream: held bytes, then the new bytes
        if (total <= (size_t)CHECKSUM_BYTES)
        {
            for (size_t i = 0; i < n; i++)
                held[heldLen + i] = (unsigned char)inbytes[i];
            heldLen = (int)total;
            return 0;
        }
        size_t bodyLen = total - CHECKSUM_BYTES;
        auto at = [&](size_t i) -> unsigned char // byte number i of the conceptual stream
        { return i < (size_t)heldLen ? held[i] : (unsigned char)in[i - heldLen]; };
        for (size_t i = 0; i < bodyLen; i++)
            outbytes[i] = (char)at(i);
        unsigned char newHeld[CHECKSUM_BYTES];
        for (int j = 0; j < CHECKSUM_BYTES; j++)
            newHeld[j] = at(bodyLen + j);
        memcpy(held, newHeld, CHECKSUM_BYTES);
        heldLen = CHECKSUM_BYTES;
        crc.update(outbytes, bodyLen);
        bodyBytes += bodyLen;
        return bodyLen;
    }
    // Call when the stream has ended: true only if there is at least a status byte, a full trailer,
    // and the trailer equals the CRC of the body.
    bool verify() const
    {
        return heldLen == CHECKSUM_BYTES && bodyBytes >= 1 && getU32BE(held) == crc.value();
    }
    uint32_t crcValue() const { return crc.value(); } // CRC of the body released so far
    uint64_t bodySize() const { return bodyBytes; }
};
 
// ---------------------------------------------------------------------------------------------
// Socket helpers
// ---------------------------------------------------------------------------------------------
 
// send() may send fewer bytes than asked, so loop until everything is out. MSG_NOSIGNAL: a closed peer
// gives an error return instead of killing the program with SIGPIPE.
inline bool sendAll(int sock, const char *buf, size_t n)
{
    while (n > 0)
    {
        ssize_t k = send(sock, buf, n, MSG_NOSIGNAL);
        if (k < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (k == 0)
            return false;
        buf += k;
        n -= (size_t)k;
    }
    return true;
}
inline bool recvAll(int sock, char *buf, size_t n)
{
    while (n > 0)
    {
        ssize_t k = recv(sock, buf, n, 0);
        if (k < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (k == 0)
            return false;
        buf += k;
        n -= (size_t)k;
    }
    return true;
}
// Make recv()/send() give up (error EAGAIN) instead of waiting forever on a silent peer.
inline void setSocketTimeouts(int sock, int seconds)
{
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}
// Create a TCP socket and connect it to host:port. Returns the socket, or -1 on failure.
inline int connectToHost(const char *host, int port)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = nullptr;
    string portText = to_string(port);
    if (getaddrinfo(host, portText.c_str(), &hints, &res) != 0 || res == nullptr)
        return -1;
    int sock = -1;
    for (struct addrinfo *p = res; p != nullptr; p = p->ai_next)
    {
        sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock < 0)
            continue;
        if (connect(sock, p->ai_addr, p->ai_addrlen) == 0)
            break;
        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);
    return sock;
}
// Create a listening TCP socket on `port` (all interfaces). Returns the socket, or -1 on failure.
inline int createListenSocket(int port)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return -1;
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)); // allow quick restarts
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(sock, 16) != 0)
    {
        close(sock);
        return -1;
    }
    return sock;
}
// Send a complete error message: [STATUS_ERROR][text][CRC-32]. Used by server, LB (its own errors) alike.
inline bool sendErrorReply(int sock, const string &text)
{
    CRC32 crc;
    char status = (char)STATUS_ERROR;
    crc.update(&status, 1);
    crc.update(text.data(), text.size());
    unsigned char trailer[CHECKSUM_BYTES];
    putU32BE(crc.value(), trailer);
    return sendAll(sock, &status, 1) && sendAll(sock, text.data(), text.size()) &&
           sendAll(sock, (const char *)trailer, CHECKSUM_BYTES);
}
inline void installSignalHandlers(void (*handler)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);
}
 
inline void gracefulClose(int sock)
{
    shutdown(sock, SHUT_WR);
    setSocketTimeouts(sock, 1);
    char junk[4096];
    size_t drained = 0;
    while (drained < 256 * 1024)
    {
        ssize_t k = recv(sock, junk, sizeof(junk), 0);
        if (k <= 0)
            break;
        drained += (size_t)k;
    }
    close(sock);
}
 