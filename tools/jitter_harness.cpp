#include "Settings.h"
#include "Timecode.h"
#include "TimecodeEngine.h"
#include "outputs/ArtnetOutput.h"
#include "outputs/MidiOutput.h"
#include "outputs/OutputTiming.h"
#include "outputs/SmpteOutput.h"
#include "outputs/SntcOutput.h"
#include "outputs/UdpSender.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
#define CLOSE_SOCKET ::closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#define CLOSE_SOCKET ::close
#endif

namespace {

using namespace libera_timecode;
using Clock = std::chrono::steady_clock;

struct Options {
    double durationSeconds{60.0};
    std::vector<double> rates{0.95, 1.0, 1.05};
    FrameRate fps{FrameRate::fps_30_NDF};
    int artnetPort{16454};
    int sntcPort{18405};
    int cpuWorkers{0};
    double networkMbps{0.0};
    std::string reportPath{"jitter-report.csv"};
    bool enableSmpte{true};
    bool enableMidi{true};
    bool enableArtnet{true};
    bool enableSntc{true};
    double maxMeanJitterMs{std::numeric_limits<double>::infinity()};
    double maxP99JitterMs{std::numeric_limits<double>::infinity()};
    double maxJitterMs{std::numeric_limits<double>::infinity()};
    bool failOnDuplicates{false};
};

struct PhaseStats {
    std::string protocol;
    double rate{1.0};
    double expectedIntervalMs{0.0};
    uint64_t packets{0};
    uint64_t frameChanges{0};
    uint64_t duplicates{0};
    uint64_t backwards{0};
    uint64_t parseErrors{0};
    std::vector<double> intervalsMs;
    std::vector<double> absJitterMs;
};

std::string lowerNoSpace(std::string s) {
    s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) {
        return c == ' ' || c == '_' || c == '-';
    }), s.end());
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool parseFps(const std::string& value, FrameRate& out) {
    const std::string v = lowerNoSpace(value);
    if (v == "23.976" || v == "23976") { out = FrameRate::fps_23_976; return true; }
    if (v == "24") { out = FrameRate::fps_24; return true; }
    if (v == "25") { out = FrameRate::fps_25; return true; }
    if (v == "29.97df" || v == "2997df") { out = FrameRate::fps_29_97_DF; return true; }
    if (v == "29.97ndf" || v == "2997ndf" || v == "29.97") {
        out = FrameRate::fps_29_97_NDF;
        return true;
    }
    if (v == "30df") { out = FrameRate::fps_30_DF; return true; }
    if (v == "30ndf" || v == "30") { out = FrameRate::fps_30_NDF; return true; }
    return false;
}

std::vector<double> parseRates(const std::string& csv) {
    std::vector<double> out;
    std::stringstream ss(csv);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) out.push_back(std::stod(item));
    }
    return out;
}

void printUsage(const char* argv0) {
    std::cout
        << "Usage: " << argv0 << " [options]\n"
        << "  --duration SECONDS       seconds per rate phase (default 60)\n"
        << "  --rates CSV              playback rates (default 0.95,1.0,1.05)\n"
        << "  --fps FPS                23.976,24,25,29.97df,29.97ndf,30df,30ndf\n"
        << "  --cpu-workers N          busy CPU worker threads (default 0)\n"
        << "  --network-mbps N         loopback UDP load in Mbps (default 0)\n"
        << "  --report PATH            CSV report path (default jitter-report.csv)\n"
        << "  --artnet-port PORT       loopback Art-Net port (default 16454)\n"
        << "  --sntc-port PORT         loopback SNTC port (default 18405)\n"
        << "  --max-mean-jitter-ms N   fail if mean absolute jitter exceeds N\n"
        << "  --max-p99-jitter-ms N    fail if p99 absolute jitter exceeds N\n"
        << "  --max-jitter-ms N        fail if max absolute jitter exceeds N\n"
        << "  --fail-on-duplicates     fail if duplicate frame packets are seen\n"
        << "  --no-smpte|--no-midi|--no-artnet|--no-sntc\n";
}

