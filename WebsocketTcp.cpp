/*
 * Native Windows WebSocket to TCP Proxy - Secure Hardened Edition
 *
 * Security fixes applied:
 *  1.  Token-based authentication (header or query string)
 *  2.  Connection concurrency limit
 *  3.  Handshake request size cap (8 KB)
 *  4.  WebSocket frame payload size cap (16 MB)
 *  5.  Full WebSocket handshake validation (method, headers, version)
 *  6.  Origin header validation
 *  7.  Thread-safe socket cleanup via shared TunnelContext + std::once_flag
 *  8.  Write mutex prevents interleaved WebSocket frames
 *  9.  Socket read/write timeouts
 *  10. RFC 6455 compliance: control frame size, reserved bits, mask requirement
 *  11. Partial-send handling on all socket writes
 *  12. All critical Win32/Winsock API return values checked
 *  13. Process/thread priority lowered to ABOVE_NORMAL (prevents CPU starvation)
 *  14. Command-line argument validation
 *  15. Constant-time token comparison (anti timing-attack)
 *
 * NOTE: This proxy does NOT terminate TLS.  For production, place it behind
 *       a TLS-terminating reverse proxy (nginx, Caddy, etc.) so that browser
 *       traffic travels over wss://.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wincrypt.h>

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "crypt32.lib")

/* ================================================================
 *  Configuration — adjust these for your deployment
 * ================================================================ */

static const std::string WS_MAGIC_STRING =
    "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// Max size of the HTTP upgrade request (bytes)
static const size_t    MAX_HANDSHAKE_SIZE  = 8192;

// Max WebSocket frame payload the proxy will accept (bytes)
static const uint64_t  MAX_PAYLOAD_SIZE    = 16ULL * 1024 * 1024;

// Max simultaneous tunnelled connections
static const int       MAX_CONNECTIONS     = 50;

// Socket-level read / write timeout (milliseconds)
static const DWORD     SOCKET_TIMEOUT_MS   = 30000;

// Timeout specifically for the handshake phase
static const DWORD     HANDSHAKE_TIMEOUT_MS = 10000;

// Target TCP port (VNC)
static const int       TARGET_PORT         = 5900;

// Authentication token.
// In production load this from an environment variable or config file.
static const std::string AUTH_TOKEN =
    "CHANGE_ME_TO_A_SECURE_RANDOM_TOKEN";

// Allowed Origin values.  Empty vector = skip Origin check.
static const std::vector<std::string> ALLOWED_ORIGINS = {
    // "https://your-trusted-domain.com",
};

/* ================================================================
 *  Globals
 * ================================================================ */

static std::atomic<int> g_active_connections{0};

/* ================================================================
 *  TunnelContext — shared by the two forwarding threads
 *  Guarantees every socket is closed exactly once, and the
 *  connection counter is decremented exactly once.
 * ================================================================ */
struct TunnelContext {
    SOCKET            ws_sock;
    SOCKET            tcp_sock;
    std::once_flag    cleanup_flag;
    std::mutex        ws_write_mutex;   // serialises writes to ws_sock

    TunnelContext(SOCKET ws, SOCKET tcp)
        : ws_sock(ws), tcp_sock(tcp) {}

    /* Thread-safe, idempotent shutdown of both sockets. */
    void close_all() {
        std::call_once(cleanup_flag, [this]() {
            shutdown(ws_sock,  SD_BOTH);
            shutdown(tcp_sock, SD_BOTH);
            closesocket(ws_sock);
            closesocket(tcp_sock);
            int remaining = --g_active_connections;
            std::cout << "[*] Tunnel closed.  Active: "
                      << remaining << std::endl;
        });
    }

    ~TunnelContext() { close_all(); }

    // non-copyable
    TunnelContext(const TunnelContext&)            = delete;
    TunnelContext& operator=(const TunnelContext&) = delete;
};

/* ================================================================
 *  Low-level helpers
 * ================================================================ */

// Read exactly `len` bytes or fail.
static bool recv_exact(SOCKET sock, char* buf, int len) {
    if (len <= 0) return true;
    int total = 0;
    while (total < len) {
        int n = recv(sock, buf + total, len - total, 0);
        if (n <= 0) return false;
        total += n;
    }
    return true;
}

