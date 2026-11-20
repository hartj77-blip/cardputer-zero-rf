#include "rds.h"

#include <cstring>

namespace zrf {
namespace {

// Offset words that mark which of a group's four blocks this is. The third
// block uses C in "A" groups and C' in "B" groups.
const uint16_t kOffsetA = 0x0FC, kOffsetB = 0x198, kOffsetC = 0x168, kOffsetC2 = 0x350, kOffsetD = 0x1B4;

bool block_ok(uint32_t word26, uint16_t offset) {
    uint16_t data = uint16_t(word26 >> 10), check = uint16_t(word26 & 0x3FF);
    return (rds_crc(data) ^ offset) == check;
}

char printable(uint8_t c) { return c >= 32 && c < 127 ? char(c) : ' '; }

void append_block(std::vector<uint8_t>& bits, uint16_t data, uint16_t offset) {
    uint32_t word = uint32_t(data) << 10 | uint32_t(rds_crc(data) ^ offset);
    for (int i = 25; i >= 0; --i) bits.push_back(uint8_t(word >> i & 1));
}

}  // namespace

uint16_t rds_crc(uint16_t data) {
    // Remainder of data * x^10 divided by x^10 + x^8 + x^7 + x^5 + x^4 + x^3 + 1.
    uint32_t reg = uint32_t(data) << 10;
    for (int i = 25; i >= 10; --i)
        if (reg & (1u << i)) reg ^= 0x5B9u << (i - 10);
    return uint16_t(reg & 0x3FF);
}

void RdsDecoder::push_bit(int bit) {
    shift_ = (shift_ << 1 | uint32_t(bit & 1)) & 0x3FFFFFF;
    if (!synced_) {
        // Look for the first block of a group at every bit position.
        if (block_ok(shift_, kOffsetA)) {
            synced_ = true;
            blocks_[0] = uint16_t(shift_ >> 10);
            ok_[0] = true;
            block_ = 1;
            bit_count_ = 0;
            bad_blocks_ = 0;
        }
        return;
    }
    if (++bit_count_ < 26) return;
    bit_count_ = 0;
    bool ok = block_ == 0 ? block_ok(shift_, kOffsetA)
              : block_ == 1 ? block_ok(shift_, kOffsetB)
              : block_ == 2 ? block_ok(shift_, kOffsetC) || block_ok(shift_, kOffsetC2)
              : block_ok(shift_, kOffsetD);
    blocks_[block_] = uint16_t(shift_ >> 10);
    ok_[block_] = ok;
    bad_blocks_ = ok ? 0 : bad_blocks_ + 1;
    // A run of bad blocks means we have slipped: start looking again.
    if (bad_blocks_ >= 8) {
        synced_ = false;
        return;
    }
    if (++block_ == 4) {
        block_ = 0;
        group(blocks_, ok_);
    }
}

void RdsDecoder::group(const uint16_t b[4], const bool ok[4]) {
    if (!ok[0] || !ok[1]) return;
    pi_ = b[0];
    ++groups_;
    int type = b[1] >> 12, version_b = b[1] >> 11 & 1;
    if (type == 0 && ok[3]) {
        // Station name, two characters at a time.
        int at = (b[1] & 3) * 2;
        name_buf_[at] = printable(uint8_t(b[3] >> 8));
        name_buf_[at + 1] = printable(uint8_t(b[3] & 0xFF));
        name_seen_ |= uint8_t(1 << (b[1] & 3));
        if (name_seen_ == 0x0F) name_.assign(name_buf_, 8);
    } else if (type == 2) {
        // Radio text. A change of the A/B flag means a new message.
        int flag = b[1] >> 4 & 1, addr = b[1] & 0xF;
        if (flag != text_flag_) {
            text_flag_ = flag;
            std::memset(text_buf_, ' ', sizeof text_buf_);
            text_seen_lo_ = text_seen_hi_ = 0;
        }
        int per = version_b ? 2 : 4, at = addr * per;
        uint8_t chars[4];
        int n = 0;
        if (!version_b && ok[2]) { chars[n++] = uint8_t(b[2] >> 8); chars[n++] = uint8_t(b[2] & 0xFF); }
        else if (!version_b) return;
        if (!ok[3]) return;
        chars[n++] = uint8_t(b[3] >> 8);
        chars[n++] = uint8_t(b[3] & 0xFF);
        bool ended = false;
        for (int i = 0; i < n; ++i) {
            if (chars[i] == 0x0D) { ended = true; text_buf_[at + i] = 0; break; }
            text_buf_[at + i] = printable(chars[i]);
        }
        (addr < 8 ? text_seen_lo_ : text_seen_hi_) |= 1u << addr;
        // Complete when the end mark arrived and every segment before it has.
        uint32_t need = (1u << (addr + 1)) - 1, seen = text_seen_lo_ | text_seen_hi_;
        if ((ended || addr == 15) && (seen & need) == need) {
            std::string t(text_buf_, size_t(at + n));
            t = t.c_str();   // stop at the end mark
            while (!t.empty() && t.back() == ' ') t.pop_back();
            text_ = t;
        }
    }
}

std::vector<uint8_t> rds_encode(uint16_t pi, const std::string& name_in, const std::string& text_in) {
    std::string name = (name_in + "        ").substr(0, 8);
    std::string text = text_in.substr(0, 64);
    if (text.size() < 64) text += '\r';
    while (text.size() % 4) text += ' ';
    std::vector<uint8_t> bits;
    size_t text_groups = text.size() / 4;
    // Name groups interleaved with text groups, as stations send them.
    for (size_t i = 0; i < std::max<size_t>(4, text_groups); ++i) {
        uint16_t seg = uint16_t(i % 4);
        append_block(bits, pi, kOffsetA);
        append_block(bits, uint16_t(0x0 << 12 | 0 << 11 | seg), kOffsetB);          // group 0A
        append_block(bits, 0xE0CD, kOffsetC);                                        // no alternative frequencies
        append_block(bits, uint16_t(uint8_t(name[seg * 2]) << 8 | uint8_t(name[seg * 2 + 1])), kOffsetD);
        if (i < text_groups) {
            append_block(bits, pi, kOffsetA);
            append_block(bits, uint16_t(0x2 << 12 | 0 << 11 | i), kOffsetB);         // group 2A
            append_block(bits, uint16_t(uint8_t(text[i * 4]) << 8 | uint8_t(text[i * 4 + 1])), kOffsetC);
            append_block(bits, uint16_t(uint8_t(text[i * 4 + 2]) << 8 | uint8_t(text[i * 4 + 3])), kOffsetD);
        }
    }
    return bits;
}

}  // namespace zrf
