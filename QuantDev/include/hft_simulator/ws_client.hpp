#pragma once

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>

#include <netdb.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <string>
#include <cstdint>
#include <cstring>

namespace hft {

class WsClient {
public:
    ~WsClient() { close(); }

    std::string last_error() const { return err_; }

    bool connect(const std::string& host, const std::string& port,
                 const std::string& target) {

        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
            err_ = "DNS resolution failed for " + host; return false;
        }
        for (addrinfo* p = res; p; p = p->ai_next) {
            fd_ = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (fd_ < 0) continue;
            if (::connect(fd_, p->ai_addr, p->ai_addrlen) == 0) break;
            ::close(fd_); fd_ = -1;
        }
        freeaddrinfo(res);
        if (fd_ < 0) { err_ = "TCP connect failed"; return false; }

        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_) { err_ = "SSL_CTX_new failed"; return false; }
        SSL_CTX_set_default_verify_paths(ctx_);
        ssl_ = SSL_new(ctx_);
        SSL_set_fd(ssl_, fd_);
        SSL_set_tlsext_host_name(ssl_, host.c_str());
        SSL_set1_host(ssl_, host.c_str());
        SSL_set_verify(ssl_, SSL_VERIFY_PEER, nullptr);
        if (SSL_connect(ssl_) != 1) {
            err_ = "TLS handshake failed (cert verify?)"; return false;
        }

        std::string key = make_key();
        std::string req =
            "GET " + target + " HTTP/1.1\r\n"
            "Host: " + host + ":" + port + "\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: " + key + "\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n";
        if (SSL_write(ssl_, req.data(), (int)req.size()) <= 0) {
            err_ = "WS handshake write failed"; return false;
        }

        std::string resp;
        while (resp.find("\r\n\r\n") == std::string::npos) {
            char c; int r = SSL_read(ssl_, &c, 1);
            if (r <= 0) { err_ = "WS handshake read failed"; return false; }
            resp.push_back(c);
            if (resp.size() > 8192) break;
        }
        if (resp.find(" 101 ") == std::string::npos) {
            err_ = "WS upgrade rejected: " + resp.substr(0, resp.find("\r\n"));
            return false;
        }
        return true;
    }

    bool read_message(std::string& out) {
        std::string assembled;
        while (true) {
            std::string h;
            if (!read_n(2, h)) return false;
            uint8_t b0 = (uint8_t)h[0], b1 = (uint8_t)h[1];
            bool   fin    = b0 & 0x80;
            int    op     = b0 & 0x0F;
            bool   masked = b1 & 0x80;
            uint64_t len  = b1 & 0x7F;
            if (len == 126) {
                std::string e; if (!read_n(2, e)) return false;
                len = ((uint64_t)(uint8_t)e[0] << 8) | (uint8_t)e[1];
            } else if (len == 127) {
                std::string e; if (!read_n(8, e)) return false;
                len = 0; for (int i = 0; i < 8; ++i) len = (len << 8) | (uint8_t)e[i];
            }
            std::string payload;
            if (len && !read_n(len, payload)) return false;
            if (masked && payload.size() >= 4) {

            }
            if (op == 0x9) { send_frame(0xA, payload); continue; }
            if (op == 0xA) { continue; }
            if (op == 0x8) { send_frame(0x8, payload); return false; }
            assembled += payload;
            if (fin) { out.swap(assembled); return true; }
        }
    }

    void set_timeout(int seconds) {
        if (fd_ < 0) return;
        timeval tv{ seconds, 0 };
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }

    void close() {
        if (ssl_) { SSL_shutdown(ssl_); SSL_free(ssl_); ssl_ = nullptr; }
        if (ctx_) { SSL_CTX_free(ctx_); ctx_ = nullptr; }
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

private:
    bool fill_() {
        char tmp[16384];
        int r = SSL_read(ssl_, tmp, sizeof tmp);
        if (r <= 0) return false;
        buf_.append(tmp, r);
        return true;
    }
    bool read_n(size_t n, std::string& dst) {
        while (buf_.size() - off_ < n) if (!fill_()) return false;
        dst.assign(buf_.data() + off_, n);
        off_ += n;
        if (off_ > (1u << 16)) { buf_.erase(0, off_); off_ = 0; }
        return true;
    }

    void send_frame(int op, const std::string& p) {
        std::string f;
        f.push_back((char)(0x80 | op));
        const uint8_t M = 0x80;
        if (p.size() < 126) {
            f.push_back((char)(M | p.size()));
        } else if (p.size() <= 0xFFFF) {
            f.push_back((char)(M | 126));
            f.push_back((char)((p.size() >> 8) & 0xFF));
            f.push_back((char)(p.size() & 0xFF));
        } else {
            f.push_back((char)(M | 127));
            for (int i = 7; i >= 0; --i) f.push_back((char)((p.size() >> (8 * i)) & 0xFF));
        }
        unsigned char mk[4]; RAND_bytes(mk, 4);
        f.append((char*)mk, 4);
        for (size_t i = 0; i < p.size(); ++i) f.push_back((char)(p[i] ^ mk[i % 4]));
        SSL_write(ssl_, f.data(), (int)f.size());
    }

    static std::string b64(const unsigned char* d, int n) {
        static const char* T =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string o; int v = 0, b = -6;
        for (int i = 0; i < n; ++i) {
            v = (v << 8) | d[i]; b += 8;
            while (b >= 0) { o.push_back(T[(v >> b) & 0x3F]); b -= 6; }
        }
        if (b > -6) o.push_back(T[((v << 8) >> (b + 8)) & 0x3F]);
        while (o.size() % 4) o.push_back('=');
        return o;
    }
    static std::string make_key() {
        unsigned char r[16]; RAND_bytes(r, 16); return b64(r, 16);
    }

    int      fd_  = -1;
    SSL*     ssl_ = nullptr;
    SSL_CTX* ctx_ = nullptr;
    std::string buf_;
    size_t      off_ = 0;
    std::string err_;
};

}
