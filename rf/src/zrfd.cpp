// zrfd: the radio daemon. One process owns the receiver; clients (the RF
// screens, scripts) talk to it over a Unix socket. Protocol: rf/README.md.
//
// The receiver is only open while someone is subscribed to its output, so a
// dongle draws streaming power only when it is being watched.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "dsp/demod.h"
#include "dsp/spectrum.h"
#include "ft8.h"
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
    int ppm = 0;                  // crystal correction, parts per million
    bool bias_tee = false;        // power an amplifier up the coax
    std::optional<double> squelch;   // mute audio unless the signal is this many dB above the noise
    bool ft8 = false;             // decode FT8 on the tuned (dial) frequency, upper sideband
    std::string record;           // file to write raw IQ to; "" = not recording
    std::string tool;             // hand the receiver to another program: "sensors", "aircraft", "aprs", "server"; "" = none
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
    std::vector<std::string> decoded;   // decoder output lines not yet sent to clients
    double recorded_bytes = 0;
    double signal_db = 0;     // tuned signal above the noise floor, from the last frame
    bool squelch_open = true;
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
// Default: sox's play, which the base OS's Media app also uses; else aplay.
std::string g_audio_spec =
    "cmd:if command -v play >/dev/null; then exec play -q -t raw -r %r -e signed -b 16 -c 1 -; "
    "else exec aplay -q -t raw -f S16_LE -c 1 -r %r; fi";

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
        for (size_t at; (at = cmd.find("%r")) != std::string::npos;)
            cmd.replace(at, 2, std::to_string(std::lround(rate)));
        int p[2];
        if (pipe2(p, O_CLOEXEC) != 0) return false;
        pid_ = fork();
        if (pid_ < 0) { ::close(p[0]); ::close(p[1]); return false; }
        if (pid_ == 0) {
            dup2(p[0], 0);
            setpgid(0, 0);   // so close() can stop the shell and the player together
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
            kill(-pid_, SIGTERM);
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

// A decoder program run in place of our own DSP; we read its output lines.
class Decoder {
public:
    ~Decoder() { stop(); }
    bool running() const { return pid_ > 0; }
    bool start(const std::string& cmd) {
        stop();
        int p[2];
        if (pipe2(p, O_CLOEXEC) != 0) return false;
        pid_ = fork();
        if (pid_ < 0) { ::close(p[0]); ::close(p[1]); return false; }
        if (pid_ == 0) {
            dup2(p[1], 1);
            setpgid(0, 0);
            execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
            _exit(127);
        }
        ::close(p[1]);
        fd_ = p[0];
        buf_.clear();
        started_ = std::chrono::steady_clock::now();
        return true;
    }
    // Wait up to ms for output and split it into lines. False once the
    // program has closed its output (it exited).
    bool pump(int ms, std::vector<std::string>& lines) {
        pollfd p{fd_, POLLIN, 0};
        if (poll(&p, 1, ms) <= 0) return true;
        char buf[4096];
        ssize_t n = read(fd_, buf, sizeof buf);
        if (n <= 0) return n < 0 && errno == EINTR;
        buf_.append(buf, size_t(n));
        size_t nl;
        while ((nl = buf_.find('\n')) != std::string::npos) {
            if (nl > 0) lines.push_back(buf_.substr(0, nl));
            buf_.erase(0, nl + 1);
        }
        if (buf_.size() > 65536) buf_.clear();
        return true;
    }
    // Returns the program's exit code, or -1 if it had to be stopped.
    // exiting: its output has already closed, so it is on its way out.
    int stop(bool exiting = false) {
        int code = -1;
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
        if (pid_ > 0) {
            int status = 0;
            // A program's output closes a moment before it can be collected:
            // give one that is already exiting time to report how it ended.
            pid_t got = 0;
            for (int i = 0; i < 20 && (got = waitpid(pid_, &status, WNOHANG)) == 0 && exiting; ++i) usleep(10 * 1000);
            if (got == pid_) {
                if (WIFEXITED(status)) code = WEXITSTATUS(status);
            } else {
                kill(-pid_, SIGTERM);
                kill(pid_, SIGTERM);
                waitpid(pid_, &status, 0);
            }
        }
        pid_ = -1;
        return code;
    }
    double seconds_running() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
    }
private:
    int fd_ = -1;
    pid_t pid_ = -1;
    std::string buf_;
    std::chrono::steady_clock::time_point started_;
};

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

