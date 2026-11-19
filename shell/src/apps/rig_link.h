// Talk to a transceiver through Hamlib's rigctld: read its frequency (so the
// spectrum can follow the dial) and set it. Frequency only: nothing here can
// key a transmitter.
//
// The rig is described in Station: Hamlib model number, serial port, speed.
// rigctld is started on demand and stopped when this object goes away; one
// that is already running on the port is used as it is.
#pragma once

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <string>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ui/prefs.h"

namespace zs {

class RigLink {
public:
    ~RigLink() { stop(); }

    static bool configured() { return prefs::get_int("rf.rig_model", 0) > 0; }
    bool active() const { return active_; }
    bool connected() const { return fd_ >= 0; }

    void start() {
        active_ = true;
        ticks_ = failures_ = 0;
        if (!try_connect()) spawn();
    }

    void stop() {
        active_ = false;
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
        if (pid_ > 0) {
            kill(pid_, SIGTERM);
            waitpid(pid_, nullptr, 0);
        }
        pid_ = -1;
    }

    // Call from a timer. True with the rig's frequency when a reading arrived.
    // `lost` is set once the rig has stopped answering for a few seconds.
    bool poll(double& hz, bool& lost) {
        lost = false;
        if (!active_) return false;
        if (fd_ < 0) {
            if (++ticks_ % 12 == 0 && !try_connect() && ++failures_ > 8) lost = true;
            return false;
        }
        if (++ticks_ % 8 == 0) send("f");   // about three times a second
        char buf[256];
        ssize_t n = read(fd_, buf, sizeof buf);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
            close(fd_);
            fd_ = -1;
            failures_ = 0;
            return false;
        }
        if (n > 0) in_.append(buf, size_t(n));
        bool got = false;
        size_t nl;
        while ((nl = in_.find('\n')) != std::string::npos) {
            std::string line = in_.substr(0, nl);
            in_.erase(0, nl + 1);
            // Frequency replies are bare numbers; "RPRT n" lines are status.
            char* end = nullptr;
            double v = std::strtod(line.c_str(), &end);
            if (end != line.c_str() && *end == 0 && v > 1e4) { hz = v; got = true; }
        }
        return got;
    }

    void set_freq(double hz) {
        char cmd[48];
        std::snprintf(cmd, sizeof cmd, "F %.0f", hz);
        send(cmd);
    }

private:
    static int port() { return prefs::get_int("rf.rigctld_port", 4532); }

    bool try_connect() {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(uint16_t(port()));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
            close(fd);
            return false;
        }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        fd_ = fd;
        in_.clear();
        return true;
    }

    void spawn() {
        std::string model = std::to_string(prefs::get_int("rf.rig_model", 0));
        std::string dev = prefs::get_str("rf.rig_port", "/dev/ttyUSB0");
        std::string baud = std::to_string(prefs::get_int("rf.rig_baud", 0));
        std::string p = std::to_string(port());
        pid_ = fork();
        if (pid_ != 0) return;
        for (int fd = 3; fd < 1024; ++fd) close(fd);
        // Listen on this device only: rig control is not offered to the network.
        if (baud == "0")
            execlp("rigctld", "rigctld", "-m", model.c_str(), "-r", dev.c_str(), "-T", "127.0.0.1", "-t", p.c_str(), nullptr);
        else
            execlp("rigctld", "rigctld", "-m", model.c_str(), "-r", dev.c_str(), "-s", baud.c_str(), "-T", "127.0.0.1", "-t",
                   p.c_str(), nullptr);
        _exit(127);
    }

    void send(const std::string& line) {
        if (fd_ < 0) return;
        std::string out = line + "\n";
        if (::send(fd_, out.data(), out.size(), MSG_NOSIGNAL) < 0 && errno != EAGAIN) {
            close(fd_);
            fd_ = -1;
        }
    }

    bool active_ = false;
    int fd_ = -1, ticks_ = 0, failures_ = 0;
    pid_t pid_ = -1;
    std::string in_;
};

}  // namespace zs
