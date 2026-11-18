// zrfd: the radio daemon. One process owns the receiver; clients (the RF
// screens, scripts) talk to it over a Unix socket. Protocol: rf/README.md.
//
// The receiver is only open while someone is subscribed to its output, so a
// dongle draws streaming power only when it is being watched.
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "dsp/demod.h"
#include "dsp/spectrum.h"
#include "proto.h"
#include "sdr/source.h"

using namespace zrf;

namespace {

const size_t kNfft = 1024;
const int kMaxFftsPerFrame = 8;       // enough averaging; the rest of the samples are skipped
const size_t kMaxQueued = 256 * 1024; // per client; frames are dropped past this

struct Settings {
    double freq = 100e6;     // what the user is tuned to
    double offset = 0;       // added to freq for the hardware (IF tap, upconverter)
    double rate = 1.024e6;
    std::optional<double> gain;   // nullopt = automatic
    int fps = 15;
    Mode mode = Mode::Off;        // listening: demodulate the tuned frequency to audio
};

struct Shared {
    std::mutex mu;
    std::condition_variable cv;
    Settings want;
    bool dirty = true;
    int subscribers = 0;
    bool stop = false;
    // Latest output from the worker.
    uint64_t seq = 0;
    std::vector<float> frame;
    double frame_freq = 0, frame_rate = 0;
    std::string error;        // last open/read failure, "" when running
    uint64_t error_seq = 0;
    bool running = false;
    double audio_rate = 0;    // 0 when not listening
};

Shared g;
int g_wake[2] = {-1, -1};
std::atomic<bool> g_quit{false};

void wake_main() {
    char c = 1;
    (void)!write(g_wake[1], &c, 1);
}

void report_error(const std::string& msg) {
    std::lock_guard<std::mutex> lk(g.mu);
    g.error = msg;
    g.running = false;
    ++g.error_seq;
    wake_main();
}

// Where demodulated audio goes: "none", "file:<path>" (raw 16-bit mono) or
// "cmd:<shell command>" fed on its standard input, with %r replaced by the
// sample rate.
std::string g_audio_spec = "cmd:aplay -q -t raw -f S16_LE -c 1 -r %r";

class AudioSink {
public:
    ~AudioSink() { close(); }
    bool open(double rate) {
        close();
        if (g_audio_spec == "none") return true;
        if (g_audio_spec.rfind("file:", 0) == 0) {
            fd_ = ::open(g_audio_spec.c_str() + 5, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            return fd_ >= 0;
        }
        if (g_audio_spec.rfind("cmd:", 0) != 0) return false;
        std::string cmd = g_audio_spec.substr(4);
        size_t at = cmd.find("%r");
        if (at != std::string::npos) cmd.replace(at, 2, std::to_string(std::lround(rate)));
        int p[2];
        if (pipe2(p, O_CLOEXEC) != 0) return false;
        pid_ = fork();
        if (pid_ < 0) { ::close(p[0]); ::close(p[1]); return false; }
        if (pid_ == 0) {
            dup2(p[0], 0);
            execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
            _exit(127);
        }
        ::close(p[0]);
        fd_ = p[1];
        // Never let a stalled player hold up the receiver: drop audio instead.
        fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK);
        return true;
    }
    // False once the output has failed (for example the player exited).
    bool write(const std::vector<int16_t>& pcm) {
        if (fd_ < 0 || pcm.empty()) return true;
        ssize_t n = ::write(fd_, pcm.data(), pcm.size() * 2);
        if (n >= 0 || errno == EAGAIN || errno == EINTR) return true;
        close();
        return false;
    }
    void close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
        if (pid_ > 0) {
            kill(pid_, SIGTERM);
            waitpid(pid_, nullptr, 0);
        }
        pid_ = -1;
    }
private:
    int fd_ = -1;
    pid_t pid_ = -1;
};

void notify(const std::string& msg) {
    std::lock_guard<std::mutex> lk(g.mu);
    g.error = msg;
    ++g.error_seq;
    wake_main();
}

bool apply(Source& src, const Settings& s, std::string& err) {
    if (!src.set_rate(s.rate)) { err = "sample rate not supported"; return false; }
    if (!src.set_freq(s.freq + s.offset)) { err = "frequency out of range"; return false; }
    if (!src.set_gain(s.gain)) { err = "gain not supported"; return false; }
    return true;
}