// The outside program for a job on this kind of receiver. Each opens the
// dongle itself, which is why ours must be closed first. "" = not possible.
std::string tool_command(const std::string& spec, const std::string& tool, double freq) {
    std::string env = "ZRF_" + tool + "_CMD";
    for (char& c : env) c = char(std::toupper(static_cast<unsigned char>(c)));
    if (const char* c = std::getenv(env.c_str())) return c;

    bool sim = spec == "sim", file = spec.rfind("file:", 0) == 0;
    std::string index = !sim && !file && spec.size() > 7 ? std::to_string(std::atoi(spec.c_str() + 7)) : "0";
    char self[512];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    std::string dir = n > 0 ? std::string(self, size_t(n)) : "";
    dir = dir.substr(0, dir.rfind('/') + 1);

    if (tool == "sensors") {
        // Weather stations, tyre sensors, remotes: rtl_433, JSON lines out.
        const std::string opts = " -F json -M level -C si 2>/dev/null";
        if (sim) return "exec " + shell_quote(dir + "zrf-sim-sensors");
        if (file) return "exec rtl_433 -r " + shell_quote(spec.substr(5)) + opts;
        return "exec rtl_433 -d " + index + " -f " + std::to_string(std::llround(freq)) + opts;
    }
    if (tool == "aircraft") {
        // ADS-B at 1090 MHz, raw frames out. dump1090 hears more when it is
        // installed; rtl_adsb comes with the rtl-sdr tools.
        if (sim) return "exec " + shell_quote(dir + "zrf-sim-aircraft");
        if (file) return "";
        return "if command -v dump1090-mutability >/dev/null; then exec dump1090-mutability --device-index " + index +
               " --raw 2>/dev/null; else exec rtl_adsb -d " + index + " 2>/dev/null; fi";
    }
    if (tool == "aprs") {
        // Amateur packet radio: FM audio from rtl_fm into multimon-ng's
        // 1200 baud decoder. Two programs, stopped together as one group.
        if (sim) return "exec " + shell_quote(dir + "zrf-sim-aprs");
        if (file) return "";
        return "rtl_fm -d " + index + " -f " + std::to_string(std::llround(freq)) +
               " -M fm -s 22050 -l 0 - 2>/dev/null | multimon-ng -t raw -a AFSK1200 -A - 2>/dev/null";
    }
    if (tool == "server") {
        // Serve the dongle to SDR programs on other machines.
        if (sim) return "echo 'simulated server: nothing is listening'; exec sleep 86400";
        if (file) return "";
        return "exec rtl_tcp -a 0.0.0.0 -p 1234 -d " + index + " 2>&1";
    }
    return "";
}

// Raw IQ to a file, as unsigned 8-bit pairs (the format rtl_sdr writes and
// most tools read), with a small SigMF description next to it.
class Recorder {
public:
    ~Recorder() { close(); }
    bool active() const { return f_ != nullptr; }
    const std::string& path() const { return path_; }
    double bytes() const { return bytes_; }
    bool open(const std::string& path, double freq, double rate) {
        close();
        f_ = std::fopen(path.c_str(), "wb");
        if (!f_) return false;
        path_ = path;
        bytes_ = 0;
        size_t slash = path.rfind('/'), dot = path.rfind('.');
        std::string base = dot != std::string::npos && (slash == std::string::npos || dot > slash) ? path.substr(0, dot) : path;
        std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
        if (FILE* m = std::fopen((base + ".sigmf-meta").c_str(), "w")) {
            char when[32];
            time_t t = time(nullptr);
            strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%SZ", gmtime(&t));
            std::fprintf(m,
                         "{\n  \"global\": {\"core:datatype\": \"cu8\", \"core:sample_rate\": %.0f, "
                         "\"core:version\": \"1.0.0\", \"core:dataset\": \"%s\", \"core:recorder\": \"zrfd\"},\n"
                         "  \"captures\": [{\"core:sample_start\": 0, \"core:frequency\": %.0f, \"core:datetime\": \"%s\"}],\n"
                         "  \"annotations\": []\n}\n",
                         rate, name.c_str(), freq, when);
            std::fclose(m);
        }
        return true;
    }
    // False when writing failed or the disk is nearly full; the file is closed.
    bool write(const cf32* in, size_t n) {
        buf_.resize(n * 2);
        for (size_t i = 0; i < n; ++i) {
            buf_[2 * i] = uint8_t(std::clamp(std::lround(in[i].real() * 127.5f + 127.5f), 0L, 255L));
            buf_[2 * i + 1] = uint8_t(std::clamp(std::lround(in[i].imag() * 127.5f + 127.5f), 0L, 255L));
        }
        bool ok = std::fwrite(buf_.data(), 1, buf_.size(), f_) == buf_.size();
        bytes_ += double(buf_.size());
        // Check free space about once a second's worth of data.
        if (ok && (since_check_ += buf_.size()) > 2000000) {
            since_check_ = 0;
            struct statvfs v {};
            if (statvfs(path_.c_str(), &v) == 0 && double(v.f_bavail) * double(v.f_frsize) < 200e6) ok = false;
        }
        if (!ok) close();
        return ok;
    }
    void close() {
        if (f_) std::fclose(f_);
        f_ = nullptr;
    }
private:
    FILE* f_ = nullptr;
    std::string path_;
    std::vector<uint8_t> buf_;
    double bytes_ = 0;
    size_t since_check_ = 0;
};

