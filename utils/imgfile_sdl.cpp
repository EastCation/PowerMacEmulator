/*
DingusPPC - The Experimental PowerPC Macintosh emulator
Copyright (C) 2018-26 The DingusPPC Development Team
          (See CREDITS.MD for more details)

(You may also contact divingkxt or powermax2286 on Discord)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include <utils/imgfile.h>
#include <loguru.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <vector>

namespace {
inline std::filesystem::path u8_path(const std::string& s)
{
#if defined(__cpp_lib_char8_t)
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(s.data()), s.size()));
#else
    return std::filesystem::u8path(s);
#endif
}
} // namespace
#if !defined(_WIN32) && \
    (defined(__APPLE__) || defined(__linux__) || defined(__unix__))
#define DPPC_HAS_PRIVATE_MMAP 1
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

extern bool is_deterministic;

class ImgFileBackend {
public:
    virtual ~ImgFileBackend() = default;

    virtual bool open(const std::string &img_path) = 0;
    virtual void close() = 0;
    virtual uint64_t size() const = 0;
    virtual uint64_t read(void *buf, uint64_t offset, uint64_t length) const = 0;
    virtual uint64_t write(const void *buf, uint64_t offset, uint64_t length) = 0;
};

static std::unique_ptr<ImgFileBackend> make_imgfile_backend(const std::string &img_path);

class ImgFile::Impl {
public:
    std::unique_ptr<ImgFileBackend> backend;
};

ImgFile::ImgFile(): impl(std::make_unique<Impl>())
{

}

ImgFile::~ImgFile() = default;

bool ImgFile::open(const std::string &img_path)
{
    auto backend = make_imgfile_backend(img_path);
    if (!backend)
        return false;

    impl->backend = std::move(backend);
    return true;
}

void ImgFile::close()
{
    if (!impl->backend) {
        LOG_F(WARNING, "ImgFile::close before disk was opened, ignoring.");
        return;
    }
    impl->backend->close();
    impl->backend.reset();
}

uint64_t ImgFile::size() const
{
    if (!impl->backend) {
        LOG_F(WARNING, "ImgFile::size before disk was opened, ignoring.");
        return 0;
    }
    return impl->backend->size();
}

uint64_t ImgFile::read(void* buf, uint64_t offset, uint64_t length) const
{
    if (!impl->backend) {
        LOG_F(WARNING, "ImgFile::read before disk was opened, ignoring.");
        return 0;
    }
    return impl->backend->read(buf, offset, length);
}

uint64_t ImgFile::write(const void* buf, uint64_t offset, uint64_t length)
{
    if (!impl->backend) {
        LOG_F(WARNING, "ImgFile::write before disk was opened, ignoring.");
        return 0;
    }
    return impl->backend->write(buf, offset, length);
}

class FileStreamBackend : public ImgFileBackend {
public:
    bool open(const std::string &img_path) override {
        stream = std::make_unique<std::fstream>(
            u8_path(img_path), std::ios::in | std::ios::out | std::ios::binary);
        if (!stream->is_open())
            return false;

        stream->seekg(0, std::ios::end);
        file_size = stream->tellg();
        stream->clear();
        return stream->good();
    }

    void close() override {
        stream.reset();
    }

    uint64_t size() const override {
        return file_size;
    }

    uint64_t read(void *buf, uint64_t offset, uint64_t length) const override {
        stream->clear();
        stream->seekg(offset, std::ios::beg);
        stream->read(static_cast<char *>(buf), length);
        return stream->gcount();
    }

    uint64_t write(const void *buf, uint64_t offset, uint64_t length) override {
        stream->clear();
        stream->seekp(offset, std::ios::beg);
        stream->write(static_cast<const char *>(buf), length);
#if defined(WIN32) || defined(__APPLE__) || defined(__linux)
        stream->flush();
#endif
        if (!stream->good())
            return 0;

        file_size = std::max(file_size, offset + length);
        return length;
    }

private:
    mutable std::unique_ptr<std::fstream> stream;
    uint64_t file_size = 0;
};

class MemoryStreamBackend : public ImgFileBackend {
public:
    bool open(const std::string &img_path) override {
        std::ifstream file(u8_path(img_path), std::ios::in | std::ios::binary);
        if (!file)
            return false;

        stream = std::make_unique<std::stringstream>();
        *stream << file.rdbuf();
        stream->seekg(0, std::ios::end);
        file_size = stream->tellg();
        stream->clear();
        return stream->good();
    }

    void close() override {
        stream.reset();
    }

    uint64_t size() const override {
        return file_size;
    }

    uint64_t read(void *buf, uint64_t offset, uint64_t length) const override {
        stream->clear();
        stream->seekg(offset, std::ios::beg);
        stream->read(static_cast<char *>(buf), length);
        return stream->gcount();
    }

    uint64_t write(const void *buf, uint64_t offset, uint64_t length) override {
        stream->clear();
        stream->seekp(offset, std::ios::beg);
        stream->write(static_cast<const char *>(buf), length);
        return stream->good() ? length : 0;
    }

private:
    mutable std::unique_ptr<std::stringstream> stream;
    uint64_t file_size = 0;
};

#if DPPC_HAS_PRIVATE_MMAP
class PrivateMmapBackend : public ImgFileBackend {
public:
    ~PrivateMmapBackend() override {
        close();
    }

    bool open(const std::string &img_path) override {
        fd = ::open(img_path.c_str(), O_RDONLY);
        if (fd < 0)
            return false;

        struct stat st {};
        if (::fstat(fd, &st) < 0 || st.st_size <= 0) {
            close();
            return false;
        }

        file_size = st.st_size;

        void *addr = ::mmap(nullptr, file_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
        if (addr == MAP_FAILED) {
            LOG_F(WARNING, "Private mmap failed: %s", std::strerror(errno));
            mapping = nullptr;
            close();
            return false;
        }

        mapping = static_cast<uint8_t *>(addr);
        return true;
    }

    void close() override {
        if (mapping) {
            ::munmap(mapping, file_size);
            mapping = nullptr;
        }
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }

    uint64_t size() const override {
        return file_size;
    }

    uint64_t read(void *buf, uint64_t offset, uint64_t length) const override {
        std::memcpy(buf, mapping + offset, length);
        return length;
    }

    uint64_t write(const void *buf, uint64_t offset, uint64_t length) override {
        std::memcpy(mapping + offset, buf, length);
        return length;
    }

private:
    int fd = -1;
    uint8_t *mapping = nullptr;
    uint64_t file_size = 0;
};
#endif

// ---------------------------------------------------------------------------
// Microsoft Virtual Hard Disk (VHD) support
//
// Dynamic VHD images are sparse: only blocks that have been written occupy
// space in the host file. This lets a 2 GB Macintosh disk use only a few tens
// of megabytes on the host until the guest actually writes data.
// ---------------------------------------------------------------------------

static uint32_t vhd_get_be32(const uint8_t *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
}

static uint64_t vhd_get_be64(const uint8_t *p) {
    return (uint64_t(vhd_get_be32(p)) << 32) | vhd_get_be32(p + 4);
}

static void vhd_put_be32(uint8_t *p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);  p[3] = uint8_t(v);
}

static void vhd_put_be64(uint8_t *p, uint64_t v) {
    vhd_put_be32(p, uint32_t(v >> 32));
    vhd_put_be32(p + 4, uint32_t(v));
}

static uint32_t vhd_checksum(const uint8_t *buf, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++)
        sum += buf[i];
    return ~sum;
}

static uint64_t vhd_align_up(uint64_t v, uint64_t a) {
    return ((v + a - 1) / a) * a;
}

static void vhd_calc_geometry(uint64_t size_bytes, uint16_t &cyl,
                              uint8_t &heads, uint8_t &spt) {
    uint64_t total_sectors = size_bytes / 512;
    if (total_sectors > 65535ULL * 16 * 255) {
        spt = 16; heads = 255;
    } else if (total_sectors >= 1024ULL * 16 * 63) {
        spt = 63; heads = 16;
    } else if (total_sectors >= 1024ULL * 8 * 63) {
        spt = 63; heads = 8;
    } else if (total_sectors >= 1024ULL * 4 * 63) {
        spt = 63; heads = 4;
    } else {
        spt = 17; heads = 4;
    }
    uint64_t c = total_sectors / (uint64_t(spt) * heads);
    if (c > 65535) c = 65535;
    cyl = uint16_t(c);
}

class DynamicVhdBackend : public ImgFileBackend {
public:
    bool open(const std::string &img_path) override;
    void close() override { stream.reset(); }

    uint64_t size() const override { return virtual_size; }

    uint64_t read(void *buf, uint64_t offset, uint64_t length) const override;
    uint64_t write(const void *buf, uint64_t offset, uint64_t length) override;

private:
    bool read_at(uint64_t off, void *buf, size_t len) const;
    bool write_at(uint64_t off, const void *buf, size_t len);
    bool ensure_block(uint32_t idx);
    bool write_bat_entry(uint32_t idx);

    mutable std::unique_ptr<std::fstream> stream;
    uint8_t  footer[512] = {};
    uint64_t file_size = 0;
    uint64_t virtual_size = 0;
    uint64_t table_offset = 0;
    uint32_t block_size = 0;
    uint32_t sectors_per_block = 0;
    uint32_t bitmap_size = 0;
    uint32_t max_entries = 0;
    bool     fixed = false;
    std::vector<uint32_t> bat;
};

bool DynamicVhdBackend::read_at(uint64_t off, void *buf, size_t len) const {
    stream->clear();
    stream->seekg(std::streamoff(off), std::ios::beg);
    stream->read(static_cast<char *>(buf), std::streamsize(len));
    return size_t(stream->gcount()) == len;
}

bool DynamicVhdBackend::write_at(uint64_t off, const void *buf, size_t len) {
    stream->clear();
    stream->seekp(std::streamoff(off), std::ios::beg);
    stream->write(static_cast<const char *>(buf), std::streamsize(len));
    return stream->good();
}

bool DynamicVhdBackend::open(const std::string &img_path) {
    stream = std::make_unique<std::fstream>(
        u8_path(img_path), std::ios::in | std::ios::out | std::ios::binary);
    if (!stream->is_open())
        return false;

    stream->seekg(0, std::ios::end);
    file_size = uint64_t(stream->tellg());
    stream->clear();
    if (file_size < 512)
        return false;

    if (!read_at(file_size - 512, footer, 512))
        return false;
    if (std::memcmp(footer, "conectix", 8) != 0)
        return false;

    uint32_t disk_type = vhd_get_be32(footer + 60);
    virtual_size = vhd_get_be64(footer + 48);

    if (disk_type == 3) { // fixed VHD
        fixed = true;
        if (virtual_size == 0 || virtual_size > file_size - 512)
            virtual_size = file_size - 512;
        return true;
    }

    if (disk_type != 2) // dynamic only (no differencing images)
        return false;

    uint64_t data_offset = vhd_get_be64(footer + 16);
    uint8_t header[1024];
    if (!read_at(data_offset, header, sizeof(header)))
        return false;
    if (std::memcmp(header, "cxsparse", 8) != 0)
        return false;

    table_offset  = vhd_get_be64(header + 16);
    max_entries   = vhd_get_be32(header + 28);
    block_size    = vhd_get_be32(header + 32);
    if (block_size == 0 || (block_size % 512) != 0 || max_entries == 0)
        return false;

    sectors_per_block = block_size / 512;
    bitmap_size = uint32_t(vhd_align_up((sectors_per_block + 7u) / 8u, 512));

    bat.assign(max_entries, 0xFFFFFFFFu);
    std::vector<uint8_t> raw(size_t(max_entries) * 4u);
    if (!read_at(table_offset, raw.data(), raw.size()))
        return false;
    for (uint32_t i = 0; i < max_entries; i++)
        bat[i] = vhd_get_be32(raw.data() + size_t(i) * 4u);

    if (virtual_size == 0)
        virtual_size = uint64_t(max_entries) * block_size;

    return true;
}

uint64_t DynamicVhdBackend::read(void *buf, uint64_t offset,
                                 uint64_t length) const {
    uint8_t *out = static_cast<uint8_t *>(buf);
    uint64_t done = 0;

    while (done < length && offset + done < virtual_size) {
        uint64_t cur = offset + done;

        if (fixed) {
            uint64_t chunk = std::min<uint64_t>(length - done, virtual_size - cur);
            if (!read_at(cur, out + done, size_t(chunk)))
                break;
            done += chunk;
            continue;
        }

        uint32_t blk = uint32_t(cur / block_size);
        uint64_t in_block = cur % block_size;
        uint64_t chunk = std::min<uint64_t>(length - done, block_size - in_block);

        if (blk >= bat.size() || bat[blk] == 0 || bat[blk] == 0xFFFFFFFFu) {
            std::memset(out + done, 0, size_t(chunk));
        } else {
            uint64_t off = uint64_t(bat[blk]) * 512 + bitmap_size + in_block;
            if (!read_at(off, out + done, size_t(chunk)))
                std::memset(out + done, 0, size_t(chunk));
        }
        done += chunk;
    }

    return done;
}

uint64_t DynamicVhdBackend::write(const void *buf, uint64_t offset,
                                  uint64_t length) {
    const uint8_t *in = static_cast<const uint8_t *>(buf);
    uint64_t done = 0;

    while (done < length && offset + done < virtual_size) {
        uint64_t cur = offset + done;

        if (fixed) {
            uint64_t chunk = std::min<uint64_t>(length - done, virtual_size - cur);
            if (!write_at(cur, in + done, size_t(chunk)))
                break;
            done += chunk;
            continue;
        }

        uint32_t blk = uint32_t(cur / block_size);
        uint64_t in_block = cur % block_size;
        if (blk >= bat.size())
            break;

        if (bat[blk] == 0 || bat[blk] == 0xFFFFFFFFu) {
            if (!ensure_block(blk))
                break;
        }

        uint64_t chunk = std::min<uint64_t>(length - done, block_size - in_block);
        uint64_t off = uint64_t(bat[blk]) * 512 + bitmap_size + in_block;
        if (!write_at(off, in + done, size_t(chunk)))
            break;
        done += chunk;
    }

    if (done)
        stream->flush();

    return done;
}

bool DynamicVhdBackend::write_bat_entry(uint32_t idx) {
    uint8_t entry[4];
    vhd_put_be32(entry, bat[idx]);
    return write_at(table_offset + uint64_t(idx) * 4, entry, sizeof(entry));
}

bool DynamicVhdBackend::ensure_block(uint32_t idx) {
    // The footer always lives at the very end of the file. A new block is
    // written where the footer currently starts, and the footer is moved
    // behind it.
    uint64_t footer_pos = file_size - 512;
    uint64_t block_pos  = vhd_align_up(footer_pos, 512);

    std::vector<uint8_t> bitmap(bitmap_size, 0xFF); // all sectors present
    std::vector<uint8_t> zeros(block_size, 0);

    if (!write_at(block_pos, bitmap.data(), bitmap.size()))
        return false;
    if (!write_at(block_pos + bitmap_size, zeros.data(), zeros.size()))
        return false;

    bat[idx] = uint32_t(block_pos / 512);
    if (!write_bat_entry(idx))
        return false;

    uint64_t new_footer = block_pos + bitmap_size + block_size;
    if (!write_at(new_footer, footer, 512))
        return false;

    file_size = new_footer + 512;
    return true;
}

bool vhd_create_dynamic(const std::string &img_path, uint64_t size_bytes) {
    size_bytes = (size_bytes + 511) & ~uint64_t(511);
    if (size_bytes < 512)
        return false;

    const uint32_t block_size = 2u * 1024u * 1024u;
    const uint32_t max_entries = uint32_t((size_bytes + block_size - 1) / block_size);
    const uint32_t bat_padded = uint32_t(vhd_align_up(uint64_t(max_entries) * 4u, 512));
    const uint64_t table_offset = 1536;
    const uint64_t footer_pos = table_offset + bat_padded;

    uint8_t footer[512] = {};
    std::memcpy(footer, "conectix", 8);
    vhd_put_be32(footer + 8, 0x00000002);       // features: reserved
    vhd_put_be32(footer + 12, 0x00010000);      // file format version
    vhd_put_be64(footer + 16, 512);             // data offset (dynamic header)
    uint32_t ts = uint32_t(std::time(nullptr) - 946684800ULL); // since 2000-01-01
    vhd_put_be32(footer + 24, ts);
    std::memcpy(footer + 28, "pmac", 4);        // creator application
    vhd_put_be32(footer + 32, 0x00010000);      // creator version
    std::memcpy(footer + 36, "Wi2k", 4);        // creator host OS
    vhd_put_be64(footer + 40, size_bytes);      // original size
    vhd_put_be64(footer + 48, size_bytes);      // current size

    uint16_t cyl; uint8_t heads, spt;
    vhd_calc_geometry(size_bytes, cyl, heads, spt);
    footer[56] = uint8_t(cyl >> 8);
    footer[57] = uint8_t(cyl);
    footer[58] = heads;
    footer[59] = spt;
    vhd_put_be32(footer + 60, 2);               // dynamic disk

    std::random_device rd;
    for (int i = 0; i < 16; i++)
        footer[68 + i] = uint8_t(rd());
    footer[84] = 0;                             // saved state
    vhd_put_be32(footer + 64, 0);
    vhd_put_be32(footer + 64, vhd_checksum(footer, 512));

    uint8_t header[1024] = {};
    std::memcpy(header, "cxsparse", 8);
    vhd_put_be64(header + 8, 0xFFFFFFFFFFFFFFFFULL);
    vhd_put_be64(header + 16, table_offset);
    vhd_put_be32(header + 24, 0x00010000);
    vhd_put_be32(header + 28, max_entries);
    vhd_put_be32(header + 32, block_size);
    vhd_put_be32(header + 36, 0);
    vhd_put_be32(header + 36, vhd_checksum(header, 1024));

    std::vector<uint8_t> bat(bat_padded, 0xFF);

    std::ofstream out(u8_path(img_path), std::ios::binary | std::ios::trunc);
    if (!out)
        return false;

    out.write(reinterpret_cast<const char *>(footer), 512); // footer copy at 0
    out.write(reinterpret_cast<const char *>(header), 1024);
    out.write(reinterpret_cast<const char *>(bat.data()), std::streamsize(bat.size()));
    out.write(reinterpret_cast<const char *>(footer), 512); // footer at end
    out.flush();
    (void)footer_pos;
    return out.good();
}

static bool has_vhd_extension(const std::string &path) {
    if (path.size() < 4)
        return false;
    std::string ext = path.substr(path.size() - 4);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return ext == ".vhd";
}

static std::unique_ptr<ImgFileBackend> make_imgfile_backend(const std::string &img_path) {
    std::unique_ptr<ImgFileBackend> backend;

    // Dynamic/fixed VHD images are handled by the sparse VHD backend. In
    // deterministic mode writes are suppressed, so use the normal read-only
    // backends instead.
    if (!is_deterministic && has_vhd_extension(img_path)) {
        backend = std::make_unique<DynamicVhdBackend>();
        if (backend->open(img_path))
            return backend;
        LOG_F(WARNING, "Could not open VHD image %s, trying as raw image.",
              img_path.c_str());
    }

    if (is_deterministic) {
#if DPPC_HAS_PRIVATE_MMAP
        backend = std::make_unique<PrivateMmapBackend>();
        if (backend->open(img_path))
            return backend;
        // Intentional fallthrough to MemoryStreamBackend if mmap fails.
#else
        backend = std::make_unique<MemoryStreamBackend>();
#endif
    } else {
        backend = std::make_unique<FileStreamBackend>();
    }

    if (!backend->open(img_path)) {
        return nullptr;
    }

    return backend;
}
