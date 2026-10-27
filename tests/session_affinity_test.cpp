// session_affinity_test — #118: a routing gateway (bifrost, OpenRouter) keeps
// a conversation on the backend that served it last when every request
// carries the same `x-session-id`. These tests stream a real turn through the
// OpenAI-compatible transport to a loopback server and read what arrived.

#include "agtest.hpp"

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "agentty/provider/openai/provider.hpp"
#include "agentty/provider/openai/transport.hpp"

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <unistd.h>
#endif

using namespace agentty;
namespace oai = agentty::provider::openai;

namespace {

#if defined(_WIN32)
using socket_t = SOCKET;
constexpr socket_t kInvalid = INVALID_SOCKET;
void close_socket(socket_t s) { if (s != kInvalid) ::closesocket(s); }
#else
using socket_t = int;
constexpr socket_t kInvalid = -1;
void close_socket(socket_t s) { if (s != kInvalid) ::close(s); }
#endif

// Answers each POST with a one-word SSE turn and keeps the request heads.
class ChatServer {
public:
    ChatServer() {
#if defined(_WIN32)
        WSADATA d{};
        if (::WSAStartup(MAKEWORD(2, 2), &d) != 0) return;
        wsa_ = true;
#endif
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ == kInvalid) return;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(fd_, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0
            || ::listen(fd_, 8) != 0) {
            close_socket(fd_);
            fd_ = kInvalid;
            return;
        }
#if defined(_WIN32)
        int len = sizeof(a);
#else
        socklen_t len = sizeof(a);
#endif
        (void)::getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        worker_ = std::thread([this] { serve(); });
    }

    ~ChatServer() {
        stop_.store(true);
        if (fd_ != kInvalid) {   // wake accept()
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            a.sin_port = htons(port_);
            socket_t w = ::socket(AF_INET, SOCK_STREAM, 0);
            if (w != kInvalid) {
                (void)::connect(w, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
                close_socket(w);
            }
        }
        if (worker_.joinable()) worker_.join();
        close_socket(fd_);
#if defined(_WIN32)
        if (wsa_) ::WSACleanup();
#endif
    }

    [[nodiscard]] bool valid() const { return fd_ != kInvalid; }
    [[nodiscard]] std::uint16_t port() const { return port_; }

    // The value of `name` in the last request head, lowercased name match.
    [[nodiscard]] std::optional<std::string> header(std::string_view name) const {
        std::lock_guard lk(mu_);
        std::size_t pos = 0;
        while (pos < last_head_.size()) {
            const auto eol = last_head_.find("\r\n", pos);
            const auto line = last_head_.substr(pos, eol == std::string::npos ? std::string::npos
                                                                              : eol - pos);
            const auto colon = line.find(':');
            if (colon != std::string::npos) {
                std::string key = line.substr(0, colon);
                for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (key == name) {
                    auto v = line.substr(colon + 1);
                    while (!v.empty() && v.front() == ' ') v.erase(v.begin());
                    return v;
                }
            }
            if (eol == std::string::npos) break;
            pos = eol + 2;
        }
        return std::nullopt;
    }
    [[nodiscard]] std::string body() const { std::lock_guard lk(mu_); return last_body_; }

private:
    void serve() {
        while (!stop_.load()) {
            const socket_t c = ::accept(fd_, nullptr, nullptr);
            if (c == kInvalid) continue;
            if (stop_.load()) { close_socket(c); break; }
#if defined(_WIN32)
            const DWORD ms = 5000;
            (void)::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO,
                               reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
            timeval tv{5, 0};
            (void)::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
            handle(c);
            close_socket(c);
        }
    }

    void handle(socket_t c) {
        std::string in;
        char buf[4096];
        std::size_t head_end = std::string::npos, want = 0;
        for (;;) {
            const auto n = ::recv(c, buf, sizeof(buf), 0);
            if (n <= 0) return;
            in.append(buf, static_cast<std::size_t>(n));
            if (head_end == std::string::npos && (head_end = in.find("\r\n\r\n")) != std::string::npos) {
                std::string lower = in.substr(0, head_end);
                for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (const auto m = lower.find("content-length:"); m != std::string::npos)
                    want = std::strtoull(lower.c_str() + m + 15, nullptr, 10);
            }
            if (head_end != std::string::npos && in.size() >= head_end + 4 + want) break;
        }
        {
            std::lock_guard lk(mu_);
            last_head_ = in.substr(0, head_end);
            last_body_ = in.substr(head_end + 4, want);
        }
        const std::string sse =
            "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"ok\"}}]}\n\n"
            "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
            "data: [DONE]\n\n";
        const std::string resp =
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n"
            "Content-Length: " + std::to_string(sse.size()) + "\r\n\r\n" + sse;
        std::size_t sent = 0;
        while (sent < resp.size()) {
            const auto n = ::send(c, resp.data() + sent, static_cast<int>(resp.size() - sent), 0);
            if (n <= 0) return;
            sent += static_cast<std::size_t>(n);
        }
    }

    socket_t fd_ = kInvalid;
    std::uint16_t port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread worker_;
    mutable std::mutex mu_;
    std::string last_head_, last_body_;
#if defined(_WIN32)
    bool wsa_ = false;
#endif
};

// A custom host (no registry row): the shape a self-run bifrost has.
oai::Endpoint gateway(std::uint16_t port) {
    oai::Endpoint ep;
    ep.host    = "127.0.0.1";
    ep.port    = port;
    ep.path    = "/v1/chat/completions";
    ep.use_tls = false;
    ep.label   = "127.0.0.1:" + std::to_string(port);
    return ep;
}

provider::Request turn(std::string session_key, std::string affinity_key = {}) {
    provider::Request req;
    req.model        = "router/auto";
    req.max_tokens   = 64;
    req.session_key  = std::move(session_key);
    req.affinity_key = std::move(affinity_key);
    Message m;
    m.role = Role::User;
    m.text = "hi";
    req.messages.push_back(std::move(m));
    return req;
}

provider::StreamResult send(const ChatServer& s, provider::Request req) {
    oai::OpenAIProvider p{gateway(s.port())};
    return p.stream(agtest::cap(), std::move(req), [](Msg) {});
}

} // namespace