bool parseArgs(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto needValue = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << name << " needs a value\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            std::exit(EXIT_SUCCESS);
        } else if (arg == "--duration") {
            if (const char* v = needValue("--duration")) opt.durationSeconds = std::stod(v);
        } else if (arg == "--rates") {
            if (const char* v = needValue("--rates")) opt.rates = parseRates(v);
        } else if (arg == "--fps") {
            if (const char* v = needValue("--fps"); v && !parseFps(v, opt.fps)) {
                std::cerr << "Unknown FPS: " << v << "\n";
                return false;
            }
        } else if (arg == "--cpu-workers") {
            if (const char* v = needValue("--cpu-workers")) opt.cpuWorkers = std::stoi(v);
        } else if (arg == "--network-mbps") {
            if (const char* v = needValue("--network-mbps")) opt.networkMbps = std::stod(v);
        } else if (arg == "--report") {
            if (const char* v = needValue("--report")) opt.reportPath = v;
        } else if (arg == "--artnet-port") {
            if (const char* v = needValue("--artnet-port")) opt.artnetPort = std::stoi(v);
        } else if (arg == "--sntc-port") {
            if (const char* v = needValue("--sntc-port")) opt.sntcPort = std::stoi(v);
        } else if (arg == "--max-mean-jitter-ms") {
            if (const char* v = needValue("--max-mean-jitter-ms")) {
                opt.maxMeanJitterMs = std::stod(v);
            }
        } else if (arg == "--max-p99-jitter-ms") {
            if (const char* v = needValue("--max-p99-jitter-ms")) {
                opt.maxP99JitterMs = std::stod(v);
            }
        } else if (arg == "--max-jitter-ms") {
            if (const char* v = needValue("--max-jitter-ms")) opt.maxJitterMs = std::stod(v);
        } else if (arg == "--fail-on-duplicates") {
            opt.failOnDuplicates = true;
        } else if (arg == "--no-smpte") {
            opt.enableSmpte = false;
        } else if (arg == "--no-midi") {
            opt.enableMidi = false;
        } else if (arg == "--no-artnet") {
            opt.enableArtnet = false;
        } else if (arg == "--no-sntc") {
            opt.enableSntc = false;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return false;
        }
    }
    if (opt.durationSeconds <= 0.0 || opt.rates.empty()) return false;
    return true;
}

#ifdef _WIN32
struct WsaInit {
    WsaInit() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    ~WsaInit() { WSACleanup(); }
};
#endif

struct ParsedPacket {
    bool ok{false};
    TimecodeFields tc;
};

ParsedPacket parseArtnet(const uint8_t* data, std::size_t len, FrameRate fps) {
    ParsedPacket p;
    if (len < 19) return p;
    const char magic[8] = {'A', 'r', 't', '-', 'N', 'e', 't', '\0'};
    if (!std::equal(magic, magic + 8, data)) return p;
    if (data[8] != 0x00 || data[9] != 0x97) return p;
    p.tc.frames = data[14];
    p.tc.seconds = data[15];
    p.tc.minutes = data[16];
    p.tc.hours = data[17];
    p.tc.dropFrame = frameRateInfo(fps).dropFrame;
    p.tc.integerFps = frameRateInfo(fps).integerFps;
    p.ok = true;
    return p;
}

ParsedPacket parseSntc(const uint8_t* data, std::size_t len, FrameRate fps) {
    ParsedPacket p;
    if (len < 16) return p;
    if (data[0] != 'S' || data[1] != 'N' || data[2] != 'T' || data[3] != 'C') return p;
    uint8_t cs = 0;
    for (int i = 0; i < 15; ++i) cs ^= data[i];
    if (cs != data[15]) return p;
    p.tc.hours = data[5];
    p.tc.minutes = data[6];
    p.tc.seconds = data[7];
    p.tc.frames = data[8];
    p.tc.dropFrame = frameRateInfo(fps).dropFrame;
    p.tc.integerFps = frameRateInfo(fps).integerFps;
    p.ok = true;
    return p;
}

class UdpRecorder {
public:
    using Parser = ParsedPacket (*)(const uint8_t*, std::size_t, FrameRate);

    UdpRecorder(std::string protocol, int port, FrameRate fps, Parser parser)
        : protocol_(std::move(protocol)), port_(port), fps_(fps), parser_(parser) {}