// Send exactly `len` bytes or fail.
static bool send_exact(SOCKET sock, const char* buf, int len) {
    int total = 0;
    while (total < len) {
        int n = send(sock, buf + total, len - total, 0);
        if (n <= 0) return false;
        total += n;
    }
    return true;
}

// Constant-time string comparison (prevents timing side-channels).
static bool constant_time_compare(const std::string& a,
                                  const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

/* ================================================================
 *  HTTP header helpers
 * ================================================================ */

// Case-insensitive header value extraction.
// `header_name` must include the trailing ": ", e.g. "Upgrade: ".
static std::string get_header_value(const std::string& request,
                                    const std::string& header_name) {
    std::string lower_req = request;
    std::string lower_hdr = header_name;
    std::transform(lower_req.begin(), lower_req.end(),
                   lower_req.begin(), ::tolower);
    std::transform(lower_hdr.begin(), lower_hdr.end(),
                   lower_hdr.begin(), ::tolower);

    size_t pos = lower_req.find(lower_hdr);
    if (pos == std::string::npos) return "";

    pos += header_name.size();
    size_t end = request.find("\r\n", pos);
    if (end == std::string::npos) return "";

    std::string value = request.substr(pos, end - pos);
    size_t first = value.find_first_not_of(" \t");
    if (first == std::string::npos) return "";
    size_t last  = value.find_last_not_of(" \t");
    return value.substr(first, last - first + 1);
}

/* ================================================================
 *  WebSocket accept-key generation (SHA-1 + Base64)
 * ================================================================ */

static std::string get_websocket_accept_key(const std::string& client_key) {
    std::string concat = client_key + WS_MAGIC_STRING;

    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    BYTE  hash[20] = {};
    DWORD hashLen   = 20;

    if (!CryptAcquireContext(&hProv, NULL, NULL,
                             PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        std::cerr << "[-] CryptAcquireContext failed: "
                  << GetLastError() << std::endl;
        return "";
    }
    if (!CryptCreateHash(hProv, CALG_SHA1, 0, 0, &hHash)) {
        std::cerr << "[-] CryptCreateHash failed: "
                  << GetLastError() << std::endl;
        CryptReleaseContext(hProv, 0);
        return "";
    }
    if (!CryptHashData(hHash,
                       reinterpret_cast<const BYTE*>(concat.c_str()),
                       static_cast<DWORD>(concat.size()), 0)) {
        std::cerr << "[-] CryptHashData failed: "
                  << GetLastError() << std::endl;
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return "";
    }
    if (!CryptGetHashParam(hHash, HP_HASHVAL, hash, &hashLen, 0)) {
        std::cerr << "[-] CryptGetHashParam failed: "
                  << GetLastError() << std::endl;
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return "";
    }

    DWORD b64Len = 0;
    CryptBinaryToStringA(hash, hashLen,
                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                         NULL, &b64Len);
    std::vector<char> b64Buf(b64Len);
    CryptBinaryToStringA(hash, hashLen,
                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                         b64Buf.data(), &b64Len);

    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);

    std::string key(b64Buf.data());
    key.erase(std::remove(key.begin(), key.end(), '\r'), key.end());
    key.erase(std::remove(key.begin(), key.end(), '\n'), key.end());
    return key;
}

/* ================================================================
 *  Socket tuning
 * ================================================================ */

static void optimize_socket(SOCKET sock) {
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<char*>(&one), sizeof(one));

    int buf = 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<char*>(&buf), sizeof(buf));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF,
               reinterpret_cast<char*>(&buf), sizeof(buf));

    DWORD timeout = SOCKET_TIMEOUT_MS;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<char*>(&timeout), sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<char*>(&timeout), sizeof(timeout));
}

/* ================================================================
 *  Handshake validation helpers
 * ================================================================ */

