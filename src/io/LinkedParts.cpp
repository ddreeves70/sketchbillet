#include "sb/io/LinkedParts.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <system_error>

#include "sb/io/SketchBilletFile.h"
#include "sb/model/AssemblyCommands.h"

namespace sb::io {

namespace {

namespace fs = std::filesystem;

// --- SHA-256 (FIPS 180-4) ----------------------------------------------------

constexpr std::array<std::uint32_t, 64> kRounds{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::uint32_t rotr(std::uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

void compress(std::array<std::uint32_t, 8>& h, const std::uint8_t* block) {
    std::array<std::uint32_t, 64> w{};
    for (int i = 0; i < 16; ++i) {
        w[i] = (std::uint32_t{block[i * 4]} << 24) | (std::uint32_t{block[i * 4 + 1]} << 16) |
               (std::uint32_t{block[i * 4 + 2]} << 8) | std::uint32_t{block[i * 4 + 3]};
    }
    for (int i = 16; i < 64; ++i) {
        const auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto [a, b, c, d, e, f, g, k] = h;
    for (int i = 0; i < 64; ++i) {
        const auto t1 =
            k + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kRounds[i] + w[i];
        const auto t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += k;
}

// --- Finding files -----------------------------------------------------------

fs::path fromUtf8(const std::string& text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

std::string toUtf8(const fs::path& p) {
    const auto text = p.generic_u8string();
    return std::string(text.begin(), text.end());
}

std::string fileName(const std::string& path) {
    return toUtf8(fromUtf8(path).filename());
}

// The file holds the part's design (its manifest says so).
bool holds(const fs::path& file, const DocId& part) {
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) return false;
    Bytes data;
    if (!readFileBytes(file, data)) return false;
    const auto id = readDesignId(data);
    return id && *id == part;
}

bool sameFile(const std::string& a, const fs::path& b) {
    return fromUtf8(a).lexically_normal() == b.lexically_normal();
}

// Where the file is now, relative to the assembly's folder, in words: "the
// parts folder", or "the same folder as Car".
std::string placeWords(const fs::path& file, const fs::path& folder, const std::string& assembly) {
    const auto where = folder.empty() ? fs::path() : file.parent_path().lexically_relative(folder);
    if (where.empty() || where == ".") return "the same folder as " + assembly;
    return "the " + toUtf8(where) + " folder";
}

}  // namespace

std::string sha256Hex(const std::vector<std::uint8_t>& data) {
    std::array<std::uint32_t, 8> h{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const std::size_t whole = data.size() / 64 * 64;
    for (std::size_t i = 0; i < whole; i += 64) compress(h, data.data() + i);
    // The last bytes, a 1 bit, zeros, and the length in bits.
    std::array<std::uint8_t, 128> tail{};
    const std::size_t rest = data.size() - whole;
    std::copy(data.begin() + static_cast<std::ptrdiff_t>(whole), data.end(), tail.begin());
    tail[rest] = 0x80;
    const std::size_t blocks = rest + 9 <= 64 ? 1 : 2;
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    for (int i = 0; i < 8; ++i) tail[blocks * 64 - 1 - i] = static_cast<std::uint8_t>(bits >> (8 * i));
    for (std::size_t b = 0; b < blocks; ++b) compress(h, tail.data() + b * 64);

    std::string hex;
    hex.reserve(64);
    constexpr char kDigits[] = "0123456789abcdef";
    for (const auto word : h) {
        for (int shift = 28; shift >= 0; shift -= 4) hex += kDigits[(word >> shift) & 0xf];
    }
    return hex;
}

std::string utcNow() {
    using namespace std::chrono;
    const auto now = floor<seconds>(system_clock::now());
    const auto day = floor<days>(now);
    const year_month_day date{day};
    const hh_mm_ss time{now - day};
    char text[32];
    std::snprintf(text, sizeof text, "%04d-%02u-%02uT%02d:%02d:%02dZ", static_cast<int>(date.year()),
                  static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
                  static_cast<int>(time.hours().count()), static_cast<int>(time.minutes().count()),
                  static_cast<int>(time.seconds().count()));
    return text;
}

std::string linkDate(const std::string& updated) {
    static constexpr std::array<const char*, 12> kMonths{"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    if (std::sscanf(updated.c_str(), "%4d-%2u-%2u", &year, &month, &day) != 3 || month < 1 || month > 12 ||
        day < 1 || day > 31)
        return updated;
    return std::string(kMonths[month - 1]) + " " + std::to_string(day) + ", " + std::to_string(year);
}

std::optional<fs::path> findLinkedFile(const model::PartLink& link, const DocId& part,
                                       const fs::path& folder) {
    const fs::path path = fromUtf8(link.path);
    if (holds(path, part)) return path.lexically_normal();
    const fs::path look = folder.empty() ? path.parent_path() : folder;
    if (look.empty()) return std::nullopt;
    if (const auto byName = look / path.filename(); holds(byName, part)) return byName.lexically_normal();

    // By its design id, in the folder and the folders in it (renamed or moved),
    // in name order so the same folder always gives the same answer.
    std::vector<fs::path> files;
    std::error_code ec;
    fs::recursive_directory_iterator it(look, fs::directory_options::skip_permission_denied, ec);
    constexpr int kDeepest = 3;
    constexpr std::size_t kMostFiles = 500;
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const auto& entry = *it;
        const auto name = entry.path().filename().string();
        if (entry.is_directory(ec)) {
            if (it.depth() >= kDeepest - 1 || (!name.empty() && name[0] == '.'))
                it.disable_recursion_pending();
            continue;
        }
        if (entry.path().extension() == ".sb") files.push_back(entry.path());
        if (files.size() >= kMostFiles) break;
    }
    std::sort(files.begin(), files.end());
    for (const auto& file : files) {
        if (holds(file, part)) return file.lexically_normal();
    }
    return std::nullopt;
}

LinkUpdates updateLinkedParts(const model::Document& assembly, const fs::path& folder,
                              const std::optional<DocId>& only) {
    LinkUpdates out;
    std::vector<model::PartUpdate> changes;
    std::string label;
    for (const auto* part : assembly.parts()) {
        const auto* link = assembly.partLink(part->id());
        if (!link || (only && *only != part->id())) continue;
        const std::string file = fileName(link->path);
        const std::string copy = "the copy of " + part->name() + " saved on " + linkDate(link->updated);
        const auto found = findLinkedFile(*link, part->id(), folder);
        if (!found) {
            out.notes.push_back({"part.not_found", Severity::Warning,
                                 file + " can't be found, so " + assembly.name() + " uses " + copy +
                                     ". Put " + file + " back, or link " + part->name() +
                                     " to it where it is now with part.relink part=\"" + part->name() +
                                     "\" file=<where it is>.",
                                 {}});
            if (link->found) {
                // Found before: mark it, so it shows.
                auto marked = *link;
                marked.found = false;
                changes.push_back({part->id(), nullptr, std::move(marked)});
                label = "Update " + part->name();
            }
            continue;
        }
        Bytes data;
        if (!readFileBytes(*found, data)) {
            out.notes.push_back({"part.cant_read", Severity::Warning,
                                 file + " couldn't be opened, so " + assembly.name() + " uses " + copy +
                                     ". Check that you're allowed to open it.",
                                 {}});
            continue;
        }
        const auto sha = sha256Hex(data);
        const bool moved = !sameFile(link->path, *found);
        model::PartLink now{toUtf8(*found), sha, link->updated, true};
        std::unique_ptr<model::Document> design;
        if (sha != link->sha256) {
            auto opened = readBytes(data, toUtf8(found->filename()), found->parent_path());
            if (!opened.ok() || opened.document->kind() != model::DocKind::Part) {
                out.notes.push_back({"part.cant_read", Severity::Warning,
                                     file + " couldn't be opened, so " + assembly.name() + " uses " + copy +
                                         ". " +
                                         (opened.ok()             ? file + " is an assembly now."
                                          : opened.issues.empty() ? std::string()
                                                                  : opened.issues.front().message),
                                     {}});
                continue;
            }
            design = std::move(opened.document);
            now.updated = utcNow();
        } else if (!moved && link->found) {
            continue;  // up to date
        }
        if (moved) {
            out.notes.push_back({"part.moved", Severity::Info,
                                 assembly.name() + " found " + toUtf8(found->filename()) + " in " +
                                     placeWords(*found, folder, assembly.name()) +
                                     ", and links to it there now.",
                                 {}});
        }
        if (design) {
            out.notes.push_back({"part.updated", Severity::Info,
                                 part->name() + " changed in " + toUtf8(found->filename()) + ", so " +
                                     assembly.name() + " uses the new " + part->name() + ".",
                                 {}});
        }
        changes.push_back({part->id(), std::move(design), std::move(now)});
        label = "Update " + part->name();
    }
    if (changes.size() > 1) label = "Update linked parts";
    if (!changes.empty())
        out.command = std::make_unique<model::UpdatePartsCommand>(std::move(changes), label);
    return out;
}

}  // namespace sb::io