    bool start(std::string& error) {
        socket_t fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) {
            error = "socket() failed";
            return false;
        }

        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port_));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            error = "bind() failed on port " + std::to_string(port_);
            CLOSE_SOCKET(fd);
            return false;
        }

        socket_ = fd;
        stop_.store(false);
        thread_ = std::thread([this] { run(); });
        return true;
    }

    void stop() {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
        if (socket_ >= 0) {
            CLOSE_SOCKET(socket_);
            socket_ = -1;
        }
    }

    void begin(double rate, double expectedIntervalSeconds) {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_ = PhaseStats{};
        stats_.protocol = protocol_;
        stats_.rate = rate;
        stats_.expectedIntervalMs = expectedIntervalSeconds * 1000.0;
        lastFrame_ = -1;
        lastFrameTime_ = {};
        haveLastFrame_ = false;
        recording_ = true;
    }

    PhaseStats end() {
        std::lock_guard<std::mutex> lock(mutex_);
        recording_ = false;
        return stats_;
    }

private:
    void run() {
        std::array<uint8_t, 2048> buf{};
        while (!stop_.load()) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(socket_, &fds);
            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = 100000;
            const int ready = select(static_cast<int>(socket_ + 1), &fds, nullptr, nullptr, &tv);
            if (ready <= 0) continue;

            sockaddr_in from{};
#ifdef _WIN32
            int fromLen = sizeof(from);
            const int n = recvfrom(socket_, reinterpret_cast<char*>(buf.data()),
                                   static_cast<int>(buf.size()), 0,
                                   reinterpret_cast<sockaddr*>(&from), &fromLen);
#else
            socklen_t fromLen = sizeof(from);
            const ssize_t n = recvfrom(socket_, buf.data(), buf.size(), 0,
                                       reinterpret_cast<sockaddr*>(&from), &fromLen);
#endif
            if (n <= 0) continue;
            observe(buf.data(), static_cast<std::size_t>(n), Clock::now());
        }
    }

    void observe(const uint8_t* data, std::size_t len, Clock::time_point now) {
        const ParsedPacket parsed = parser_(data, len, fps_);
        std::lock_guard<std::mutex> lock(mutex_);
        if (!recording_) return;

        ++stats_.packets;
        if (!parsed.ok) {
            ++stats_.parseErrors;
            return;
        }

        const int frame = output_timing::labelFrameNumber(parsed.tc);
        if (haveLastFrame_ && frame == lastFrame_) {
            ++stats_.duplicates;
            return;
        }
        if (haveLastFrame_ && frame < lastFrame_) {
            ++stats_.backwards;
        }

        ++stats_.frameChanges;
        if (haveLastFrame_) {
            const double intervalMs =
                std::chrono::duration<double, std::milli>(now - lastFrameTime_).count();
            stats_.intervalsMs.push_back(intervalMs);
            stats_.absJitterMs.push_back(std::fabs(intervalMs - stats_.expectedIntervalMs));
        }

        haveLastFrame_ = true;
        lastFrame_ = frame;
        lastFrameTime_ = now;
    }

    std::string protocol_;
    int port_;
    FrameRate fps_;
    Parser parser_;
    socket_t socket_{-1};
    std::atomic<bool> stop_{false};
    std::thread thread_;

    std::mutex mutex_;
    bool recording_{false};
    PhaseStats stats_;
    int lastFrame_{-1};
    Clock::time_point lastFrameTime_{};
    bool haveLastFrame_{false};
};

void cpuWorker(std::atomic<bool>& stop) {
    volatile double x = 0.0;
    while (!stop.load(std::memory_order_relaxed)) {
        for (int i = 1; i < 5000; ++i) {
            x += std::sin(static_cast<double>(i)) * std::cos(x + i);
        }
    }
}