TEST_CASE("session affinity: the thread id rides x-session-id (#118)") {
    ChatServer s;
    REQUIRE(s.valid(), "loopback server starts");
    const auto r = send(s, turn("thread-0123"));
    CHECK(r.ok());
    CHECK(s.header("x-session-id") == std::optional<std::string>{"thread-0123"});
}

TEST_CASE("session affinity: a subagent sticks with its parent's session") {
    // The subagent keeps its own prompt-cache key, but the gateway session
    // is the parent thread's, so it lands where the chat already is.
    ChatServer s;
    REQUIRE(s.valid(), "loopback server starts");
    const auto r = send(s, turn("subagent:explorer:abcd", "parent-thread"));
    CHECK(r.ok());
    CHECK(s.header("x-session-id") == std::optional<std::string>{"parent-thread"});
}

TEST_CASE("session affinity: no conversation, no header") {
    ChatServer s;
    REQUIRE(s.valid(), "loopback server starts");
    const auto r = send(s, turn(""));
    CHECK(r.ok());
    CHECK(!s.header("x-session-id").has_value());
}

TEST_CASE("session affinity: the header is all a custom host gets") {
    // prompt_cache_key stays off for hosts that didn't opt in (#93): the
    // session rides a header, which an unknown host ignores, never the body.
    ChatServer s;
    REQUIRE(s.valid(), "loopback server starts");
    (void)send(s, turn("thread-0123"));
    CHECK(s.body().find("prompt_cache_key") == std::string::npos);
}

TEST_CASE("session affinity: provider::Request::affinity falls back to the session") {
    provider::Request req;
    CHECK(req.affinity().empty());
    req.session_key = "t";
    CHECK(req.affinity() == "t");
    req.affinity_key = "parent";
    CHECK(req.affinity() == "parent");
}
