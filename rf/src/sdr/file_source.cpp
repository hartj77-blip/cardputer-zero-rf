// IQ recording playback, looped, at the sample rate.
// ".cf32" / ".cfile" = 32-bit float pairs; anything else = unsigned 8-bit
// pairs, the format rtl_sdr writes. Tuning has no effect on a recording.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "sdr/source.h"

namespace zrf {
namespace {

bool ends_with(const std::string& s, const char* suffix) {
    size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

class FileSource : public Source {
public:
    explicit FileSource(std::string path)
        : path_(std::move(path)), float_(ends_with(path_, ".cf32") || ends_with(path_, ".cfile")) {}
    ~FileSource() override { if (f_) std::fclose(f_); }

    bool open(std::string& err) override {
        if (f_) std::fclose(f_);
        f_ = std::fopen(path_.c_str(), "rb");
        if (!f_) { err = "can't open " + path_; return false; }
        pacer_.reset();
        return true;
    }
    bool set_freq(double) override { return true; }
    bool set_rate(double hz) override { if (hz <= 0) return false; rate_ = hz; return true; }
    bool set_gain(std::optional<double>) override { return true; }

    size_t read(cf32* out, size_t n) override {
        if (!f_) return 0;
        size_t done = 0;
        bool rewound = false;
        while (done < n) {
            size_t got = float_ ? read_float(out + done, n - done) : read_u8(out + done, n - done);
            done += got;
            if (done < n) {
                // End of file: loop. Two empty reads in a row means an empty file.
                if (got == 0 && rewound) return 0;
                std::rewind(f_);
                rewound = true;
            } else {
                rewound = false;
            }
        }
        pacer_.wait(n, rate_);
        return n;
    }

private:
    size_t read_float(cf32* out, size_t n) { return std::fread(out, sizeof(cf32), n, f_); }
    size_t read_u8(cf32* out, size_t n) {
        raw_.resize(n * 2);
        size_t got = std::fread(raw_.data(), 2, n, f_);
        for (size_t i = 0; i < got; ++i)
            out[i] = cf32((raw_[2 * i] - 127.5f) / 127.5f, (raw_[2 * i + 1] - 127.5f) / 127.5f);
        return got;
    }

    std::string path_;
    bool float_;
    FILE* f_ = nullptr;
    double rate_ = 1.024e6;
    std::vector<uint8_t> raw_;
    Pacer pacer_;
};

}  // namespace

std::unique_ptr<Source> make_file_source(const std::string& path) {
    return std::make_unique<FileSource>(path);
}

}  // namespace zrf