bool apply(Source& src, const Settings& s, std::string& err) {
    if (!src.set_rate(s.rate)) { err = "sample rate not supported"; return false; }
    if (!src.set_freq(s.freq + s.offset)) { err = "frequency out of range"; return false; }
    if (!src.set_gain(s.gain)) { err = "gain not supported"; return false; }
    if (!src.set_ppm(s.ppm)) { err = "frequency correction not supported"; return false; }
    if (!src.set_bias_tee(s.bias_tee)) { err = "antenna power not supported by this receiver"; return false; }
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
    std::vector<float> sorted;
    bool squelch_open = true;
    int frames_below = 5;
    // FT8: our own upper-sideband audio, collected one 15-second slot at a
    // time by the real clock and decoded on a helper thread while the next
    // slot is already being received.
    std::unique_ptr<Demod> ft8_demod;
    double ft8_rate = 0, ft8_freq = 0;
    std::vector<int16_t> ft8_pcm;
    std::vector<float> ft8_audio;
    int64_t ft8_slot = -1;
    bool ft8_whole = false;   // this slot was received from its start, on one frequency
    std::thread ft8_thread;
    auto stop_ft8 = [&] {
        ft8_demod.reset();
        ft8_slot = -1;
        if (ft8_thread.joinable()) ft8_thread.join();
    };
    Recorder recorder;
    Decoder decoder;
    double decoder_freq = 0;
    std::string decoder_tool;
    double rec_freq = 0, rec_rate = 0;
    std::vector<std::string> lines;

    for (;;) {
        {
            std::unique_lock<std::mutex> lk(g.mu);
            if (g.stop) {
                lk.unlock();
                stop_ft8();
                return;
            }
            if (g.subscribers == 0) {
                // Nobody watching or listening: release the receiver and sleep.
                src.reset();
                ft8_demod.reset();
                ft8_slot = -1;
                recorder.close();
                g.recorded_bytes = 0;
                demod.reset();
                sink.close();
                demod_mode = Mode::Off;
                g.audio_rate = 0;
                g.running = false;
                lk.unlock();
                decoder.stop();
                lk.lock();
                g.cv.wait(lk, [] { return g.stop || g.subscribers > 0; });
                continue;
            }
        }
        std::string want_tool;
        double want_freq;
        {
            std::lock_guard<std::mutex> lk(g.mu);
            want_tool = g.want.tool;
            // The sensor and packet decoders follow the tuned frequency.
            want_freq = want_tool == "sensors" || want_tool == "aprs" ? g.want.freq : 0;
        }
        if (!want_tool.empty()) {
            // The other program needs the receiver to itself.
            if (src) { src.reset(); stop_audio(); recorder.close(); stop_ft8(); }
            if (!decoder.running() || decoder_freq != want_freq || decoder_tool != want_tool) {
                std::string cmd = tool_command(spec, want_tool, want_freq);
                if (cmd.empty() || !decoder.start(cmd)) {
                    report_error(cmd.empty() ? "not available with this receiver" : "can't start the program");
                    std::unique_lock<std::mutex> lk(g.mu);
                    g.cv.wait_for(lk, std::chrono::seconds(2), [&] { return g.stop || g.subscribers == 0 || g.want.tool != want_tool; });
                    continue;
                }
                decoder_freq = want_freq;
                decoder_tool = want_tool;
                std::lock_guard<std::mutex> lk(g.mu);
                g.running = true;
                g.error.clear();
            }
            lines.clear();
            bool alive = decoder.pump(200, lines);
            if (!lines.empty()) {
                std::lock_guard<std::mutex> lk(g.mu);
                for (auto& l : lines)
                    if (g.decoded.size() < 500) g.decoded.push_back(std::move(l));
                wake_main();
            }
            if (!alive) {
                double ran = decoder.seconds_running();
                int code = decoder.stop(true);
                // These programs give up at once when they find no dongle.
                bool dongle = spec.rfind("rtlsdr", 0) == 0;
                report_error(code == 127 ? "program not installed" : dongle && ran < 5 ? "no dongle" : "program stopped");
                std::unique_lock<std::mutex> lk(g.mu);
                g.cv.wait_for(lk, std::chrono::seconds(2), [&] { return g.stop || g.subscribers == 0 || g.want.tool != want_tool; });
            }
            continue;
        }
        if (decoder.running()) {
            decoder.stop();
            std::lock_guard<std::mutex> lk(g.mu);
            g.dirty = true;
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
            // A recording is one frequency and rate: retuning ends it, and a
            // new path starts one.
            if (recorder.active() && (cur.record != recorder.path() || cur.freq != rec_freq || cur.rate != rec_rate)) {
                recorder.close();
                if (cur.record == recorder.path()) {
                    notify("recording stopped: retuned");
                    std::lock_guard<std::mutex> lk(g.mu);
                    g.want.record.clear();
                    cur.record.clear();
                }
            }
            if (!cur.record.empty() && !recorder.active()) {
                if (recorder.open(cur.record, cur.freq, cur.rate)) {
                    rec_freq = cur.freq;
                    rec_rate = cur.rate;
                } else {
                    notify("can't write the recording");
                    std::lock_guard<std::mutex> lk(g.mu);
                    g.want.record.clear();
                    cur.record.clear();
                }
            }
            // FT8 needs its own demodulator; a retune spoils the slot in progress.
            if (!cur.ft8) {
                if (ft8_demod) stop_ft8();
            } else if (!ft8_demod || ft8_rate != cur.rate) {
                if (cur.rate >= 64e3) {
                    ft8_demod = std::make_unique<Demod>(Mode::Usb, cur.rate);
                    ft8_rate = cur.rate;
                    ft8_slot = -1;
                }
            }
            if (ft8_freq != cur.freq) ft8_whole = false;
            ft8_freq = cur.freq;
            // With squelch on, stay muted until a frame shows a signal here.
            frames_below = 5;
            squelch_open = !cur.squelch;
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
            // Don't spin if it keeps opening and then failing.
            std::unique_lock<std::mutex> lk(g.mu);
            g.cv.wait_for(lk, std::chrono::seconds(2), [] { return g.stop || g.subscribers == 0; });
            continue;
        }
        if (demod) {
            pcm.clear();
            demod->run(block.data(), kNfft, pcm);
            if (cur.squelch && !squelch_open) std::fill(pcm.begin(), pcm.end(), int16_t(0));
            if (!sink.write(pcm)) notify("audio output failed");
        }
        if (ft8_demod) {
            ft8_pcm.clear();
            ft8_demod->run(block.data(), kNfft, ft8_pcm);
            double audio_rate = ft8_demod->audio_rate();
            double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
            int64_t slot = int64_t(std::floor(now / kFt8SlotSeconds));
            if (slot != ft8_slot) {
                // A slot just ended: decode it if we heard all of it.
                if (ft8_slot >= 0 && ft8_whole && ft8_audio.size() >= size_t(13.3 * audio_rate)) {
                    if (ft8_thread.joinable()) ft8_thread.join();
                    ft8_thread = std::thread([audio = std::move(ft8_audio), rate = int(std::lround(audio_rate)), start = ft8_slot] {
                        std::vector<Ft8Message> found = ft8_decode(audio.data(), audio.size(), rate);
                        time_t t = time_t(start * 15);
                        tm utc{};
                        gmtime_r(&t, &utc);
                        std::lock_guard<std::mutex> lk(g.mu);
                        for (const Ft8Message& m : found) {
                            char line[160];
                            std::snprintf(line, sizeof line, "FT8 %02d%02d%02d %d %.1f %.0f %s", utc.tm_hour, utc.tm_min, utc.tm_sec,
                                          m.snr_db, m.dt, m.freq_hz, m.text.c_str());
                            if (g.decoded.size() < 500) g.decoded.push_back(line);
                        }
                        // An empty slot is news too: it tells the screen the decoder is alive.
                        char done[48];
                        std::snprintf(done, sizeof done, "FT8 %02d%02d%02d done %zu", utc.tm_hour, utc.tm_min, utc.tm_sec, found.size());
                        if (g.decoded.size() < 500) g.decoded.push_back(done);
                        wake_main();
                    });
                }
                ft8_audio.clear();
                ft8_slot = slot;
                // Line the audio up with the slot: fill in what was missed at
                // its start, and give up on the slot if that was much.
                double into = now - double(slot) * kFt8SlotSeconds;
                ft8_whole = into < 1.0;
                ft8_audio.assign(size_t(std::max(0.0, into) * audio_rate), 0.0f);
            }
            if (ft8_audio.size() < size_t(15.5 * audio_rate))
                for (int16_t v : ft8_pcm) ft8_audio.push_back(v / 32768.0f);
        }
        if (recorder.active() && !recorder.write(block.data(), kNfft)) {
            notify("recording stopped: storage full");
            std::lock_guard<std::mutex> lk(g.mu);
            g.want.record.clear();
            cur.record.clear();
        }
        if (ffts < kMaxFftsPerFrame) { spectrum.add(block.data()); ++ffts; }
        since_frame += kNfft;
        if (since_frame >= size_t(cur.rate / cur.fps)) {
            std::vector<float> frame = spectrum.take();
            since_frame = 0;
            ffts = 0;
            // Signal strength at the tuned frequency, against the noise floor
            // (the median of the whole view). Drives the squelch.
            double half_width = cur.mode == Mode::Wfm ? 100e3 : cur.mode == Mode::Nfm ? 8e3 : cur.mode == Mode::Am ? 5e3 : cur.mode == Mode::Cw ? 500 : 3e3;
            long w = std::max(1L, std::lround(half_width / (cur.rate / kNfft)));
            long lo = std::max(0L, long(kNfft / 2) - w), hi = std::min(long(kNfft) - 1, long(kNfft / 2) + w);
            float peak = *std::max_element(frame.begin() + lo, frame.begin() + hi + 1);
            sorted = frame;
            std::nth_element(sorted.begin(), sorted.begin() + kNfft / 2, sorted.end());
            double signal = peak - sorted[kNfft / 2];
            if (cur.squelch && signal >= *cur.squelch) frames_below = 0;
            else ++frames_below;
            // Stay open through short dips (about a third of a second).
            squelch_open = !cur.squelch || frames_below < 5;
            std::lock_guard<std::mutex> lk(g.mu);
            g.signal_db = signal;
            g.recorded_bytes = recorder.active() ? recorder.bytes() : 0;
            g.squelch_open = squelch_open;
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
    bool decoding = false;    // asked for decoder output; same rule
    bool recording = false;   // asked for an IQ recording; same rule
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
        if (!parse_mode(arg, m)) return "err mode needs off, wfm, nfm, am, usb, lsb or cw";
        if ((m != Mode::Off) != c.listening) change_subscribers(c.listening ? -1 : +1);
        c.listening = m != Mode::Off;
        update([&](Settings& s) { s.mode = m; });
        return "ok";
    }
    if (cmd == "record") {
        // The rest of the line is the path, which may contain spaces.
        std::string path = line.size() > 7 ? line.substr(7) : "";
        if (path.empty()) return "err record needs a file path or off";
        bool on = path != "off";
        if (on && path[0] != '/') return "err record needs an absolute path";
        {
            // A recording belongs to the client that started it; nobody else
            // may stop or replace it.
            std::lock_guard<std::mutex> lk(g.mu);
            if (!g.want.record.empty() && !c.recording) return "err another program is recording";
        }
        if (on != c.recording) change_subscribers(c.recording ? -1 : +1);
        c.recording = on;
        update([&](Settings& s) { s.record = on ? path : ""; });
        return "ok";
    }
    if (cmd == "ppm") {
        if (!is_num || std::fabs(num) > 500 || num != std::floor(num)) return "err ppm needs a whole number, -500..500";
        update([&](Settings& s) { s.ppm = int(num); });
        return "ok";
    }
    if (cmd == "biastee") {
        if (arg != "on" && arg != "off") return "err biastee needs on or off";
        update([&](Settings& s) { s.bias_tee = arg == "on"; });
        return "ok";
    }
    if (cmd == "squelch") {
        if (arg == "off") { update([](Settings& s) { s.squelch.reset(); }); return "ok"; }
        if (!is_num || num < 0 || num > 100) return "err squelch needs dB above the noise (0..100) or off";
        update([&](Settings& s) { s.squelch = num; });
        return "ok";
    }
    if (cmd == "decode") {
        if (arg != "sensors" && arg != "aircraft" && arg != "aprs" && arg != "ft8" && arg != "server" && arg != "off")
            return "err decode needs sensors, aircraft, aprs, ft8, server or off";
        bool on = arg != "off";
        if (on != c.decoding) change_subscribers(c.decoding ? -1 : +1);
        c.decoding = on;
        // FT8 is decoded here, alongside the spectrum; the rest hand the
        // receiver to another program.
        update([&](Settings& s) {
            s.ft8 = arg == "ft8";
            s.tool = on && arg != "ft8" ? arg : "";
        });
        return "ok";
    }
    if (cmd == "status") {
        std::lock_guard<std::mutex> lk(g.mu);
        char buf[480];
        std::snprintf(buf, sizeof buf, "ok freq=%.0f offset=%.0f rate=%.0f gain=%s ppm=%d biastee=%s fps=%d mode=%s audio_rate=%.0f squelch=%s signal=%.1f open=%d decode=%s recorded=%.0f running=%d source=%s",
                      g.want.freq, g.want.offset, g.want.rate,
                      g.want.gain ? std::to_string(*g.want.gain).c_str() : "auto", g.want.ppm,
                      g.want.bias_tee ? "on" : "off", g.want.fps,
                      mode_name(g.want.mode), g.audio_rate,
                      g.want.squelch ? std::to_string(int(*g.want.squelch)).c_str() : "off", g.signal_db,
                      g.squelch_open ? 1 : 0, g.want.ft8 ? "ft8" : g.want.tool.empty() ? "off" : g.want.tool.c_str(), g.recorded_bytes, g.running ? 1 : 0, spec.c_str());
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
            std::vector<std::string> decoded;
            {
                std::lock_guard<std::mutex> lk(g.mu);
                decoded.swap(g.decoded);
                if (g.seq != sent_seq) { frame = g.frame; freq = g.frame_freq; rate = g.frame_rate; sent_seq = g.seq; }
                if (g.error_seq != sent_error_seq) { error = g.error; sent_error_seq = g.error_seq; }
            }
            for (auto& [fd, c] : clients) {
                if ((!c.cols && !c.listening && !c.decoding && !c.recording) || c.out.size() > kMaxQueued) continue;
                if (!error.empty()) c.out += "E " + error + "\n";
                if (c.decoding)
                    for (auto& l : decoded) c.out += "D " + l + "\n";
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
            bool was_listening = clients[fd].listening, was_decoding = clients[fd].decoding;
            if (clients[fd].recording) {
                change_subscribers(-1);
                update([](Settings& s) { s.record.clear(); });
            }
            if (was_listening) change_subscribers(-1);
            if (was_decoding) change_subscribers(-1);
            close(fd);
            clients.erase(fd);
            if (was_listening) {
                // Audio belongs to the client that asked for it.
                bool others = false;
                for (auto& [other_fd, other] : clients) others = others || other.listening;
                if (!others) update([](Settings& s) { s.mode = Mode::Off; });
            }
            if (was_decoding) {
                bool others = false;
                for (auto& [other_fd, other] : clients) others = others || other.decoding;
                if (!others) update([](Settings& s) { s.tool.clear(); s.ft8 = false; });
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