void networkLoadWorker(std::atomic<bool>& stop, double mbps) {
    UdpSender sender;
    std::string err;
    if (!sender.configure("127.0.0.1", 19191, err)) {
        std::cerr << "network load disabled: " << err << "\n";
        return;
    }

    std::vector<uint8_t> payload(1200, 0xA5);
    const double bytesPerSecond = mbps * 1000.0 * 1000.0 / 8.0;
    const auto interval = std::chrono::duration<double>(payload.size() / bytesPerSecond);
    auto next = Clock::now();
    while (!stop.load(std::memory_order_relaxed)) {
        sender.send(payload.data(), payload.size());
        next += std::chrono::duration_cast<Clock::duration>(interval);
        std::this_thread::sleep_until(next);
        if (Clock::now() > next + std::chrono::seconds(1)) next = Clock::now();
    }
}

double mean(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
}

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const auto idx = static_cast<std::size_t>(
        std::clamp(p, 0.0, 1.0) * static_cast<double>(v.size() - 1));
    return v[idx];
}

double maxOrZero(const std::vector<double>& v) {
    return v.empty() ? 0.0 : *std::max_element(v.begin(), v.end());
}

void writeReport(const std::string& path, const std::vector<PhaseStats>& rows) {
    std::ofstream out(path, std::ios::trunc);
    out << "protocol,rate,expected_ms,packets,frame_changes,duplicates,backwards,"
           "parse_errors,mean_interval_ms,p95_interval_ms,p99_interval_ms,"
           "max_interval_ms,mean_abs_jitter_ms,p95_abs_jitter_ms,p99_abs_jitter_ms,"
           "max_abs_jitter_ms\n";
    out << std::fixed << std::setprecision(6);
    for (const auto& s : rows) {
        out << s.protocol << ','
            << s.rate << ','
            << s.expectedIntervalMs << ','
            << s.packets << ','
            << s.frameChanges << ','
            << s.duplicates << ','
            << s.backwards << ','
            << s.parseErrors << ','
            << mean(s.intervalsMs) << ','
            << percentile(s.intervalsMs, 0.95) << ','
            << percentile(s.intervalsMs, 0.99) << ','
            << maxOrZero(s.intervalsMs) << ','
            << mean(s.absJitterMs) << ','
            << percentile(s.absJitterMs, 0.95) << ','
            << percentile(s.absJitterMs, 0.99) << ','
            << maxOrZero(s.absJitterMs) << '\n';
    }
}

void printSummary(const std::vector<PhaseStats>& rows) {
    std::cout << std::fixed << std::setprecision(3);
    for (const auto& s : rows) {
        std::cout << s.protocol
                  << " rate=" << s.rate
                  << " packets=" << s.packets
                  << " frames=" << s.frameChanges
                  << " dup=" << s.duplicates
                  << " backwards=" << s.backwards
                  << " parse_errors=" << s.parseErrors
                  << " mean_jitter_ms=" << mean(s.absJitterMs)
                  << " p99_jitter_ms=" << percentile(s.absJitterMs, 0.99)
                  << " max_jitter_ms=" << maxOrZero(s.absJitterMs)
                  << "\n";
    }
}