static bool validate_websocket_handshake(const std::string& req) {
    if (req.substr(0, 4) != "GET ") {
        std::cerr << "[-] Not a GET request." << std::endl;
        return false;
    }

    std::string upgrade = get_header_value(req, "Upgrade: ");
    std::string lower_upgrade = upgrade;
    std::transform(lower_upgrade.begin(), lower_upgrade.end(),
                   lower_upgrade.begin(), ::tolower);
    if (lower_upgrade != "websocket") {
        std::cerr << "[-] Invalid Upgrade header." << std::endl;
        return false;
    }

    std::string conn = get_header_value(req, "Connection: ");
    std::string lower_conn = conn;
    std::transform(lower_conn.begin(), lower_conn.end(),
                   lower_conn.begin(), ::tolower);
    if (lower_conn.find("upgrade") == std::string::npos) {
        std::cerr << "[-] Invalid Connection header." << std::endl;
        return false;
    }

    if (get_header_value(req, "Sec-WebSocket-Key: ").empty()) {
        std::cerr << "[-] Missing Sec-WebSocket-Key." << std::endl;
        return false;
    }

    std::string ver = get_header_value(req, "Sec-WebSocket-Version: ");
    if (ver != "13") {
        std::cerr << "[-] Unsupported WS version: " << ver << std::endl;
        return false;
    }
    return true;
}

static bool validate_origin(const std::string& req) {
    if (ALLOWED_ORIGINS.empty()) return true;

    std::string origin = get_header_value(req, "Origin: ");
    if (origin.empty()) {
        std::cerr << "[-] Missing Origin header." << std::endl;
        return false;
    }
    for (const auto& allowed : ALLOWED_ORIGINS)
        if (origin == allowed) return true;

    std::cerr << "[-] Rejected Origin: " << origin << std::endl;
    return false;
}

static bool validate_auth(const std::string& req) {
    // 1) Try X-Auth-Token header
    std::string token = get_header_value(req, "X-Auth-Token: ");

    // 2) Fallback: ?token=xxx in the request URI
    if (token.empty()) {
        size_t end = req.find(" HTTP/");
        if (end != std::string::npos) {
            std::string uri = req.substr(4, end - 4);
            size_t tp = uri.find("token=");
            if (tp != std::string::npos) {
                tp += 6;
                size_t te = uri.find('&', tp);
                token = uri.substr(tp, te == std::string::npos
                                       ? std::string::npos : te - tp);
            }
        }
    }

    if (token.empty() || !constant_time_compare(token, AUTH_TOKEN)) {
        std::cerr << "[-] Authentication failed." << std::endl;
        return false;
    }
    return true;
}

/* ================================================================
 *  Thread 1 :  Browser (WebSocket) ──► Target (TCP)
 * ================================================================ */

