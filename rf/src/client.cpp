#include "client.h"

#include <cerrno>
#include <cstring>
#include <sstream>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "proto.h"

namespace zrf {

bool RadioClient::connect(const std::string& path_in) {
    close();
    std::string path = path_in.empty() ? default_socket_path() : path_in;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path) return false;
    std::strcpy(addr.sun_path, path.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    // Connecting to a local socket succeeds or fails at once.
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        ::close(fd);
        return false;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fd_ = fd;
    in_.clear();
    return true;
}

void RadioClient::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

void RadioClient::send(const std::string& line) {
    if (fd_ < 0) return;
    std::string out = line + "\n";
    // Commands are tiny; if even that can't be written the daemon is stuck or gone.
    if (::send(fd_, out.data(), out.size(), MSG_NOSIGNAL) != ssize_t(out.size())) close();
}

bool RadioClient::poll(int wait_ms, std::vector<SpectrumFrame>& frames, std::string& error) {
    if (fd_ < 0) return false;
    if (wait_ms > 0) {
        pollfd p{fd_, POLLIN, 0};
        ::poll(&p, 1, wait_ms);
    }
    char buf[8192];
    for (;;) {
        ssize_t n = read(fd_, buf, sizeof buf);
        if (n > 0) { in_.append(buf, size_t(n)); continue; }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
        close();   // end of stream or a real error
        return false;
    }
    size_t nl;
    while ((nl = in_.find('\n')) != std::string::npos) {
        std::string line = in_.substr(0, nl);
        in_.erase(0, nl + 1);
        if (line.rfind("S ", 0) == 0) {
            std::istringstream is(line.substr(2));
            SpectrumFrame f;
            std::string hex;
            if (is >> f.freq >> f.rate >> hex && from_hex(hex, f.cols)) {
                frames.push_back(std::move(f));
                error.clear();
            }
        } else if (line.rfind("E ", 0) == 0) {
            error = line.substr(2);
        } else if (line.rfind("err ", 0) == 0) {
            error = line.substr(4);
        }
    }
    return true;
}

bool spawn_daemon(const std::string& source, const std::string& socket_path) {
    const char* bin = std::getenv("ZRFD_BIN");
    std::string prog = bin && *bin ? bin : "zrfd";
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        // Detach fully: the daemon must outlive a restart of the UI, and must
        // not hold the UI's screen, keyboard or socket file descriptors.
        setsid();
        if (fork() != 0) _exit(0);
        for (int fd = 3; fd < 1024; ++fd) ::close(fd);
        if (socket_path.empty())
            execlp(prog.c_str(), prog.c_str(), "--source", source.c_str(), "--idle-exit", "10", nullptr);
        else
            execlp(prog.c_str(), prog.c_str(), "--source", source.c_str(), "--idle-exit", "10", "--socket",
                   socket_path.c_str(), nullptr);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return true;
}

}  // namespace zrf