bool evaluateThresholds(const Options& opt, const std::vector<PhaseStats>& rows) {
    bool failed = false;
    for (const auto& s : rows) {
        auto fail = [&](const std::string& reason) {
            failed = true;
            std::cerr << "FAIL " << s.protocol << " rate=" << s.rate
                      << ": " << reason << "\n";
        };

        if (s.frameChanges == 0) fail("no frame changes recorded");
        if (s.parseErrors != 0) fail("parse_errors=" + std::to_string(s.parseErrors));
        if (s.backwards != 0) fail("backwards=" + std::to_string(s.backwards));
        if (opt.failOnDuplicates && s.duplicates != 0) {
            fail("duplicates=" + std::to_string(s.duplicates));
        }

        const double meanJitter = mean(s.absJitterMs);
        const double p99Jitter = percentile(s.absJitterMs, 0.99);
        const double maxJitter = maxOrZero(s.absJitterMs);
        if (meanJitter > opt.maxMeanJitterMs) {
            fail("mean_abs_jitter_ms=" + std::to_string(meanJitter)
                 + " > " + std::to_string(opt.maxMeanJitterMs));
        }
        if (p99Jitter > opt.maxP99JitterMs) {
            fail("p99_abs_jitter_ms=" + std::to_string(p99Jitter)
                 + " > " + std::to_string(opt.maxP99JitterMs));
        }
        if (maxJitter > opt.maxJitterMs) {
            fail("max_abs_jitter_ms=" + std::to_string(maxJitter)
                 + " > " + std::to_string(opt.maxJitterMs));
        }
    }
    return !failed;
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    WsaInit wsa;
#endif

    Options opt;
    if (!parseArgs(argc, argv, opt)) {
        printUsage(argv[0]);
        return EXIT_FAILURE;
    }

    std::string err;
    UdpRecorder artnetRec("artnet", opt.artnetPort, opt.fps, parseArtnet);
    UdpRecorder sntcRec("sntc", opt.sntcPort, opt.fps, parseSntc);
    if (opt.enableArtnet && !artnetRec.start(err)) {
        std::cerr << err << "\n";
        return EXIT_FAILURE;
    }
    if (opt.enableSntc && !sntcRec.start(err)) {
        std::cerr << err << "\n";
        return EXIT_FAILURE;
    }

    TimecodeEngine engine;
    engine.seek(0.0);

    SmpteOutput smpte(engine);
    MidiOutput midi(engine);
    ArtnetOutput artnet(engine);
    SntcOutput sntc(engine);

    if (opt.enableSmpte) {
        SmpteSettings s;
        s.enabled = true;
        s.fps = opt.fps;
        s.level = 0.0f; // run the real callback path silently.
        smpte.applyConfig(s);
    }
    if (opt.enableMidi) {
        MidiSettings m;
        m.enabled = true;
        m.fps = opt.fps;
        m.createVirtualPort = true;
        midi.applyConfig(m);
    }
    if (opt.enableArtnet) {
        ArtnetSettings a;
        a.enabled = true;
        a.fps = opt.fps;
        a.targetIp = "127.0.0.1";
        a.port = opt.artnetPort;
        artnet.applyConfig(a);
    }
    if (opt.enableSntc) {
        SntcSettings s;
        s.enabled = true;
        s.fps = opt.fps;
        s.targetIp = "127.0.0.1";
        s.port = opt.sntcPort;
        sntc.applyConfig(s);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(750));
    std::cout << "Output status:\n"
              << "  SMPTE:   " << smpte.status() << "\n"
              << "  MIDI:    " << midi.status() << "\n"
              << "  Art-Net: " << artnet.status() << "\n"
              << "  SNTC:    " << sntc.status() << "\n";

    std::atomic<bool> loadStop{false};
    std::vector<std::thread> loadThreads;
    for (int i = 0; i < opt.cpuWorkers; ++i) {
        loadThreads.emplace_back(cpuWorker, std::ref(loadStop));
    }
    if (opt.networkMbps > 0.0) {
        loadThreads.emplace_back(networkLoadWorker, std::ref(loadStop), opt.networkMbps);
    }

    std::vector<PhaseStats> allStats;
    const double nominalFps = frameRateInfo(opt.fps).nominalFps;

    for (double rate : opt.rates) {
        std::cout << "Running rate " << rate << " for "
                  << opt.durationSeconds << " seconds...\n";
        engine.pause();
        engine.seek(0.0);
        engine.setPlaybackRate(rate);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        const double expectedInterval = 1.0 / std::max(1.0, nominalFps * rate);
        if (opt.enableArtnet) artnetRec.begin(rate, expectedInterval);
        if (opt.enableSntc) sntcRec.begin(rate, expectedInterval);

        engine.play();
        std::this_thread::sleep_for(
            std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(opt.durationSeconds)));
        engine.pause();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (opt.enableArtnet) allStats.push_back(artnetRec.end());
        if (opt.enableSntc) allStats.push_back(sntcRec.end());
    }

    loadStop.store(true);
    for (auto& t : loadThreads) {
        if (t.joinable()) t.join();
    }

    smpte.stop();
    midi.stop();
    artnet.stop();
    sntc.stop();
    artnetRec.stop();
    sntcRec.stop();

    writeReport(opt.reportPath, allStats);
    printSummary(allStats);
    std::cout << "Wrote " << opt.reportPath << "\n";

    return evaluateThresholds(opt, allStats) ? EXIT_SUCCESS : EXIT_FAILURE;
}
