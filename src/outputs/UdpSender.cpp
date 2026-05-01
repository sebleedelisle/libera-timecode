#include "UdpSender.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
#define CLOSE_SOCKET ::closesocket
static int lastSocketError() { return WSAGetLastError(); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
using socket_t = int;
#define CLOSE_SOCKET ::close
static int lastSocketError() { return errno; }
#endif

namespace libera_timecode {

namespace {

bool ipIsBroadcast(const std::string& ip) {
    return ip == "255.255.255.255";
}

std::string trim(const std::string& s) {
    auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> splitTargets(const std::string& csv) {
    std::vector<std::string> out;
    std::string buf;
    for (char c : csv) {
        if (c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\n') {
            auto t = trim(buf);
            if (!t.empty()) out.push_back(t);
            buf.clear();
        } else {
            buf += c;
        }
    }
    auto t = trim(buf);
    if (!t.empty()) out.push_back(t);
    return out;
}

#ifdef _WIN32
struct WsaInit {
    WsaInit() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    ~WsaInit() { WSACleanup(); }
};
WsaInit& wsaInit() {
    static WsaInit instance;
    return instance;
}
#endif

} // namespace

UdpSender::UdpSender() {
#ifdef _WIN32
    wsaInit();
#endif
}

UdpSender::~UdpSender() { close(); }

void UdpSender::close() {
    if (socket_ >= 0) {
        CLOSE_SOCKET(static_cast<socket_t>(socket_));
        socket_ = -1;
    }
    targets_.clear();
}

bool UdpSender::configure(const std::string& targetsCsv, int targetPort, std::string& errorMessage) {
    close();
    targetPort_ = targetPort;
    const auto ips = splitTargets(targetsCsv);
    if (ips.empty()) {
        errorMessage = "No targets specified";
        return false;
    }

    socket_t fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        errorMessage = "socket() failed: " + std::to_string(lastSocketError());
        return false;
    }

    bool wantBroadcast = std::any_of(ips.begin(), ips.end(), ipIsBroadcast);
    if (wantBroadcast) {
        int yes = 1;
        if (::setsockopt(fd, SOL_SOCKET, SO_BROADCAST,
                         reinterpret_cast<const char*>(&yes), sizeof(yes)) < 0) {
            errorMessage = "SO_BROADCAST failed: " + std::to_string(lastSocketError());
            CLOSE_SOCKET(fd);
            return false;
        }
    }

    for (const auto& ip : ips) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(targetPort));
        if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
            errorMessage = "Invalid IP: " + ip;
            CLOSE_SOCKET(fd);
            targets_.clear();
            return false;
        }
        Target t;
        t.ip = ip;
        std::memcpy(t.addr, &addr, sizeof(addr));
        t.addrLen = sizeof(addr);
        targets_.push_back(std::move(t));
    }

    socket_ = static_cast<int>(fd);
    return true;
}

bool UdpSender::send(const void* data, std::size_t bytes) {
    if (socket_ < 0 || targets_.empty()) return false;
    bool anyOk = false;
    for (const auto& t : targets_) {
        const sockaddr* a = reinterpret_cast<const sockaddr*>(t.addr);
#ifdef _WIN32
        int n = ::sendto(static_cast<socket_t>(socket_),
                         static_cast<const char*>(data), static_cast<int>(bytes),
                         0, a, static_cast<int>(t.addrLen));
        if (n == static_cast<int>(bytes)) anyOk = true;
#else
        ssize_t n = ::sendto(socket_, data, bytes, 0, a, static_cast<socklen_t>(t.addrLen));
        if (n == static_cast<ssize_t>(bytes)) anyOk = true;
#endif
    }
    return anyOk;
}

} // namespace libera_timecode
