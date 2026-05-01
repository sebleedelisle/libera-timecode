#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace libera_timecode {

// UDP sender that fans out each send() to one or more destinations from a
// single socket. Re-opens the socket whenever configure() is called.
class UdpSender {
public:
    UdpSender();
    ~UdpSender();
    UdpSender(const UdpSender&) = delete;
    UdpSender& operator=(const UdpSender&) = delete;

    // Targets is a comma-/whitespace-separated list of IPv4 addresses.
    // Any "255.255.255.255" entry enables SO_BROADCAST on the socket.
    bool configure(const std::string& targetsCsv, int targetPort, std::string& errorMessage);
    void close();

    // Sends one packet to every configured target. Returns true if at least
    // one sendto() succeeded.
    bool send(const void* data, std::size_t bytes);

    bool ok() const { return socket_ >= 0 && !targets_.empty(); }
    int targetPort() const { return targetPort_; }
    std::size_t targetCount() const { return targets_.size(); }

private:
    struct Target {
        std::string ip;
        unsigned char addr[64]{};
        std::size_t addrLen{0};
    };

    int socket_{-1};
    int targetPort_{0};
    std::vector<Target> targets_;
};

} // namespace libera_timecode
