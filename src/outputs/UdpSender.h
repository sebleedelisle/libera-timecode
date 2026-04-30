#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace libera_timecode {

// Minimal UDP unicast/broadcast sender. Re-opens the socket whenever
// configure() is called.
class UdpSender {
public:
    UdpSender();
    ~UdpSender();
    UdpSender(const UdpSender&) = delete;
    UdpSender& operator=(const UdpSender&) = delete;

    // Returns true on success. errorMessage is populated on failure.
    bool configure(const std::string& targetIp, int targetPort, std::string& errorMessage);
    void close();

    // Returns true on success.
    bool send(const void* data, std::size_t bytes);

    bool ok() const { return socket_ >= 0; }
    const std::string& targetIp() const { return targetIp_; }
    int targetPort() const { return targetPort_; }

private:
    int socket_{-1};
    std::string targetIp_;
    int targetPort_{0};
    // sockaddr_in stored as raw bytes to avoid leaking platform headers.
    unsigned char addr_[64]{};
    std::size_t addrLen_{0};
    bool isBroadcast_{false};
};

} // namespace libera_timecode