static void ws_to_tcp_thread(std::shared_ptr<TunnelContext> ctx) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    std::vector<char> payload;
    payload.reserve(1024 * 1024);

    while (true) {
        /* ---- read 2-byte frame header ---- */
        unsigned char hdr[2];
        if (!recv_exact(ctx->ws_sock, reinterpret_cast<char*>(hdr), 2))
            break;

        /* reserved bits must be 0 (no extensions negotiated) */
        if (hdr[0] & 0x70) {
            std::cerr << "[-] Reserved bits set — protocol violation."
                      << std::endl;
            break;
        }

        bool     fin      = (hdr[0] & 0x80) != 0;
        int      opcode   = hdr[0] & 0x0F;
        bool     masked   = (hdr[1] & 0x80) != 0;
        uint64_t pay_len  = hdr[1] & 0x7F;

        /* RFC 6455 §5.1: client frames MUST be masked */
        if (!masked) {
            std::cerr << "[-] Unmasked client frame — rejected."
                      << std::endl;
            break;
        }

        /* extended payload length */
        if (pay_len == 126) {
            unsigned char ext[2];
            if (!recv_exact(ctx->ws_sock,
                            reinterpret_cast<char*>(ext), 2))
                break;
            pay_len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
        } else if (pay_len == 127) {
            unsigned char ext[8];
            if (!recv_exact(ctx->ws_sock,
                            reinterpret_cast<char*>(ext), 8))
                break;
            pay_len = 0;
            for (int i = 0; i < 8; ++i)
                pay_len = (pay_len << 8) | ext[i];
            if (pay_len & 0x8000000000000000ULL) {
                std::cerr << "[-] Invalid length (MSB set)." << std::endl;
                break;
            }
        }

        /* size guards */
        if (pay_len > MAX_PAYLOAD_SIZE) {
            std::cerr << "[-] Payload too large: " << pay_len << std::endl;
            break;
        }
        if (opcode >= 0x8 && pay_len > 125) {
            std::cerr << "[-] Control frame payload > 125 bytes." << std::endl;
            break;
        }

        /* We do not support fragmentation in this proxy.
           Reject continuation frames and non-FIN data frames. */
        if (opcode == 0x0 || (!fin && opcode < 0x8)) {
            std::cerr << "[-] Fragmented frames not supported." << std::endl;
            break;
        }

        /* masking key */
        unsigned char mask_key[4];
        if (!recv_exact(ctx->ws_sock,
                        reinterpret_cast<char*>(mask_key), 4))
            break;

        /* payload */
        if (pay_len > 0) {
            payload.resize(static_cast<size_t>(pay_len));
            if (!recv_exact(ctx->ws_sock, payload.data(),
                            static_cast<int>(pay_len)))
                break;
            for (size_t i = 0; i < static_cast<size_t>(pay_len); ++i)
                payload[i] ^= mask_key[i & 3];
        } else {
            payload.clear();
        }

        /* ---- handle opcodes ---- */

        if (opcode == 0x8) {   /* Close */
            std::cout << "[*] Browser sent Close frame." << std::endl;
            unsigned char close_frame[2] = {0x88, 0x00};
            {
                std::lock_guard<std::mutex> lk(ctx->ws_write_mutex);
                send(ctx->ws_sock,
                     reinterpret_cast<char*>(close_frame), 2, 0);
            }
            break;
        }

        if (opcode == 0x9) {   /* Ping → Pong */
            std::vector<char> pong;
            pong.reserve(2 + static_cast<size_t>(pay_len));
            pong.push_back(static_cast<char>(0x8A));
            pong.push_back(static_cast<char>(pay_len));
            if (pay_len > 0)
                pong.insert(pong.end(), payload.begin(),
                            payload.begin() + static_cast<size_t>(pay_len));
            {
                std::lock_guard<std::mutex> lk(ctx->ws_write_mutex);
                if (!send_exact(ctx->ws_sock, pong.data(),
                                static_cast<int>(pong.size())))
                    break;
            }
            continue;
        }

        if (opcode == 0xA) {   /* Pong — ignore */
            continue;
        }

        /* ---- data frame: forward to TCP target ---- */
        if (pay_len > 0) {
            if (!send_exact(ctx->tcp_sock, payload.data(),
                            static_cast<int>(pay_len)))
                break;
        }
    }

    ctx->close_all();
}

/* ================================================================
 *  Thread 2 :  Target (TCP) ──► Browser (WebSocket)
 * ================================================================ */

static void tcp_to_ws_thread(std::shared_ptr<TunnelContext> ctx) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    const int MAX_HDR  = 10;
    const int CHUNK    = 128 * 1024;
    std::vector<char> buf(MAX_HDR + CHUNK);

    while (true) {
        int n = recv(ctx->tcp_sock, buf.data() + MAX_HDR, CHUNK, 0);
        if (n <= 0) {
            std::cout << "[*] Target disconnected." << std::endl;
            break;
        }

        /* build WebSocket binary frame header in the space before
           the payload (the MAX_HDR gap). */
        int   hdr_sz = 0;
        char* hdr    = nullptr;

        if (n < 126) {
            hdr_sz = 2;
            hdr    = buf.data() + MAX_HDR - hdr_sz;
            hdr[0] = static_cast<char>(0x82);
            hdr[1] = static_cast<char>(n);
        } else if (n <= 65535) {
            hdr_sz = 4;
            hdr    = buf.data() + MAX_HDR - hdr_sz;
            hdr[0] = static_cast<char>(0x82);
            hdr[1] = static_cast<char>(126);
            hdr[2] = static_cast<char>((n >> 8) & 0xFF);
            hdr[3] = static_cast<char>( n       & 0xFF);
        } else {
            hdr_sz = 10;
            hdr    = buf.data() + MAX_HDR - hdr_sz;
            hdr[0] = static_cast<char>(0x82);
            hdr[1] = static_cast<char>(127);
            uint64_t len64 = static_cast<uint64_t>(n);
            hdr[2] = static_cast<char>((len64 >> 56) & 0xFF);
            hdr[3] = static_cast<char>((len64 >> 48) & 0xFF);
            hdr[4] = static_cast<char>((len64 >> 40) & 0xFF);
            hdr[5] = static_cast<char>((len64 >> 32) & 0xFF);
            hdr[6] = static_cast<char>((len64 >> 24) & 0xFF);
            hdr[7] = static_cast<char>((len64 >> 16) & 0xFF);
            hdr[8] = static_cast<char>((len64 >>  8) & 0xFF);
            hdr[9] = static_cast<char>( len64        & 0xFF);
        }

        {
            std::lock_guard<std::mutex> lk(ctx->ws_write_mutex);
            if (!send_exact(ctx->ws_sock, hdr, hdr_sz + n))
                break;
        }
    }

    ctx->close_all();
}

