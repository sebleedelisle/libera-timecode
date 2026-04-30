#include "UdpSender.h"

#include <cstring>

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
}

bool UdpSender::configure(const std::string& targetIp, int targetPort, std::string& errorMessage) {
    close();
    targetIp_ = targetIp;
    targetPort_ = targetPort;
    isBroadcast_ = ipIsBroadcast(targetIp);

    socket_t fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        errorMessage = "socket() failed: " + std::to_string(lastSocketError());
        return false;
    }

    if (isBroadcast_) {
        int yes = 1;
        if (::setsockopt(fd, SOL_SOCKET, SO_BROADCAST,
                         reinterpret_cast<const char*>(&yes), sizeof(yes)) < 0) {
            errorMessage = "SO_BROADCAST failed: " + std::to_string(lastSocketError());
            CLOSE_SOCKET(fd);
            return false;
        }
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(targetPort));
    if (::inet_pton(AF_INET, targetIp.c_str(), &addr.sin_addr) != 1) {
        errorMessage = "Invalid IP: " + targetIp;
        CLOSE_SOCKET(fd);
        return false;
    }

    static_assert(sizeof(addr) <= sizeof(addr_), "sockaddr_in too large");
    std::memcpy(addr_, &addr, sizeof(addr));
    addrLen_ = sizeof(addr);
    socket_ = static_cast<int>(fd);
    return true;
}

bool UdpSender::send(const void* data, std::size_t bytes) {
    if (socket_ < 0) return false;
    const sockaddr* a = reinterpret_cast<const sockaddr*>(addr_);
#ifdef _WIN32
    int n = ::sendto(static_cast<socket_t>(socket_),
                     static_cast<const char*>(data), static_cast<int>(bytes),
                     0, a, static_cast<int>(addrLen_));
    return n == static_cast<int>(bytes);
#else
    ssize_t n = ::sendto(socket_, data, bytes, 0, a, static_cast<socklen_t>(addrLen_));
    return n == static_cast<ssize_t>(bytes);
#endif
}

} // namespace libera_timecode