void worker(std::string spec) {
    std::unique_ptr<Source> src;
    Spectrum spectrum(kNfft);
    std::vector<cf32> block(kNfft);
    Settings cur;
    std::unique_ptr<Demod> demod;
    AudioSink sink;
    std::vector<int16_t> pcm;
    Mode demod_mode = Mode::Off;
    double demod_rate = 0;
    auto stop_audio = [&] {
        demod.reset();
        sink.close();
        demod_mode = Mode::Off;
        std::lock_guard<std::mutex> lk(g.mu);
        g.audio_rate = 0;
    };
    size_t since_frame = 0;
    int ffts = 0;

    for (;;) {
        {
            std::unique_lock<std::mutex> lk(g.mu);
            if (g.stop) return;
            if (g.subscribers == 0) {
                // Nobody watching or listening: release the receiver and sleep.
                src.reset();
                demod.reset();
                sink.close();
                demod_mode = Mode::Off;
                g.audio_rate = 0;
                g.running = false;
                g.cv.wait(lk, [] { return g.stop || g.subscribers > 0; });
                continue;
            }
        }
        if (!src) {
            std::string err;
            src = make_source(spec, err);
            if (src && !src->open(err)) src.reset();
            if (src) {
                std::lock_guard<std::mutex> lk(g.mu);
                g.dirty = true;
                g.error.clear();
            } else {
                report_error(err);
                // Try again shortly: the dongle may be plugged in at any time.
                std::unique_lock<std::mutex> lk(g.mu);
                g.cv.wait_for(lk, std::chrono::seconds(2), [] { return g.stop || g.subscribers == 0; });
                continue;
            }
        }
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(g.mu);
            if (g.dirty) { cur = g.want; g.dirty = false; changed = true; }
        }
        if (changed) {
            std::string err;
            if (!apply(*src, cur, err)) {
                report_error(err);
                std::unique_lock<std::mutex> lk(g.mu);
                g.cv.wait(lk, [] { return g.stop || g.dirty || g.subscribers == 0; });
                continue;
            }
            spectrum.take();   // drop samples from the old tuning
            since_frame = 0;
            ffts = 0;
            // Retuning keeps the audio path; only a new mode or rate rebuilds it.
            if (cur.mode != demod_mode || cur.rate != demod_rate) {
                stop_audio();
                if (cur.mode != Mode::Off && cur.rate >= 64e3) {
                    demod = std::make_unique<Demod>(cur.mode, cur.rate);
                    demod_mode = cur.mode;
                    demod_rate = cur.rate;
                    if (!sink.open(demod->audio_rate())) notify("audio output failed");
                    std::lock_guard<std::mutex> lk(g.mu);
                    g.audio_rate = demod->audio_rate();
                }
            }
        }

        if (src->read(block.data(), kNfft) != kNfft) {
            src.reset();
            report_error("receiver stopped");
            continue;
        }
        if (demod) {
            pcm.clear();
            demod->run(block.data(), kNfft, pcm);
            if (!sink.write(pcm)) notify("audio output failed");
        }
        if (ffts < kMaxFftsPerFrame) { spectrum.add(block.data()); ++ffts; }
        since_frame += kNfft;
        if (since_frame >= size_t(cur.rate / cur.fps)) {
            std::vector<float> frame = spectrum.take();
            since_frame = 0;
            ffts = 0;
            std::lock_guard<std::mutex> lk(g.mu);
            g.frame = std::move(frame);
            g.frame_freq = cur.freq;
            g.frame_rate = cur.rate;
            g.running = true;
            g.error.clear();
            ++g.seq;
            wake_main();
        }
    }
}

struct Client {
    std::string in, out;
    size_t cols = 0;   // 0 = not subscribed to the spectrum
    bool listening = false;   // asked for audio; it stops when this client leaves
};

void change_subscribers(int delta) {
    std::lock_guard<std::mutex> lk(g.mu);
    g.subscribers += delta;
    g.cv.notify_all();
}

template <typename F>
void update(F&& f) {
    std::lock_guard<std::mutex> lk(g.mu);
    f(g.want);
    g.dirty = true;
    g.cv.notify_all();
}

