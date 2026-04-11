/*
 * Native Windows WebSocket to TCP Proxy - SPEED TEST EDITION
 * * Features:
 * - 10-second timeout bug FIXED
 * - Real-time throughput speedometer with Task Manager style Area Chart
 * - Modern Windows 11/10 UI with Visual Styles enabled
 * - Minimize & Close to System Tray support
 * - Clean English UI
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wincrypt.h>
#include <shellapi.h>

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <cstring>
#include <chrono>
#include <iomanip>

 // Enable Modern Windows Visual Styles (Buttons, Edits, etc.)
#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#pragma warning(disable: 4819)
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(linker, "/SUBSYSTEM:windows")

// --- Global Constants ---
static const std::string WS_MAGIC_STRING = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
static const size_t    MAX_HANDSHAKE_SIZE = 8192;
static const uint64_t  MAX_PAYLOAD_SIZE = 16ULL * 1024 * 1024;
static const int       MAX_CONNECTIONS = 50;

static const DWORD     SOCKET_TIMEOUT_MS = 30000;
static const DWORD     HANDSHAKE_TIMEOUT_MS = 10000;
static const std::string AUTH_TOKEN = "LqhDPjhfkjkun5D2K1V0L9ZYI6LW8QMpOEJauqGmDlMAvC4H"; //replace your token & token in vncmac.html
static const std::vector<std::string> ALLOWED_ORIGINS = {
     //""
};

// --- UI Constants ---
#define WM_TRAYICON         (WM_USER + 1)
#define ID_BTN_START        101
#define ID_TIMER_SPEED      102
#define ID_TRAY_OPEN        103
#define ID_TRAY_EXIT        104

// --- Global Variables ---
static std::atomic<int>    g_active_connections{ 0 };
static std::atomic<double> g_current_speed_mbps{ 0.0 };
static std::atomic<bool>   g_is_running{ false };
static SOCKET              g_listen_sock = INVALID_SOCKET;
static std::vector<double> g_speed_history(60, 0.0); // 60 seconds history

HWND g_hwndTargetIP;
HWND g_hwndTargetPort;
HWND g_hwndListenPort;
HWND g_hwndBtnStart;
HWND g_hwndSpeedTxt;
HWND g_hwndGraph;
HWND g_hwndLog;

NOTIFYICONDATAW g_nid = {};
HFONT g_hFont = NULL;


static void append_log(const std::string& msg) {
    if (!g_hwndLog) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    char time_buf[64];
    sprintf_s(time_buf, "[%04d-%02d-%02d %02d:%02d:%02d] ",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    std::string full_msg_ansi = std::string(time_buf) + msg + "\r\n";
    int wlen = MultiByteToWideChar(CP_ACP, 0, full_msg_ansi.c_str(), -1, NULL, 0);
    if (wlen <= 0) return;
    std::wstring wmsg(wlen, 0);
    MultiByteToWideChar(CP_ACP, 0, full_msg_ansi.c_str(), -1, &wmsg[0], wlen);

    int len = GetWindowTextLength(g_hwndLog);
    SendMessage(g_hwndLog, EM_SETSEL, len, len);
    SendMessage(g_hwndLog, EM_REPLACESEL, 0, (LPARAM)wmsg.c_str());
}


// Context structure for handling the connection pair
struct TunnelContext {
    SOCKET            ws_sock;
    SOCKET            tcp_sock;
    std::once_flag    cleanup_flag;
    std::mutex        ws_write_mutex;
    std::string client_info; 

    TunnelContext(SOCKET ws, SOCKET tcp) : ws_sock(ws), tcp_sock(tcp) {}

    void close_all() {
        std::call_once(cleanup_flag, [this]() {
            shutdown(ws_sock, SD_BOTH);
            shutdown(tcp_sock, SD_BOTH);
            closesocket(ws_sock);
            closesocket(tcp_sock);
            --g_active_connections;
            if (!client_info.empty()) {
                append_log("Disconnected: " + client_info);
            }
            });
    }

    ~TunnelContext() { close_all(); }
    TunnelContext(const TunnelContext&) = delete;
    TunnelContext& operator=(const TunnelContext&) = delete;
};

// --- Helper Functions ---
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

static bool send_exact(SOCKET sock, const char* buf, int len) {
    int total = 0;
    while (total < len) {
        int n = send(sock, buf + total, len - total, 0);
        if (n <= 0) return false;
        total += n;
    }
    return true;
}

static std::string get_header_value(const std::string& request, const std::string& header_name) {
    std::string lower_req = request;
    std::string lower_hdr = header_name;
    std::transform(lower_req.begin(), lower_req.end(), lower_req.begin(), ::tolower);
    std::transform(lower_hdr.begin(), lower_hdr.end(), lower_hdr.begin(), ::tolower);

    size_t pos = lower_req.find(lower_hdr);
    if (pos == std::string::npos) return "";

    pos += header_name.size();
    size_t end = request.find("\r\n", pos);
    if (end == std::string::npos) return "";

    std::string value = request.substr(pos, end - pos);
    size_t first = value.find_first_not_of(" \t");
    if (first == std::string::npos) return "";
    size_t last = value.find_last_not_of(" \t");
    return value.substr(first, last - first + 1);
}

static std::string get_websocket_accept_key(const std::string& client_key) {
    std::string concat = client_key + WS_MAGIC_STRING;
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    BYTE  hash[20] = {};
    DWORD hashLen = 20;

    if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) return "";
    if (!CryptCreateHash(hProv, CALG_SHA1, 0, 0, &hHash)) { CryptReleaseContext(hProv, 0); return ""; }
    if (!CryptHashData(hHash, reinterpret_cast<const BYTE*>(concat.c_str()), static_cast<DWORD>(concat.size()), 0)) {
        CryptDestroyHash(hHash); CryptReleaseContext(hProv, 0); return "";
    }
    if (!CryptGetHashParam(hHash, HP_HASHVAL, hash, &hashLen, 0)) {
        CryptDestroyHash(hHash); CryptReleaseContext(hProv, 0); return "";
    }

    DWORD b64Len = 0;
    CryptBinaryToStringA(hash, hashLen, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &b64Len);
    std::vector<char> b64Buf(b64Len);
    CryptBinaryToStringA(hash, hashLen, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64Buf.data(), &b64Len);

    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);

    std::string key(b64Buf.data());
    key.erase(std::remove(key.begin(), key.end(), '\r'), key.end());
    key.erase(std::remove(key.begin(), key.end(), '\n'), key.end());
    return key;
}

static bool constant_time_compare(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

static bool validate_websocket_handshake(const std::string& req) {
    if (req.substr(0, 4) != "GET ") return false;
    std::string upgrade = get_header_value(req, "Upgrade: ");
    std::transform(upgrade.begin(), upgrade.end(), upgrade.begin(), ::tolower);
    if (upgrade != "websocket") return false;
    std::string conn = get_header_value(req, "Connection: ");
    std::transform(conn.begin(), conn.end(), conn.begin(), ::tolower);
    if (conn.find("upgrade") == std::string::npos) return false;
    if (get_header_value(req, "Sec-WebSocket-Key: ").empty()) return false;
    if (get_header_value(req, "Sec-WebSocket-Version: ") != "13") return false;
    return true;
}

static bool validate_origin(const std::string& req) {
    if (ALLOWED_ORIGINS.empty()) return true;
    std::string origin = get_header_value(req, "Origin: ");
    if (origin.empty()) return false;
    for (const auto& allowed : ALLOWED_ORIGINS) if (origin == allowed) return true;

    return false;
}

static bool validate_auth(const std::string& req) {
    std::string token = get_header_value(req, "X-Auth-Token: ");
    if (token.empty()) {
        size_t end = req.find(" HTTP/");
        if (end != std::string::npos) {
            std::string uri = req.substr(4, end - 4);
            size_t tp = uri.find("token=");
            if (tp != std::string::npos) {
                tp += 6;
                size_t te = uri.find('&', tp);
                token = uri.substr(tp, te == std::string::npos ? std::string::npos : te - tp);
            }
        }
    }
    return !token.empty() && constant_time_compare(token, AUTH_TOKEN);
}

static void optimize_socket(SOCKET sock) {
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&one), sizeof(one));
    DWORD keepAlive = 1;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<char*>(&keepAlive), sizeof(keepAlive));
    DWORD zero = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&zero), sizeof(zero));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&zero), sizeof(zero));
    int giant_buf = 32 * 1024 * 1024; // 32MB
    DWORD timeout = SOCKET_TIMEOUT_MS;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
}

// --- Thread Functions ---
static void ws_to_tcp_thread(std::shared_ptr<TunnelContext> ctx) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    std::vector<char> payload;
    payload.reserve(1024 * 1024);

    while (g_is_running) {
        unsigned char hdr[2];
        if (!recv_exact(ctx->ws_sock, reinterpret_cast<char*>(hdr), 2)) break;

        bool     fin = (hdr[0] & 0x80) != 0;
        int      opcode = hdr[0] & 0x0F;
        bool     masked = (hdr[1] & 0x80) != 0;

        if (!masked) break; // RFC 6455: Client frames MUST be masked

        uint64_t pay_len = hdr[1] & 0x7F;

        if (pay_len == 126) {
            unsigned char ext[2];
            if (!recv_exact(ctx->ws_sock, reinterpret_cast<char*>(ext), 2)) break;
            pay_len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
        }
        else if (pay_len == 127) {
            unsigned char ext[8];
            if (!recv_exact(ctx->ws_sock, reinterpret_cast<char*>(ext), 8)) break;
            pay_len = 0;
            for (int i = 0; i < 8; ++i) pay_len = (pay_len << 8) | ext[i];
        }

        if (pay_len > MAX_PAYLOAD_SIZE) break;


        unsigned char mask_key[4];
        if (masked && !recv_exact(ctx->ws_sock, reinterpret_cast<char*>(mask_key), 4)) break;

        if (pay_len > 0) {
            payload.resize(static_cast<size_t>(pay_len));
            if (!recv_exact(ctx->ws_sock, payload.data(), static_cast<int>(pay_len))) break;
            if (masked) {
                for (size_t i = 0; i < static_cast<size_t>(pay_len); ++i)
                    payload[i] ^= mask_key[i & 3];
            }
        }
        else { payload.clear(); }

        if (opcode == 0x8) { break; }
        if (opcode == 0x9 || opcode == 0xA) { continue; }

        if (pay_len > 0) {
            if (!send_exact(ctx->tcp_sock, payload.data(), static_cast<int>(pay_len))) break;
        }
    }
    ctx->close_all();
}

static void tcp_to_ws_thread(std::shared_ptr<TunnelContext> ctx) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    const int MAX_HDR = 10;
    const int CHUNK = 1024 * 1024;
    std::vector<char> buf(MAX_HDR + CHUNK);

    auto last_report_time = std::chrono::steady_clock::now();
    uint64_t bytes_since_last_report = 0;

    while (g_is_running) {
        int total_n = recv(ctx->tcp_sock, buf.data() + MAX_HDR, CHUNK, 0);
        if (total_n <= 0) break;

        u_long available = 0;
        while (total_n < CHUNK) {
            if (ioctlsocket(ctx->tcp_sock, FIONREAD, &available) != 0 || available == 0) {
                break;
            }
            int to_read = (std::min)(static_cast<int>(available), CHUNK - total_n);
            int n2 = recv(ctx->tcp_sock, buf.data() + MAX_HDR + total_n, to_read, 0);
            if (n2 <= 0) break;
            total_n += n2;
        }

        int n = total_n;

        bytes_since_last_report += n;
        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_report_time).count();

        if (elapsed_ms >= 1000) {
            double MBps = (bytes_since_last_report / 1024.0 / 1024.0) / (elapsed_ms / 1000.0);
            g_current_speed_mbps = MBps;

            bytes_since_last_report = 0;
            last_report_time = now;
        }

        int  hdr_sz = 0;
        char* hdr = nullptr;

        if (n < 126) {
            hdr_sz = 2; hdr = buf.data() + MAX_HDR - hdr_sz;
            hdr[0] = static_cast<char>(0x82); hdr[1] = static_cast<char>(n);
        }
        else if (n <= 65535) {
            hdr_sz = 4; hdr = buf.data() + MAX_HDR - hdr_sz;
            hdr[0] = static_cast<char>(0x82); hdr[1] = static_cast<char>(126);
            hdr[2] = static_cast<char>((n >> 8) & 0xFF); hdr[3] = static_cast<char>(n & 0xFF);
        }
        else {
            hdr_sz = 10; hdr = buf.data() + MAX_HDR - hdr_sz;
            hdr[0] = static_cast<char>(0x82); hdr[1] = static_cast<char>(127);
            uint64_t len64 = static_cast<uint64_t>(n);
            hdr[2] = static_cast<char>((len64 >> 56) & 0xFF); hdr[3] = static_cast<char>((len64 >> 48) & 0xFF);
            hdr[4] = static_cast<char>((len64 >> 40) & 0xFF); hdr[5] = static_cast<char>((len64 >> 32) & 0xFF);
            hdr[6] = static_cast<char>((len64 >> 24) & 0xFF); hdr[7] = static_cast<char>((len64 >> 16) & 0xFF);
            hdr[8] = static_cast<char>((len64 >> 8) & 0xFF);  hdr[9] = static_cast<char>(len64 & 0xFF);
        }

        {
            std::lock_guard<std::mutex> lk(ctx->ws_write_mutex);
            if (!send_exact(ctx->ws_sock, hdr, hdr_sz + n)) break;
        }
    }

    g_current_speed_mbps = 0.0;
    ctx->close_all();
}

static void proxy_server_thread_func(std::string target_ip, int target_port, int listen_port) {
    WSADATA wsa{};
    int wsa_res = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (wsa_res != 0) return;

    g_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int reuse = 1;
    setsockopt(g_listen_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&reuse), sizeof(reuse));

    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &srv.sin_addr);
    srv.sin_port = htons(static_cast<u_short>(listen_port));

    bind(g_listen_sock, reinterpret_cast<sockaddr*>(&srv), sizeof(srv));
    listen(g_listen_sock, SOMAXCONN);

    while (g_is_running) {
        sockaddr_in cli{};
        int cli_len = sizeof(cli);
        SOCKET ws_sock = accept(g_listen_sock, reinterpret_cast<sockaddr*>(&cli), &cli_len);

        if (ws_sock == INVALID_SOCKET) {
            if (!g_is_running) break;
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        int client_port = ntohs(cli.sin_port);
        inet_ntop(AF_INET, &cli.sin_addr, client_ip, INET_ADDRSTRLEN);
        std::string c_info = std::string(client_ip) + ":" + std::to_string(client_port);


        /* ---- Connection Limit ---- */
        if (g_active_connections.load() >= MAX_CONNECTIONS) {
            const char* msg = "HTTP/1.1 503 Service Unavailable\r\n\r\n";
            send(ws_sock, msg, static_cast<int>(strlen(msg)), 0);
            closesocket(ws_sock);
            continue;
        }

        /* ---- Handshake Timeout ---- */
        DWORD hs_to = HANDSHAKE_TIMEOUT_MS;
        setsockopt(ws_sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&hs_to), sizeof(hs_to));

        /* ---- Secure Read Request ---- */
        std::string request;
        request.reserve(4096);
        char c;
        bool hs_ok = false;
        while (recv(ws_sock, &c, 1, 0) == 1) {
            request += c;
            if (request.size() >= 4 && request.substr(request.size() - 4) == "\r\n\r\n") {
                hs_ok = true;
                break;
            }
            if (request.size() >= MAX_HANDSHAKE_SIZE) break;
        }
        if (!hs_ok) { closesocket(ws_sock); continue; }

        /* ---- Security Validations ---- */

        if (!validate_websocket_handshake(request)) {
            append_log("Handshake Failed: " + c_info);
            const char* msg = "HTTP/1.1 400 Bad Request\r\n\r\n";
            send(ws_sock, msg, static_cast<int>(strlen(msg)), 0);
            closesocket(ws_sock); continue;
        }
        if (!validate_origin(request)) {
            append_log("Origin Rejected: " + c_info + " | Origin: " + get_header_value(request, "Origin: "));
            const char* msg = "HTTP/1.1 403 Forbidden\r\n\r\n";
            send(ws_sock, msg, static_cast<int>(strlen(msg)), 0);
            closesocket(ws_sock); continue;
        }
        if (!validate_auth(request)) {
            append_log("Auth Failed: " + c_info + " | Invalid Token");
            const char* msg = "HTTP/1.1 401 Unauthorized\r\n\r\n";
            send(ws_sock, msg, static_cast<int>(strlen(msg)), 0);
            closesocket(ws_sock); continue;
        }

        /* ---- Compute Accept Key & Response ---- */
        std::string client_key = get_header_value(request, "Sec-WebSocket-Key: ");
        std::string accept_key = get_websocket_accept_key(client_key);
        std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept_key + "\r\n\r\n";

        if (!send_exact(ws_sock, response.c_str(), static_cast<int>(response.size()))) {
            closesocket(ws_sock); continue;
        }

        append_log("Connected: " + c_info + " | Origin: " + get_header_value(request, "Origin: "));

        g_active_connections++;


        SOCKET tcp_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in tgt{};
        tgt.sin_family = AF_INET;
        inet_pton(AF_INET, target_ip.c_str(), &tgt.sin_addr);
        tgt.sin_port = htons(target_port);

        if (connect(tcp_sock, reinterpret_cast<sockaddr*>(&tgt), sizeof(tgt)) == SOCKET_ERROR) {
            closesocket(ws_sock); closesocket(tcp_sock);
            g_active_connections--;
            continue;
        }

        optimize_socket(ws_sock);
        optimize_socket(tcp_sock);

        auto ctx = std::make_shared<TunnelContext>(ws_sock, tcp_sock);
        ctx->client_info = c_info;
        std::thread t1(ws_to_tcp_thread, ctx);
        std::thread t2(tcp_to_ws_thread, ctx);
        t1.detach(); t2.detach();
    }

    if (g_listen_sock != INVALID_SOCKET) {
        closesocket(g_listen_sock);
        g_listen_sock = INVALID_SOCKET;
    }
    WSACleanup();
}

