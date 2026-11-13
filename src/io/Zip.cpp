#include "Zip.h"

#include <zip.h>

#include <fstream>
#include <memory>
#include <system_error>

namespace sb::io::zip {

namespace {

// Every entry is stamped 1980-01-01 00:00, the earliest zip date, written directly in
// zip's DOS format. (zip_file_set_mtime would convert through the local time zone,
// so the same design saved in two time zones would give different bytes.)
constexpr zip_uint16_t kDosTime = 0;                        // 00:00:00
constexpr zip_uint16_t kDosDate = (0 << 9) | (1 << 5) | 1;  // years since 1980, month, day

struct ZipCloser {
    void operator()(zip_t* z) const { zip_discard(z); }
};
struct SourceFreer {
    void operator()(zip_source_t* s) const { zip_source_free(s); }
};
using ZipPtr = std::unique_ptr<zip_t, ZipCloser>;
using SourcePtr = std::unique_ptr<zip_source_t, SourceFreer>;

std::string errorText(zip_error_t* err) {
    std::string text = zip_error_strerror(err);
    zip_error_fini(err);
    return text;
}

}  // namespace

Diagnostics pack(const Entries& entries, Bytes& out, const Diagnostic& failure, bool compress) {
    auto failed = [&](std::string detail) {
        Diagnostic d = failure;
        d.detail = std::move(detail);
        return Diagnostics{std::move(d)};
    };

    zip_error_t err;
    zip_error_init(&err);
    SourcePtr src(zip_source_buffer_create(nullptr, 0, 0, &err));
    if (!src) return failed(errorText(&err));
    ZipPtr za(zip_open_from_source(src.get(), ZIP_TRUNCATE, &err));
    if (!za) return failed(errorText(&err));
    zip_source_keep(src.get());  // za now shares src; keep it alive after closing za

    for (const auto& [name, text] : entries) {
        zip_source_t* s = zip_source_buffer(za.get(), text.data(), text.size(), 0);
        if (!s) return failed(zip_strerror(za.get()));
        const zip_int64_t index =
            zip_file_add(za.get(), name.c_str(), s, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE);
        if (index < 0) {
            zip_source_free(s);
            return failed(zip_strerror(za.get()));
        }
        const auto i = static_cast<zip_uint64_t>(index);
        if (zip_file_set_dostime(za.get(), i, kDosTime, kDosDate, 0) != 0) return failed(zip_strerror(za.get()));
        if (!compress && zip_set_file_compression(za.get(), i, ZIP_CM_STORE, 0) != 0)
            return failed(zip_strerror(za.get()));
    }
    if (zip_close(za.get()) != 0) return failed(zip_strerror(za.get()));
    (void)za.release();  // zip_close freed it

    // Copy the finished archive out of the memory source.
    if (zip_source_open(src.get()) != 0) return failed("cannot reopen archive buffer");
    zip_source_seek(src.get(), 0, SEEK_END);
    const zip_int64_t size = zip_source_tell(src.get());
    zip_source_seek(src.get(), 0, SEEK_SET);
    out.assign(static_cast<std::size_t>(size < 0 ? 0 : size), 0);
    const zip_int64_t got = zip_source_read(src.get(), out.data(), out.size());
    zip_source_close(src.get());
    if (size < 0 || got != size) return failed("cannot read archive buffer");
    return {};
}

Diagnostics writeAtomically(const Bytes& bytes, const std::filesystem::path& path, const Diagnostic& failure) {
    auto cantWrite = [&](std::string detail) {
        Diagnostic d = failure;
        d.detail = std::move(detail);
        return Diagnostics{std::move(d)};
    };
    auto tmp = path;
    tmp += ".saving";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return cantWrite("cannot create " + tmp.string());
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        f.close();
        if (!f) {
            std::error_code ignore;
            std::filesystem::remove(tmp, ignore);
            return cantWrite("cannot write " + tmp.string());
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::error_code ignore;
        std::filesystem::remove(tmp, ignore);
        return cantWrite("cannot replace " + path.string() + ": " + ec.message());
    }
    return {};
}

}  // namespace sb::io::zip