std::string handle(Client& c, const std::string& line, const std::string& spec) {
    std::istringstream is(line);
    std::string cmd, arg;
    is >> cmd >> arg;
    char* end = nullptr;
    double num = arg.empty() ? 0 : std::strtod(arg.c_str(), &end);
    bool is_num = !arg.empty() && end && *end == 0;

    if (cmd == "hello") return "ok zrfd 1 source=" + spec;
    if (cmd == "tune") {
        if (!is_num || num < 0 || num > 6e9) return "err tune needs a frequency in Hz";
        update([&](Settings& s) { s.freq = num; });
        return "ok";
    }
    if (cmd == "offset") {
        if (!is_num || std::fabs(num) > 6e9) return "err offset needs a value in Hz";
        update([&](Settings& s) { s.offset = num; });
        return "ok";
    }
    if (cmd == "rate") {
        if (!is_num || num < 1e4 || num > 2e7) return "err rate needs a sample rate in Hz";
        update([&](Settings& s) { s.rate = num; });
        return "ok";
    }
    if (cmd == "gain") {
        if (arg == "auto") { update([](Settings& s) { s.gain.reset(); }); return "ok"; }
        if (!is_num) return "err gain needs dB or auto";
        update([&](Settings& s) { s.gain = num; });
        return "ok";
    }
    if (cmd == "spectrum") {
        if (arg == "off") {
            if (c.cols) change_subscribers(-1);
            c.cols = 0;
            return "ok";
        }
        if (!is_num || num < 1 || num > double(kNfft)) return "err spectrum needs 1.." + std::to_string(kNfft) + " columns or off";
        int fps = 0;
        if (is >> fps) {
            if (fps < 1 || fps > 30) return "err frames per second must be 1..30";
            update([&](Settings& s) { s.fps = fps; });
        }
        if (!c.cols) change_subscribers(+1);
        c.cols = size_t(num);
        return "ok";
    }
    if (cmd == "mode") {
        Mode m;
        if (!parse_mode(arg, m)) return "err mode needs off, wfm, nfm or am";
        if ((m != Mode::Off) != c.listening) change_subscribers(c.listening ? -1 : +1);
        c.listening = m != Mode::Off;
        update([&](Settings& s) { s.mode = m; });
        return "ok";
    }
    if (cmd == "status") {
        std::lock_guard<std::mutex> lk(g.mu);
        char buf[320];
        std::snprintf(buf, sizeof buf, "ok freq=%.0f offset=%.0f rate=%.0f gain=%s fps=%d mode=%s audio_rate=%.0f running=%d source=%s",
                      g.want.freq, g.want.offset, g.want.rate,
                      g.want.gain ? std::to_string(*g.want.gain).c_str() : "auto", g.want.fps,
                      mode_name(g.want.mode), g.audio_rate, g.running ? 1 : 0, spec.c_str());
        std::string out = buf;
        if (!g.error.empty()) out += " error=" + g.error;
        return out;
    }
    if (cmd.empty()) return "";
    return "err unknown command: " + cmd;
}

void on_signal(int) {
    g_quit = true;
    wake_main();
}

}  // namespace