// --- Improved Graph Control (Task Manager Style Area Chart) ---
LRESULT CALLBACK GraphWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        int width = rc.right - rc.left;
        int height = rc.bottom - rc.top;

        // Double Buffering for flicker-free drawing
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBitmap = CreateCompatibleBitmap(hdc, width, height);
        SelectObject(memDC, memBitmap);

        // Modern White Background
        HBRUSH bgBrush = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(memDC, &rc, bgBrush);
        DeleteObject(bgBrush);

        // Draw subtle Grid lines
        HPEN gridPen = CreatePen(PS_SOLID, 1, RGB(235, 235, 235));
        SelectObject(memDC, gridPen);
        for (int i = 1; i < 5; ++i) { // Horizontal
            int y = i * height / 5;
            MoveToEx(memDC, 0, y, NULL); LineTo(memDC, width, y);
        }
        for (int i = 1; i < 6; ++i) { // Vertical
            int x = i * width / 6;
            MoveToEx(memDC, x, 0, NULL); LineTo(memDC, x, height);
        }
        DeleteObject(gridPen);

        // Calculate dynamic maximum value for Y-Axis
        double max_val = 1.0;
        for (double v : g_speed_history) {
            if (v > max_val) max_val = v * 1.2;
        }

        // Draw Y-Axis Maximum Ruler Text
        SetBkMode(memDC, TRANSPARENT);
        SetTextColor(memDC, RGB(120, 120, 120));
        SelectObject(memDC, g_hFont);
        WCHAR text[64];
        swprintf(text, 64, L"%.1f MB/s", max_val);
        RECT textRect = { 0, 5, width - 8, 25 };
        DrawTextW(memDC, text, -1, &textRect, DT_RIGHT | DT_TOP);

        // --- Draw the Area Chart (Filled Area + Top Line) ---
        int hist_len = (int)g_speed_history.size();
        POINT* pts = new POINT[hist_len + 2];

        for (int i = 0; i < hist_len; ++i) {
            pts[i].x = i * width / (hist_len - 1);
            pts[i].y = height - (int)((g_speed_history[i] / max_val) * height);
            if (pts[i].y < 0) pts[i].y = 0;
            if (pts[i].y >= height) pts[i].y = height - 1;
        }

        // Complete the polygon for the fill (bottom right to bottom left)
        pts[hist_len].x = width;
        pts[hist_len].y = height;
        pts[hist_len + 1].x = 0;
        pts[hist_len + 1].y = height;

        // Draw filled area
        HPEN nullPen = CreatePen(PS_NULL, 0, 0);
        HBRUSH fillBrush = CreateSolidBrush(RGB(225, 240, 255)); // Light Blue Fill
        SelectObject(memDC, nullPen);
        SelectObject(memDC, fillBrush);
        Polygon(memDC, pts, hist_len + 2);
        DeleteObject(fillBrush);
        DeleteObject(nullPen);

        // Draw the top accent line
        HPEN linePen = CreatePen(PS_SOLID, 2, RGB(0, 120, 215)); // Windows Blue Accent
        SelectObject(memDC, linePen);
        MoveToEx(memDC, pts[0].x, pts[0].y, NULL);
        for (int i = 1; i < hist_len; ++i) {
            LineTo(memDC, pts[i].x, pts[i].y);
        }
        DeleteObject(linePen);
        delete[] pts;

        // Draw sharp outer border
        HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
        SelectObject(memDC, borderPen);
        SelectObject(memDC, GetStockObject(HOLLOW_BRUSH));
        Rectangle(memDC, 0, 0, width, height);
        DeleteObject(borderPen);

        // Copy buffer to screen
        BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);

        DeleteObject(memBitmap);
        DeleteDC(memDC);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// --- Main Window Procedure ---
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        // Modern Segoe UI Font for clean English look
        g_hFont = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        // UI Elements positioning (Cleaned up spacing)
        HWND hLbl1 = CreateWindowW(L"STATIC", L"Target IP:", WS_VISIBLE | WS_CHILD, 20, 22, 80, 20, hwnd, NULL, NULL, NULL);
        g_hwndTargetIP = CreateWindowA("EDIT", "192.168.50.64", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, 100, 20, 140, 24, hwnd, NULL, NULL, NULL);

        HWND hLbl2 = CreateWindowW(L"STATIC", L"Target Port:", WS_VISIBLE | WS_CHILD, 260, 22, 80, 20, hwnd, NULL, NULL, NULL);
        g_hwndTargetPort = CreateWindowA("EDIT", "5900", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, 340, 20, 80, 24, hwnd, NULL, NULL, NULL);

        HWND hLbl3 = CreateWindowW(L"STATIC", L"Listen Port:", WS_VISIBLE | WS_CHILD, 20, 62, 80, 20, hwnd, NULL, NULL, NULL);
        g_hwndListenPort = CreateWindowA("EDIT", "6800", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, 100, 60, 140, 24, hwnd, NULL, NULL, NULL);

        g_hwndBtnStart = CreateWindowW(L"BUTTON", L"Start Proxy", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 260, 58, 160, 28, hwnd, (HMENU)ID_BTN_START, NULL, NULL);

        g_hwndSpeedTxt = CreateWindowW(L"STATIC", L"Status: Waiting to start...", WS_VISIBLE | WS_CHILD, 20, 105, 400, 20, hwnd, NULL, NULL, NULL);

        // Register & Create the newly beautified Graph Control
        WNDCLASSW gwc = {};
        gwc.lpfnWndProc = GraphWndProc;
        gwc.hInstance = GetModuleHandle(NULL);
        gwc.lpszClassName = L"GraphControlClass";
        RegisterClassW(&gwc);

        g_hwndGraph = CreateWindowW(L"GraphControlClass", L"", WS_VISIBLE | WS_CHILD, 20, 135, 400, 200, hwnd, NULL, NULL, NULL);

        g_hwndLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_VISIBLE | WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            20, 345, 400, 200, hwnd, NULL, NULL, NULL);

        SendMessage(g_hwndLog, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // Apply fonts
        SendMessage(hLbl1, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessage(g_hwndTargetIP, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessage(hLbl2, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessage(g_hwndTargetPort, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessage(hLbl3, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessage(g_hwndListenPort, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessage(g_hwndBtnStart, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessage(g_hwndSpeedTxt, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // Setup Tray Icon
        memset(&g_nid, 0, sizeof(NOTIFYICONDATAW));
        g_nid.cbSize = sizeof(NOTIFYICONDATAW);
        g_nid.hWnd = hwnd;
        g_nid.uID = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_TRAYICON;
        g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
        wcscpy_s(g_nid.szTip, sizeof(g_nid.szTip) / sizeof(WCHAR), L"WebSocket Proxy");
        Shell_NotifyIconW(NIM_ADD, &g_nid);

        SetTimer(hwnd, ID_TIMER_SPEED, 1000, NULL);
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdcStatic = (HDC)wParam;
        SetTextColor(hdcStatic, RGB(20, 20, 20));
        SetBkMode(hdcStatic, TRANSPARENT);
        return (INT_PTR)GetStockObject(WHITE_BRUSH);
    }

    case WM_COMMAND: {
        if (LOWORD(wParam) == ID_BTN_START) {
            if (!g_is_running) {
                char ip[256], tPort[32], lPort[32];
                GetWindowTextA(g_hwndTargetIP, ip, sizeof(ip));
                GetWindowTextA(g_hwndTargetPort, tPort, sizeof(tPort));
                GetWindowTextA(g_hwndListenPort, lPort, sizeof(lPort));

                g_is_running = true;
                std::thread(proxy_server_thread_func, std::string(ip), std::stoi(tPort), std::stoi(lPort)).detach();

                SetWindowTextW(g_hwndBtnStart, L"Stop Proxy");
                EnableWindow(g_hwndTargetIP, FALSE);
                EnableWindow(g_hwndTargetPort, FALSE);
                EnableWindow(g_hwndListenPort, FALSE);
            }
            else {
                g_is_running = false;
                if (g_listen_sock != INVALID_SOCKET) {
                    closesocket(g_listen_sock);
                    g_listen_sock = INVALID_SOCKET;
                }
                SetWindowTextW(g_hwndBtnStart, L"Start Proxy");
                EnableWindow(g_hwndTargetIP, TRUE);
                EnableWindow(g_hwndTargetPort, TRUE);
                EnableWindow(g_hwndListenPort, TRUE);
            }
        }
        else if (LOWORD(wParam) == ID_TRAY_OPEN) {
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        }
        else if (LOWORD(wParam) == ID_TRAY_EXIT) {
            PostQuitMessage(0);
        }
        return 0;
    }

    case WM_TIMER: {
        if (wParam == ID_TIMER_SPEED) {
            double speed = g_is_running ? g_current_speed_mbps.load() : 0.0;
            int connCount = g_active_connections.load();

            for (size_t i = 0; i < g_speed_history.size() - 1; ++i) {
                g_speed_history[i] = g_speed_history[i + 1];
            }
            g_speed_history.back() = speed;

            InvalidateRect(g_hwndGraph, NULL, FALSE);

            WCHAR text[128];
            if (g_is_running) {
                swprintf(text, 128, L"Speed: %.2f MB/s  |  Active Connections: %d", speed, connCount);
            }
            else {
                swprintf(text, 128, L"Status: Stopped");
            }
            SetWindowTextW(g_hwndSpeedTxt, text);

            swprintf(g_nid.szTip, sizeof(g_nid.szTip) / sizeof(WCHAR), L"Proxy Speed: %.2f MB/s", speed);
            Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        }
        return 0;
    }

                 // CRITICAL FIX: Ensure movement (SC_MOVE) works by routing unhandled commands to DefWindowProc
    case WM_SYSCOMMAND: {
        if ((wParam & 0xFFF0) == SC_MINIMIZE) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        break;
    }

    case WM_CLOSE: {
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }

    case WM_TRAYICON: {
        if (lParam == WM_LBUTTONDBLCLK) {
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        }
        else if (lParam == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_OPEN, L"Open Dashboard");
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");
            SetForegroundWindow(hwnd);
            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);
        }
        return 0;
    }

    case WM_DESTROY: {
        g_is_running = false;
        if (g_listen_sock != INVALID_SOCKET) {
            closesocket(g_listen_sock);
        }
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_hFont) DeleteObject(g_hFont);
        PostQuitMessage(0);
        return 0;
    }
    }

    // Default window procedure for uncaught messages (Fixes dragging, resizing, closing logic)
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int APIENTRY WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nCmdShow) {
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

    const wchar_t* CLASS_NAME = L"ProxyWindowClass";

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);

    RegisterClassW(&wc);

    DWORD dwStyle = WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX;

    HWND hwnd = CreateWindowExW(
        0,
        CLASS_NAME,
        L"WebSocket to TCP Proxy",
        dwStyle,
        CW_USEDEFAULT, CW_USEDEFAULT, 455, 600,
        NULL, NULL, hInstance, NULL
    );

    if (hwnd == NULL) {
        return 0;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg = {};
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return 0;
}