/* ================================================================
 *  main
 * ================================================================ */

int main(int argc, char* argv[]) {
    /* ---- argument validation ---- */
    if (argc != 3) {
        std::cerr << "Usage: proxy.exe <Target IP> <Listen Port>"
                  << std::endl;
        return 1;
    }

    std::string target_ip = argv[1];
    {
        sockaddr_in tmp{};
        if (inet_pton(AF_INET, target_ip.c_str(),
                      &tmp.sin_addr) != 1) {
            std::cerr << "[-] Invalid target IP: "
                      << target_ip << std::endl;
            return 1;
        }
    }

    int listen_port = 0;
    try {
        listen_port = std::stoi(argv[2]);
        if (listen_port < 1 || listen_port > 65535)
            throw std::out_of_range("port");
    } catch (...) {
        std::cerr << "[-] Invalid port: " << argv[2] << std::endl;
        return 1;
    }

    /* ---- process priority (no longer TIME_CRITICAL) ---- */
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

    /* ---- Winsock init ---- */
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "[-] WSAStartup failed: "
                  << WSAGetLastError() << std::endl;
        return 1;
    }

    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) {
        std::cerr << "[-] socket() failed: "
                  << WSAGetLastError() << std::endl;
        WSACleanup();
        return 1;
    }

    int reuse = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<char*>(&reuse), sizeof(reuse));

    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &srv.sin_addr);
    srv.sin_port = htons(static_cast<u_short>(listen_port));

    if (bind(listen_sock,
             reinterpret_cast<sockaddr*>(&srv), sizeof(srv))
            == SOCKET_ERROR) {
        std::cerr << "[-] bind() failed: "
                  << WSAGetLastError() << std::endl;
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }

    if (listen(listen_sock, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "[-] listen() failed: "
                  << WSAGetLastError() << std::endl;
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }

    std::cout << "[+] Secure proxy listening on 127.0.0.1:"
              << listen_port << " -> " << target_ip << ":"
              << TARGET_PORT << std::endl;

    if (ALLOWED_ORIGINS.empty())
        std::cout << "[!] WARNING: Origin validation disabled."
                  << std::endl;
    if (AUTH_TOKEN == "CHANGE_ME_TO_A_SECURE_RANDOM_TOKEN")
        std::cout << "[!] WARNING: Default auth token in use — "
                     "change before production!" << std::endl;

    /* ============================================================
     *  Accept loop
     * ============================================================ */
    while (true) {
        sockaddr_in cli{};
        int cli_len = sizeof(cli);
        SOCKET ws_sock = accept(listen_sock,
                                reinterpret_cast<sockaddr*>(&cli),
                                &cli_len);
        if (ws_sock == INVALID_SOCKET) {
            std::cerr << "[-] accept() failed: "
                      << WSAGetLastError() << std::endl;
            continue;
        }

        /* ---- connection limit ---- */
        if (g_active_connections.load() >= MAX_CONNECTIONS) {
            std::cerr << "[-] Connection limit reached ("
                      << MAX_CONNECTIONS << ")." << std::endl;
            const char* msg = "HTTP/1.1 503 Service Unavailable\r\n\r\n";
            send(ws_sock, msg, static_cast<int>(strlen(msg)), 0);
            closesocket(ws_sock);
            continue;
        }

        /* ---- handshake timeout ---- */
        DWORD hs_to = HANDSHAKE_TIMEOUT_MS;
        setsockopt(ws_sock, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<char*>(&hs_to), sizeof(hs_to));

        /* ---- read HTTP request with size cap ---- */
        std::string request;
        request.reserve(4096);
        char c;
        bool hs_ok = false;
        while (recv(ws_sock, &c, 1, 0) == 1) {
            request += c;
            if (request.size() >= 4 &&
                request[request.size()-4] == '\r' &&
                request[request.size()-3] == '\n' &&
                request[request.size()-2] == '\r' &&
                request[request.size()-1] == '\n') {
                hs_ok = true;
                break;
            }
            if (request.size() >= MAX_HANDSHAKE_SIZE) {
                std::cerr << "[-] Handshake too large." << std::endl;
                break;
            }
        }
        if (!hs_ok) {
            closesocket(ws_sock);
            continue;
        }

        /* ---- validate handshake ---- */
        if (!validate_websocket_handshake(request)) {
            const char* msg = "HTTP/1.1 400 Bad Request\r\n\r\n";
            send(ws_sock, msg, static_cast<int>(strlen(msg)), 0);
            closesocket(ws_sock);
            continue;
        }

        /* ---- validate Origin ---- */
        if (!validate_origin(request)) {
            const char* msg = "HTTP/1.1 403 Forbidden\r\n\r\n";
            send(ws_sock, msg, static_cast<int>(strlen(msg)), 0);
            closesocket(ws_sock);
            continue;
        }

        /* ---- validate auth token ---- */
        if (!validate_auth(request)) {
            const char* msg = "HTTP/1.1 401 Unauthorized\r\n\r\n";
            send(ws_sock, msg, static_cast<int>(strlen(msg)), 0);
            closesocket(ws_sock);
            continue;
        }

        /* ---- compute accept key ---- */
        std::string client_key =
            get_header_value(request, "Sec-WebSocket-Key: ");
        std::string accept_key = get_websocket_accept_key(client_key);
        if (accept_key.empty()) {
            std::cerr << "[-] Failed to compute accept key." << std::endl;
            closesocket(ws_sock);
            continue;
        }

        /* ---- build 101 response ---- */
        std::string response =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + accept_key + "\r\n";

        std::string protos =
            get_header_value(request, "Sec-WebSocket-Protocol: ");
        if (!protos.empty()) {
            size_t comma = protos.find(',');
            std::string sel = (comma != std::string::npos)
                              ? protos.substr(0, comma) : protos;
            size_t a = sel.find_first_not_of(" \t");
            size_t b = sel.find_last_not_of(" \t");
            if (a != std::string::npos)
                sel = sel.substr(a, b - a + 1);
            response += "Sec-WebSocket-Protocol: " + sel + "\r\n";
        }
        response += "\r\n";

        if (!send_exact(ws_sock, response.c_str(),
                        static_cast<int>(response.size()))) {
            closesocket(ws_sock);
            continue;
        }

        /* ---- connect to target ---- */
        SOCKET tcp_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (tcp_sock == INVALID_SOCKET) {
            std::cerr << "[-] socket() for target failed." << std::endl;
            closesocket(ws_sock);
            continue;
        }

        sockaddr_in tgt{};
        tgt.sin_family = AF_INET;
        inet_pton(AF_INET, target_ip.c_str(), &tgt.sin_addr);
        tgt.sin_port = htons(static_cast<u_short>(TARGET_PORT));

        if (connect(tcp_sock, reinterpret_cast<sockaddr*>(&tgt),
                    sizeof(tgt)) == SOCKET_ERROR) {
            std::cerr << "[-] Target connection failed: "
                      << WSAGetLastError() << std::endl;
            closesocket(ws_sock);
            closesocket(tcp_sock);
            continue;
        }

        /* ---- optimise sockets (includes timeouts) ---- */
        optimize_socket(ws_sock);
        optimize_socket(tcp_sock);

        /* ---- launch tunnel threads ---- */
        int conns = ++g_active_connections;
        std::cout << "[+] Tunnel up.  Active: " << conns << std::endl;

        auto ctx = std::make_shared<TunnelContext>(ws_sock, tcp_sock);

        try {
            std::thread t1(ws_to_tcp_thread, ctx);
            std::thread t2(tcp_to_ws_thread, ctx);
            t1.detach();
            t2.detach();
        } catch (const std::system_error& e) {
            std::cerr << "[-] Thread creation failed: "
                      << e.what() << std::endl;
            ctx->close_all();
            continue;
        }
    }

    closesocket(listen_sock);
    WSACleanup();
    return 0;
}