int main(int argc, char** argv) {
    std::string spec = "rtlsdr", path = default_socket_path();
    if (const char* a = std::getenv("ZRF_AUDIO")) g_audio_spec = a;
    int idle_exit = 0;   // seconds with no client before exiting; 0 = stay
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--source" && i + 1 < argc) spec = argv[++i];
        else if (a == "--socket" && i + 1 < argc) path = argv[++i];
        else if (a == "--idle-exit" && i + 1 < argc) idle_exit = std::atoi(argv[++i]);
        else if (a == "--audio" && i + 1 < argc) g_audio_spec = argv[++i];
        else {
            std::fprintf(stderr, "usage: zrfd [--source sim|file:<path>|rtlsdr[:n]] [--socket <path>] [--idle-exit <seconds>]\n"
                                 "            [--audio none|file:<path>|cmd:<player command, %%r = sample rate>]\n");
            return a == "--help" ? 0 : 2;
        }
    }
    {
        std::string err;
        if (!make_source(spec, err)) { std::fprintf(stderr, "zrfd: %s\n", err.c_str()); return 2; }
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path) { std::fprintf(stderr, "zrfd: socket path too long\n"); return 2; }
    std::strcpy(addr.sun_path, path.c_str());
    int lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    unlink(path.c_str());
    mode_t old = umask(0077);   // only this user may control the radio
    bool bound = lfd >= 0 && bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0 && listen(lfd, 8) == 0;
    umask(old);
    if (!bound) { std::perror("zrfd: socket"); return 1; }
    if (pipe2(g_wake, O_NONBLOCK | O_CLOEXEC) != 0) { std::perror("zrfd: pipe"); return 1; }

    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::thread th(worker, spec);
    std::map<int, Client> clients;
    uint64_t sent_seq = 0, sent_error_seq = 0;
    auto last_client = std::chrono::steady_clock::now();

    while (!g_quit) {
        std::vector<pollfd> fds;
        fds.push_back({lfd, POLLIN, 0});
        fds.push_back({g_wake[0], POLLIN, 0});
        for (auto& [fd, c] : clients) fds.push_back({fd, short(POLLIN | (c.out.empty() ? 0 : POLLOUT)), 0});
        int timeout = -1;
        if (idle_exit > 0 && clients.empty()) {
            auto idle = std::chrono::steady_clock::now() - last_client;
            if (idle >= std::chrono::seconds(idle_exit)) break;
            timeout = int(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::seconds(idle_exit) - idle).count()) + 1;
        }
        if (poll(fds.data(), nfds_t(fds.size()), timeout) < 0 && errno != EINTR) break;

        if (fds[0].revents & POLLIN) {
            int fd;
            while ((fd = accept4(lfd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC)) >= 0) clients[fd];
        }
        if (fds[1].revents & POLLIN) {
            char buf[64];
            while (read(g_wake[0], buf, sizeof buf) > 0) {}
            std::vector<float> frame;
            double freq = 0, rate = 0;
            std::string error;
            {
                std::lock_guard<std::mutex> lk(g.mu);
                if (g.seq != sent_seq) { frame = g.frame; freq = g.frame_freq; rate = g.frame_rate; sent_seq = g.seq; }
                if (g.error_seq != sent_error_seq) { error = g.error; sent_error_seq = g.error_seq; }
            }
            for (auto& [fd, c] : clients) {
                if ((!c.cols && !c.listening) || c.out.size() > kMaxQueued) continue;
                if (!error.empty()) c.out += "E " + error + "\n";
                if (frame.empty() || !c.cols) continue;
                std::vector<float> cols = reduce_peak(frame, c.cols);
                std::vector<uint8_t> bytes(cols.size());
                for (size_t i = 0; i < cols.size(); ++i) bytes[i] = db_to_byte(cols[i]);
                char head[64];
                std::snprintf(head, sizeof head, "S %.0f %.0f ", freq, rate);
                c.out += head + to_hex(bytes) + "\n";
            }
        }

        std::vector<int> dead;
        for (size_t i = 2; i < fds.size(); ++i) {
            auto it = clients.find(fds[i].fd);
            if (it == clients.end()) continue;
            Client& c = it->second;
            bool closed = (fds[i].revents & (POLLERR | POLLNVAL)) != 0;
            if (fds[i].revents & (POLLIN | POLLHUP)) {
                char buf[1024];
                ssize_t n;
                while ((n = read(fds[i].fd, buf, sizeof buf)) > 0) c.in.append(buf, size_t(n));
                if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) closed = true;
                size_t nl;
                while ((nl = c.in.find('\n')) != std::string::npos) {
                    std::string reply = handle(c, c.in.substr(0, nl), spec);
                    c.in.erase(0, nl + 1);
                    if (!reply.empty()) c.out += reply + "\n";
                }
                if (c.in.size() > 4096) closed = true;   // not speaking our protocol
            }
            if (!closed && !c.out.empty()) {
                ssize_t n = write(fds[i].fd, c.out.data(), c.out.size());
                if (n > 0) c.out.erase(0, size_t(n));
                else if (n < 0 && errno != EAGAIN && errno != EINTR) closed = true;
            }
            if (closed) dead.push_back(fds[i].fd);
        }
        for (int fd : dead) {
            if (clients[fd].cols) change_subscribers(-1);
            bool was_listening = clients[fd].listening;
            if (was_listening) change_subscribers(-1);
            close(fd);
            clients.erase(fd);
            if (was_listening) {
                // Audio belongs to the client that asked for it.
                bool others = false;
                for (auto& [other_fd, other] : clients) others = others || other.listening;
                if (!others) update([](Settings& s) { s.mode = Mode::Off; });
            }
            last_client = std::chrono::steady_clock::now();
        }
    }

    {
        std::lock_guard<std::mutex> lk(g.mu);
        g.stop = true;
        g.cv.notify_all();
    }
    th.join();
    unlink(path.c_str());
    return 0;
}
