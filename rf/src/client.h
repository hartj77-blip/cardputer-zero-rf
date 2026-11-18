// Non-blocking client for the radio daemon, for use from a UI loop.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace zrf {

struct SpectrumFrame {
    double freq = 0, rate = 0;
    std::vector<uint8_t> cols;   // see proto.h for the level scale
};

class RadioClient {
public:
    ~RadioClient() { close(); }
    bool connected() const { return fd_ >= 0; }
    // "" = the default socket path.
    bool connect(const std::string& path = "");
    void close();
    void send(const std::string& line);
    // Collect what the daemon has sent, waiting up to wait_ms for something
    // to arrive. `error` is set to the latest problem the daemon reported.
    // False if the connection is gone.
    bool poll(int wait_ms, std::vector<SpectrumFrame>& frames, std::string& error);

private:
    int fd_ = -1;
    std::string in_;
};

// Start the daemon in the background. It exits by itself once no client has
// been connected for a while. `source` is a source spec such as "sim".
// The program run is $ZRFD_BIN, or zrfd from PATH.
bool spawn_daemon(const std::string& source, const std::string& socket_path = "");

}  // namespace zrf
