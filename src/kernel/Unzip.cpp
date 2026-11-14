#include "Unzip.h"

#include <array>
#include <cstdint>
#include <stdexcept>

namespace sb::kernel::unzip {

namespace {

// ---------------------------------------------------------------------------
// Inflate (RFC 1951), in the manner of zlib's puff.c: canonical Huffman codes
// decoded a bit at a time. Not the fastest, but short and easy to check.
// ---------------------------------------------------------------------------

constexpr int kMaxBits = 15;       // longest code
constexpr int kMaxLitLen = 286;    // literal/length codes
constexpr int kMaxDist = 30;       // distance codes
constexpr int kFixedLitLen = 288;  // codes in the fixed literal/length table

struct Damaged {};  // thrown inside this file only, caught by inflate()

class Bits {
public:
    explicit Bits(std::string_view data) : data_(data) {}

    // The next `n` bits, first bit lowest.
    unsigned take(int n) {
        while (count_ < n) {
            if (pos_ >= data_.size()) throw Damaged{};
            buffer_ |= static_cast<std::uint32_t>(static_cast<unsigned char>(data_[pos_++])) << count_;
            count_ += 8;
        }
        const unsigned out = buffer_ & ((1u << n) - 1);
        buffer_ >>= n;
        count_ -= n;
        return out;
    }
    // Drops what's left of the current byte.
    void align() {
        buffer_ = 0;
        count_ = 0;
    }
    std::size_t pos() const { return pos_; }
    void skip(std::size_t n) {
        if (pos_ + n > data_.size()) throw Damaged{};
        pos_ += n;
    }
    std::string_view data() const { return data_; }

private:
    std::string_view data_;
    std::size_t pos_ = 0;
    std::uint32_t buffer_ = 0;
    int count_ = 0;
};

// A canonical Huffman code: how many codes of each length, and the symbols in code order.
struct Huffman {
    std::array<std::uint16_t, kMaxBits + 1> count{};
    std::vector<std::uint16_t> symbol;
};

// Makes a code from each symbol's code length (0: not used). Returns false if
// the lengths ask for more codes than there are; a code that's incomplete is
// allowed only when it has a single code (as RFC 1951 allows for distances).
bool build(Huffman& h, const std::uint16_t* lengths, int n) {
    h.count.fill(0);
    for (int i = 0; i < n; ++i) ++h.count[lengths[i]];
    if (h.count[0] == n) {
        h.symbol.clear();
        return true;  // no codes: fine until one is used
    }
    int left = 1;
    for (int len = 1; len <= kMaxBits; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return false;  // too many codes
    }
    std::array<std::uint16_t, kMaxBits + 1> offs{};
    for (int len = 1; len < kMaxBits; ++len)
        offs[len + 1] = static_cast<std::uint16_t>(offs[len] + h.count[len]);
    h.symbol.assign(static_cast<std::size_t>(n), 0);
    for (int i = 0; i < n; ++i)
        if (lengths[i] != 0) h.symbol[offs[lengths[i]]++] = static_cast<std::uint16_t>(i);
    // Incomplete codes are only allowed for a single code of length 1.
    return left == 0 || (n - h.count[0] == 1);
}

int decode(Bits& in, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= kMaxBits; ++len) {
        code |= static_cast<int>(in.take(1));
        const int count = h.count[len];
        if (code - count < first) return h.symbol.at(static_cast<std::size_t>(index + (code - first)));
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    throw Damaged{};  // ran out of codes
}

constexpr std::array<std::uint16_t, 29> kLengthBase{3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                                    15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                                    67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<std::uint16_t, 29> kLengthExtra{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                     2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<std::uint16_t, 30> kDistBase{
    1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<std::uint16_t, 30> kDistExtra{0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                   6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// Decodes one block's literals and copies until its end code.
void codes(Bits& in, std::string& out, const Huffman& lit, const Huffman& dist, std::size_t limit) {
    for (;;) {
        const int sym = decode(in, lit);
        if (sym < 256) {
            if (out.size() >= limit) throw Damaged{};
            out.push_back(static_cast<char>(sym));
        } else if (sym == 256) {
            return;
        } else {
            const int s = sym - 257;
            if (s >= 29) throw Damaged{};
            const std::size_t len =
                kLengthBase[static_cast<std::size_t>(s)] + in.take(kLengthExtra[static_cast<std::size_t>(s)]);
            const int d = decode(in, dist);
            if (d >= 30) throw Damaged{};
            const std::size_t back =
                kDistBase[static_cast<std::size_t>(d)] + in.take(kDistExtra[static_cast<std::size_t>(d)]);
            if (back > out.size() || out.size() + len > limit) throw Damaged{};
            const std::size_t from = out.size() - back;
            for (std::size_t i = 0; i < len; ++i) out.push_back(out[from + i]);  // may overlap: byte by byte
        }
    }
}

void stored(Bits& in, std::string& out, std::size_t limit) {
    in.align();
    const auto data = in.data();
    const std::size_t at = in.pos();
    if (at + 4 > data.size()) throw Damaged{};
    const unsigned len =
        static_cast<unsigned char>(data[at]) | (static_cast<unsigned char>(data[at + 1]) << 8);
    const unsigned nlen =
        static_cast<unsigned char>(data[at + 2]) | (static_cast<unsigned char>(data[at + 3]) << 8);
    if ((len ^ 0xffffu) != nlen) throw Damaged{};
    in.skip(4);
    if (in.pos() + len > data.size() || out.size() + len > limit) throw Damaged{};
    out.append(data.substr(in.pos(), len));
    in.skip(len);
}

void fixed(Bits& in, std::string& out, std::size_t limit) {
    static const auto tables = [] {
        std::pair<Huffman, Huffman> t;
        std::array<std::uint16_t, kFixedLitLen> lengths{};
        int s = 0;
        for (; s < 144; ++s) lengths[static_cast<std::size_t>(s)] = 8;
        for (; s < 256; ++s) lengths[static_cast<std::size_t>(s)] = 9;
        for (; s < 280; ++s) lengths[static_cast<std::size_t>(s)] = 7;
        for (; s < kFixedLitLen; ++s) lengths[static_cast<std::size_t>(s)] = 8;
        build(t.first, lengths.data(), kFixedLitLen);
        std::array<std::uint16_t, kMaxDist> dist{};
        dist.fill(5);
        build(t.second, dist.data(), kMaxDist);
        return t;
    }();
    codes(in, out, tables.first, tables.second, limit);
}

void dynamic(Bits& in, std::string& out, std::size_t limit) {
    const int nlen = static_cast<int>(in.take(5)) + 257;
    const int ndist = static_cast<int>(in.take(5)) + 1;
    const int ncode = static_cast<int>(in.take(4)) + 4;
    if (nlen > kMaxLitLen || ndist > kMaxDist) throw Damaged{};
    static constexpr std::array<int, 19> kOrder{16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                                11, 4,  12, 3, 13, 2, 14, 1, 15};
    std::array<std::uint16_t, kMaxLitLen + kMaxDist> lengths{};
    for (int i = 0; i < ncode; ++i)
        lengths[static_cast<std::size_t>(kOrder[static_cast<std::size_t>(i)])] =
            static_cast<std::uint16_t>(in.take(3));
    Huffman lencode;
    if (!build(lencode, lengths.data(), 19) || lencode.symbol.empty()) throw Damaged{};
    for (int i = 0; i < nlen + ndist;) {
        int sym = decode(in, lencode);
        if (sym < 16) {
            lengths[static_cast<std::size_t>(i++)] = static_cast<std::uint16_t>(sym);
            continue;
        }
        std::uint16_t len = 0;
        int repeat = 0;
        if (sym == 16) {
            if (i == 0) throw Damaged{};
            len = lengths[static_cast<std::size_t>(i - 1)];
            repeat = 3 + static_cast<int>(in.take(2));
        } else if (sym == 17) {
            repeat = 3 + static_cast<int>(in.take(3));
        } else {
            repeat = 11 + static_cast<int>(in.take(7));
        }
        if (i + repeat > nlen + ndist) throw Damaged{};
        while (repeat--) lengths[static_cast<std::size_t>(i++)] = len;
    }
    if (lengths[256] == 0) throw Damaged{};  // no end-of-block code
    Huffman lit, dist;
    if (!build(lit, lengths.data(), nlen)) throw Damaged{};
    if (!build(dist, lengths.data() + nlen, ndist)) throw Damaged{};
    codes(in, out, lit, dist, limit);
}

std::uint32_t u16(std::string_view d, std::size_t at) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(d[at])) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(d[at + 1])) << 8;
}
std::uint32_t u32(std::string_view d, std::size_t at) {
    return u16(d, at) | u16(d, at + 2) << 16;
}

}  // namespace

std::optional<std::string> inflate(std::string_view data, std::size_t limit, std::string& why) {
    std::string out;
    try {
        Bits in(data);
        for (bool last = false; !last;) {
            last = in.take(1) == 1;
            switch (in.take(2)) {
                case 0:
                    stored(in, out, limit);
                    break;
                case 1:
                    fixed(in, out, limit);
                    break;
                case 2:
                    dynamic(in, out, limit);
                    break;
                default:
                    throw Damaged{};
            }
        }
    } catch (const Damaged&) {
        why = out.size() >= limit ? "it unpacks to too much" : "its packed data is damaged";
        return std::nullopt;
    } catch (const std::out_of_range&) {
        why = "its packed data is damaged";
        return std::nullopt;
    }
    return out;
}

std::vector<Entry> list(std::string_view zip, std::string& why) {
    // The end of the central directory: the last 22+ bytes (more with a comment).
    std::size_t end = std::string_view::npos;
    if (zip.size() >= 22) {
        const std::size_t lowest = zip.size() > 22 + 65535 ? zip.size() - 22 - 65535 : 0;
        for (std::size_t at = zip.size() - 22 + 1; at-- > lowest;) {
            if (u32(zip, at) == 0x06054b50) {
                end = at;
                break;
            }
        }
    }
    if (end == std::string_view::npos) {
        why = "it isn't a zip archive";
        return {};
    }
    const std::size_t count = u16(zip, end + 10);
    std::size_t at = u32(zip, end + 16);
    std::vector<Entry> out;
    for (std::size_t i = 0; i < count; ++i) {
        if (at + 46 > zip.size() || u32(zip, at) != 0x02014b50) {
            why = "its list of files is damaged";
            return {};
        }
        Entry e;
        e.method = static_cast<int>(u16(zip, at + 10));
        const std::uint32_t packed = u32(zip, at + 20), size = u32(zip, at + 24);
        const std::size_t nameLen = u16(zip, at + 28), extraLen = u16(zip, at + 30),
                          commentLen = u16(zip, at + 32);
        const std::uint32_t local = u32(zip, at + 42);
        if (packed == 0xffffffffu || size == 0xffffffffu || local == 0xffffffffu) {
            why = "it's a zip64 archive (over 4 GB), which SketchBillet can't read";
            return {};
        }
        if (at + 46 + nameLen > zip.size()) {
            why = "its list of files is damaged";
            return {};
        }
        e.name = std::string(zip.substr(at + 46, nameLen));
        e.packedSize = packed;
        e.size = size;
        // The packed bytes start after the file's own (local) header.
        if (static_cast<std::size_t>(local) + 30 > zip.size() || u32(zip, local) != 0x04034b50) {
            why = "the header of " + e.name + " is damaged";
            return {};
        }
        e.dataOffset = local + 30 + u16(zip, local + 26) + u16(zip, local + 28);
        if (e.dataOffset + e.packedSize > zip.size()) {
            why = e.name + " is cut short";
            return {};
        }
        out.push_back(std::move(e));
        at += 46 + nameLen + extraLen + commentLen;
    }
    return out;
}

std::optional<std::string> read(std::string_view zip, const Entry& entry, std::size_t limit,
                                std::string& why) {
    const auto packed = zip.substr(entry.dataOffset, entry.packedSize);
    if (entry.size > limit) {
        why = entry.name + " unpacks to too much";
        return std::nullopt;
    }
    if (entry.method == 0) return std::string(packed);
    if (entry.method != 8) {
        why = entry.name + " is packed in a way SketchBillet can't unpack (method " +
              std::to_string(entry.method) + ")";
        return std::nullopt;
    }
    std::string inner;
    auto out = inflate(packed, limit, inner);
    if (!out) {
        why = entry.name + ": " + inner;
        return std::nullopt;
    }
    if (out->size() != entry.size) {
        why = entry.name + " unpacks to a different size than its header says";
        return std::nullopt;
    }
    return out;
}

}  // namespace sb::kernel::unzip
