// RDS: the data broadcast FM stations send alongside the music (station
// name, a line of text). This is the part that turns a stream of bits into
// those, and its mirror for making test signals.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace zrf {

class RdsDecoder {
public:
    // Feed bits in the order received (after differential decoding).
    void push_bit(int bit);

    bool synced() const { return synced_; }
    uint16_t pi() const { return pi_; }                 // station identity code
    const std::string& name() const { return name_; }   // 8 characters, once all were received
    const std::string& text() const { return text_; }   // radio text, once a whole message was received
    int groups() const { return groups_; }

private:
    void group(const uint16_t b[4], const bool ok[4]);

    uint32_t shift_ = 0;
    int bit_count_ = 0;      // bits since the last block boundary, when synced
    int block_ = 0;          // which block (0..3) the next 26 bits are
    bool synced_ = false;
    int bad_blocks_ = 0;
    uint16_t blocks_[4] = {};
    bool ok_[4] = {};

    uint16_t pi_ = 0;
    int groups_ = 0;
    char name_buf_[8] = {};
    uint8_t name_seen_ = 0;
    std::string name_;
    char text_buf_[64] = {};
    uint32_t text_seen_lo_ = 0, text_seen_hi_ = 0;
    int text_flag_ = -1;
    std::string text_;
};

// The 10 check bits for 16 data bits (before the block's offset word is added).
uint16_t rds_crc(uint16_t data);

// A repeating bit sequence carrying a station name (up to 8 characters) and a
// line of radio text (up to 64), for generating test signals.
std::vector<uint8_t> rds_encode(uint16_t pi, const std::string& name, const std::string& text);

}  // namespace zrf
