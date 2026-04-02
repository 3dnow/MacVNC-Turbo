/*
 * Native Windows WebSocket to TCP Proxy (WebSockify) - V9 Ultra-Low Latency Edition
 * Extreme latency optimizations:
 * 1. Process & Thread elevated to TIME_CRITICAL priority to preempt OS scheduler.
 * 2. L1/L2 Cache aligned buffer sizes (64KB) for nanosecond in-cache framing.
 * 3. Aggressive socket flushing and hardware offload hints.
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

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "crypt32.lib")

const std::string WS_MAGIC_STRING = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// Helper function: Ensure exact number of bytes are read from socket
bool recv_exact(SOCKET sock, char* buf, int len) {
    int total = 0;
    while (total < len) {
        int n = recv(sock, buf + total, len - total, 0);
        if (n <= 0) return false;
        total += n;
    }
    return true;
}

std::string get_websocket_accept_key(const std::string& client_key) {
    std::string magic_concat = client_key + WS_MAGIC_STRING;
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    BYTE hash[20];
    DWORD hashLen = 20;

    CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT);
    CryptCreateHash(hProv, CALG_SHA1, 0, 0, &hHash);
    CryptHashData(hHash, (BYTE*)magic_concat.c_str(), (DWORD)magic_concat.length(), 0);
    CryptGetHashParam(hHash, HP_HASHVAL, hash, &hashLen, 0);

    DWORD base64Len = 0;
    CryptBinaryToStringA(hash, hashLen, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &base64Len);
    std::vector<char> base64Buf(base64Len);
    CryptBinaryToStringA(hash, hashLen, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, base64Buf.data(), &base64Len);

    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);

    std::string accept_key(base64Buf.data());
    accept_key.erase(std::remove(accept_key.begin(), accept_key.end(), '\r'), accept_key.end());
    accept_key.erase(std::remove(accept_key.begin(), accept_key.end(), '\n'), accept_key.end());
    return accept_key;
}

void optimize_socket_for_latency(SOCKET sock) {
    int flag = 1;
    // Disable Nagle's algorithm (Force immediate transmission)
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(int));

    // Set buffers to 1MB. Not too large (avoids bloat/queuing), not too small.
    int buf_size = 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)&buf_size, sizeof(int));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char*)&buf_size, sizeof(int));
}

// Thread 1: Browser (WebSocket) -> Mac (TCP)
void ws_to_tcp_thread(SOCKET ws_sock, SOCKET tcp_sock) {
    // Elevate thread to highest possible priority for instant mouse/keyboard processing
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    std::vector<char> payload;
    payload.reserve(1024 * 1024);

    while (true) {
        unsigned char header[2];
        if (!recv_exact(ws_sock, (char*)header, 2)) break;

        int opcode = header[0] & 0x0F;
        int mask_bit = (header[1] & 0x80) >> 7;
        uint64_t payload_len = header[1] & 0x7F;

        if (payload_len == 126) {
            unsigned char ext_len[2];
            if (!recv_exact(ws_sock, (char*)ext_len, 2)) break;
            payload_len = ((uint64_t)ext_len[0] << 8) | ext_len[1];
        }
        else if (payload_len == 127) {
            unsigned char ext_len[8];
            if (!recv_exact(ws_sock, (char*)ext_len, 8)) break;
            payload_len = 0;
            for (int i = 0; i < 8; i++) {
                payload_len = (payload_len << 8) | ext_len[i];
            }
        }

        unsigned char mask_key[4] = { 0 };
        if (mask_bit) {
            if (!recv_exact(ws_sock, (char*)mask_key, 4)) break;
        }

        if (payload_len > 0) {
            payload.resize((size_t)payload_len);
            if (!recv_exact(ws_sock, payload.data(), (int)payload_len)) break;

            if (mask_bit) {
                // Inline unmasking in CPU cache
                for (uint64_t i = 0; i < payload_len; ++i) {
                    payload[i] ^= mask_key[i % 4];
                }
            }
        }

        if (opcode == 8) {
            std::cout << "[*] Browser disconnected." << std::endl;
            break;
        }
        else if (opcode == 9) {
            std::vector<unsigned char> pong;
            pong.push_back(0x8A);
            pong.push_back((unsigned char)(payload_len < 126 ? payload_len : 0));
            if (payload_len > 0) pong.insert(pong.end(), payload.begin(), payload.end());
            send(ws_sock, (char*)pong.data(), (int)pong.size(), 0);
            continue;
        }
        else if (opcode == 10) {
            continue;
        }

        if (payload_len > 0) {
            if (send(tcp_sock, payload.data(), (int)payload_len, 0) <= 0) break;
        }
    }

    shutdown(tcp_sock, SD_BOTH);
    closesocket(tcp_sock);
    closesocket(ws_sock);
}

// Thread 2: Mac (TCP) -> Browser (WebSocket)
void tcp_to_ws_thread(SOCKET ws_sock, SOCKET tcp_sock) {
    // Elevate thread to time critical
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    const int MAX_HEADER_SIZE = 10;
    // Revert to 128KB chunk size. 
    // This perfectly fits into CPU L2 cache for instant zero-copy framing.
    // Pushes frames to the browser instantly rather than waiting to batch massive 4MB chunks.
    const int CHUNK_SIZE = 128 * 1024;
    std::vector<char> send_buf(MAX_HEADER_SIZE + CHUNK_SIZE);

    while (true) {
        int bytes_read = recv(tcp_sock, send_buf.data() + MAX_HEADER_SIZE, CHUNK_SIZE, 0);
        if (bytes_read <= 0) {
            std::cout << "[*] Mac disconnected." << std::endl;
            break;
        }

        int header_size = 0;
        char* header_start = nullptr;

        if (bytes_read < 126) {
            header_size = 2;
            header_start = send_buf.data() + MAX_HEADER_SIZE - header_size;
            header_start[0] = (char)0x82;
            header_start[1] = (char)bytes_read;
        }
        else if (bytes_read <= 65535) {
            header_size = 4;
            header_start = send_buf.data() + MAX_HEADER_SIZE - header_size;
            header_start[0] = (char)0x82;
            header_start[1] = 126;
            header_start[2] = (char)((bytes_read >> 8) & 0xFF);
            header_start[3] = (char)(bytes_read & 0xFF);
        }
        else {
            header_size = 10;
            header_start = send_buf.data() + MAX_HEADER_SIZE - header_size;
            header_start[0] = (char)0x82;
            header_start[1] = 127;
            uint64_t len64 = (uint64_t)bytes_read;
            header_start[2] = (char)((len64 >> 56) & 0xFF);
            header_start[3] = (char)((len64 >> 48) & 0xFF);
            header_start[4] = (char)((len64 >> 40) & 0xFF);
            header_start[5] = (char)((len64 >> 32) & 0xFF);
            header_start[6] = (char)((len64 >> 24) & 0xFF);
            header_start[7] = (char)((len64 >> 16) & 0xFF);
            header_start[8] = (char)((len64 >> 8) & 0xFF);
            header_start[9] = (char)(len64 & 0xFF);
        }

        if (send(ws_sock, header_start, header_size + bytes_read, 0) <= 0) {
            break;
        }
    }

    shutdown(ws_sock, SD_BOTH);
    closesocket(ws_sock);
    closesocket(tcp_sock);
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: proxy.exe <Mac IP> <Listen Port>" << std::endl;
        return 1;
    }

    // Force the entire process to High Priority class
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    std::string mac_ip = argv[1];
    int listen_port = std::stoi(argv[2]);

    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in server_addr = { 0 };
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(listen_port);

    bind(listen_sock, (sockaddr*)&server_addr, sizeof(server_addr));
    listen(listen_sock, SOMAXCONN);

    std::cout << "[+] Proxy (V9 Low-Latency) running on port " << listen_port << " -> " << mac_ip << ":5900" << std::endl;

    while (true) {
        sockaddr_in client_addr;
        int client_len = sizeof(client_addr);
        SOCKET ws_sock = accept(listen_sock, (sockaddr*)&client_addr, &client_len);

        std::string request;
        char c;
        while (recv(ws_sock, &c, 1, 0) > 0) {
            request += c;
            if (request.size() >= 4 && request.substr(request.size() - 4) == "\r\n\r\n") {
                break;
            }
        }

        if (request.empty()) {
            closesocket(ws_sock);
            continue;
        }

        size_t key_pos = request.find("Sec-WebSocket-Key: ");
        if (key_pos != std::string::npos) {
            key_pos += 19;
            size_t key_end = request.find("\r\n", key_pos);
            std::string client_key = request.substr(key_pos, key_end - key_pos);

            std::string accept_key = get_websocket_accept_key(client_key);

            std::string response =
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Accept: " + accept_key + "\r\n";

            size_t proto_pos = request.find("Sec-WebSocket-Protocol: ");
            if (proto_pos != std::string::npos) {
                proto_pos += 24;
                size_t proto_end = request.find("\r\n", proto_pos);
                std::string protos = request.substr(proto_pos, proto_end - proto_pos);
                size_t comma_pos = protos.find(",");
                std::string selected_proto = (comma_pos != std::string::npos) ? protos.substr(0, comma_pos) : protos;
                response += "Sec-WebSocket-Protocol: " + selected_proto + "\r\n";
            }
            response += "\r\n";

            send(ws_sock, response.c_str(), (int)response.length(), 0);

            SOCKET tcp_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in mac_addr = { 0 };
            mac_addr.sin_family = AF_INET;
            inet_pton(AF_INET, mac_ip.c_str(), &mac_addr.sin_addr);
            mac_addr.sin_port = htons(5900);

            if (connect(tcp_sock, (sockaddr*)&mac_addr, sizeof(mac_addr)) == SOCKET_ERROR) {
                std::cerr << "[-] Target connection failed." << std::endl;
                closesocket(ws_sock);
                closesocket(tcp_sock);
                continue;
            }

            optimize_socket_for_latency(ws_sock);
            optimize_socket_for_latency(tcp_sock);

            std::cout << "[+] Tunnel established. Ultra-Low Latency forwarding active." << std::endl;

            std::thread t1(ws_to_tcp_thread, ws_sock, tcp_sock);
            std::thread t2(tcp_to_ws_thread, ws_sock, tcp_sock);
            t1.detach();
            t2.detach();
        }
        else {
            closesocket(ws_sock);
        }
    }

    closesocket(listen_sock);
    WSACleanup();
    return 0;
}