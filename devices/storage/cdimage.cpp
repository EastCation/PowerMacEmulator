/*
 * PowerMac Emulator - CD image descriptor parsing (CUE/CCD) for CD-DA.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <devices/storage/cdimage.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

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

inline std::string to_u8(const std::filesystem::path& p)
{
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::string trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

std::string upper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return char(std::toupper(c)); });
    return s;
}

std::string lower_ext(const std::string& path)
{
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos)
        return "";
    std::string e = path.substr(dot);
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return e;
}

std::string unquote(const std::string& s)
{
    size_t q1 = s.find('"');
    if (q1 != std::string::npos) {
        size_t q2 = s.find('"', q1 + 1);
        if (q2 != std::string::npos)
            return s.substr(q1 + 1, q2 - q1 - 1);
    }
    std::istringstream is(s);
    std::string t;
    is >> t;
    return t;
}

uint32_t parse_msf(const std::string& s)
{
    int m = 0, sec = 0, f = 0;
    char c1 = 0, c2 = 0;
    std::istringstream is(s);
    is >> m >> c1 >> sec >> c2 >> f;
    if (!is)
        return 0;
    return uint32_t(m) * 60 * 75 + uint32_t(sec) * 75 + uint32_t(f);
}

} // namespace

bool CdImage::load(const std::string& path, std::string& error)
{
    tracks_.clear();
    data_file_.clear();
    leadout_ = 0;

    std::string ext = lower_ext(path);
    if (ext == ".cue")
        return parse_cue(path, error);
    if (ext == ".ccd")
        return parse_ccd(path, error);
    if (ext == ".mds")
        return parse_mds(path, error);

    error = "Unsupported CD descriptor (expected .cue, .ccd or .mds)";
    return false;
}

bool CdImage::parse_cue(const std::string& path, std::string& error)
{
    std::ifstream in(u8_path(path));
    if (!in) {
        error = "Could not open CUE file";
        return false;
    }

    std::filesystem::path base = u8_path(path).parent_path();
    std::string line, file_name;
    int cur = -1;

    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty())
            continue;

        std::string u = upper(line);
        if (u.rfind("FILE", 0) == 0) {
            std::string rest = trim(line.substr(4));
            std::string name = unquote(rest);
            if (!name.empty()) {
                std::filesystem::path p = u8_path(name);
                if (p.is_relative())
                    p = base / p;
                if (file_name.empty())
                    file_name = to_u8(p.lexically_normal());
            }
        } else if (u.rfind("TRACK", 0) == 0) {
            std::istringstream is(line.substr(5));
            CdTrack t;
            is >> t.number;
            std::string type;
            is >> type;
            type = upper(type);
            t.audio = (type == "AUDIO");
            if (type == "MODE1/2048" || type == "MODE2/2048" || type == "CDI/2048")
                t.sector_size = 2048;
            else
                t.sector_size = 2352;
            if (t.number > 0) {
                tracks_.push_back(t);
                cur = int(tracks_.size()) - 1;
            }
        } else if (u.rfind("INDEX 01", 0) == 0) {
            if (cur >= 0) {
                std::string msf = trim(line.substr(8));
                tracks_[size_t(cur)].start_lba = parse_msf(msf);
            }
        }
    }

    if (tracks_.empty()) {
        error = "CUE file contains no tracks";
        return false;
    }
    if (file_name.empty()) {
        error = "CUE file has no FILE entry";
        return false;
    }

    data_file_ = file_name;
    finalize(path, 2352);
    return true;
}

bool CdImage::parse_ccd(const std::string& path, std::string& error)
{
    std::ifstream in(u8_path(path));
    if (!in) {
        error = "Could not open CCD file";
        return false;
    }

    std::filesystem::path base = u8_path(path);
    base.replace_extension(".img");
    if (!std::filesystem::exists(base)) {
        // Some tools use an .img with a different base name; require .img.
        error = "CCD companion .img file not found";
        return false;
    }
    data_file_ = to_u8(std::filesystem::absolute(base).lexically_normal());

    std::string line;
    int cur = -1;
    int mode = -1;
    bool have_index = false;

    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';')
            continue;

        std::string u = upper(line);
        if (u.rfind("[TRACK", 0) == 0) {
            int num = 0;
            sscanf(line.c_str(), "[TRACK %d]", &num);
            CdTrack t;
            t.number = num;
            tracks_.push_back(t);
            cur = int(tracks_.size()) - 1;
            mode = -1;
            have_index = false;
        } else if (cur >= 0) {
            if (u.rfind("MODE=", 0) == 0) {
                mode = std::atoi(line.c_str() + 5);
                tracks_[size_t(cur)].audio = (mode == 0);
                tracks_[size_t(cur)].sector_size = 2352;
            } else if (u.rfind("INDEX 1=", 0) == 0 || u.rfind("INDEX 01=", 0) == 0) {
                const char* eq = std::strchr(line.c_str(), '=');
                if (eq) {
                    tracks_[size_t(cur)].start_lba = uint32_t(std::strtoul(eq + 1, nullptr, 0));
                    have_index = true;
                }
            }
        }
    }

    if (tracks_.empty()) {
        error = "CCD file contains no tracks";
        return false;
    }
    (void)have_index;

    std::sort(tracks_.begin(), tracks_.end(),
              [](const CdTrack& a, const CdTrack& b) { return a.start_lba < b.start_lba; });

    finalize(path, 2352);
    return true;
}

void CdImage::finalize(const std::string& path, uint32_t sector_hint)
{
    (void)path;
    (void)sector_hint;

    std::sort(tracks_.begin(), tracks_.end(),
              [](const CdTrack& a, const CdTrack& b) { return a.start_lba < b.start_lba; });

    uint64_t file_size = 0;
    {
        ImgFile f;
        if (f.open(data_file_))
            file_size = f.size();
    }

    for (size_t i = 0; i < tracks_.size(); i++) {
        CdTrack& t = tracks_[i];
        if (t.sector_size == 0)
            t.sector_size = 2352;

        if (i == 0) {
            t.file_offset = uint64_t(t.start_lba) * t.sector_size;
        } else {
            const CdTrack& p = tracks_[i - 1];
            uint32_t delta = t.start_lba > p.start_lba ? t.start_lba - p.start_lba : 0;
            t.file_offset = p.file_offset + uint64_t(delta) * p.sector_size;
        }
    }

    for (size_t i = 0; i < tracks_.size(); i++) {
        CdTrack& t = tracks_[i];
        if (i + 1 < tracks_.size()) {
            t.length_lba = tracks_[i + 1].start_lba > t.start_lba
                         ? tracks_[i + 1].start_lba - t.start_lba : 0;
        } else {
            if (file_size > t.file_offset)
                t.length_lba = uint32_t((file_size - t.file_offset) / t.sector_size);
            else
                t.length_lba = 0;
        }
    }

    if (!tracks_.empty()) {
        const CdTrack& last = tracks_.back();
        leadout_ = last.start_lba + last.length_lba;
    }
}

bool CdImage::has_audio() const
{
    for (const auto& t : tracks_)
        if (t.audio)
            return true;
    return false;
}

const CdTrack* CdImage::track_at(uint32_t lba) const
{
    for (const auto& t : tracks_) {
        uint32_t end = t.start_lba + t.length_lba;
        if (lba >= t.start_lba && lba < end)
            return &t;
    }
    return nullptr;
}

const CdTrack* CdImage::audio_track_at(uint32_t lba) const
{
    const CdTrack* t = track_at(lba);
    return (t && t->audio) ? t : nullptr;
}

bool CdImage::audio_offset(uint32_t lba, uint64_t& offset) const
{
    const CdTrack* t = audio_track_at(lba);
    if (!t)
        return false;
    offset = t->file_offset + uint64_t(lba - t->start_lba) * t->sector_size;
    return true;
}

namespace {

uint8_t mds8(const std::vector<uint8_t>& d, size_t o)
{
    return o < d.size() ? d[o] : 0;
}

uint16_t mds16(const std::vector<uint8_t>& d, size_t o)
{
    return o + 1 < d.size() ? uint16_t(d[o] | (uint16_t(d[o + 1]) << 8)) : 0;
}

uint32_t mds32(const std::vector<uint8_t>& d, size_t o)
{
    if (o + 3 >= d.size())
        return 0;
    return uint32_t(d[o]) | (uint32_t(d[o + 1]) << 8) |
           (uint32_t(d[o + 2]) << 16) | (uint32_t(d[o + 3]) << 24);
}

uint64_t mds64(const std::vector<uint8_t>& d, size_t o)
{
    return uint64_t(mds32(d, o)) | (uint64_t(mds32(d, o + 4)) << 32);
}

} // namespace

bool CdImage::parse_mds(const std::string& path, std::string& error)
{
    std::ifstream in(u8_path(path), std::ios::binary);
    if (!in) {
        error = "Could not open MDS file";
        return false;
    }

    std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());

    if (d.size() < 88 || std::memcmp(d.data(), "MEDIA DESCRIPTOR", 16) != 0 ||
        d[16] != 0x01) {
        error = "Invalid MDS header";
        return false;
    }

    uint16_t medium   = mds16(d, 18);
    uint16_t sessions = mds16(d, 20);
    uint32_t sess_off = mds32(d, 80);

    if (medium > 2) {
        error = "MDS medium is not a CD";
        return false;
    }
    if (sessions == 0 || sess_off + size_t(sessions) * 24 > d.size()) {
        error = "Invalid MDS session table";
        return false;
    }

    uint32_t session_end = 0;

    for (uint16_t s = 0; s < sessions; s++) {
        size_t so = sess_off + size_t(s) * 24;
        int32_t s_end = int32_t(mds32(d, so + 4));
        if (s_end > 0)
            session_end = std::max(session_end, uint32_t(s_end));

        uint8_t  nall = mds8(d, so + 10);
        uint16_t first = mds16(d, so + 12);
        uint16_t last  = mds16(d, so + 14);
        uint32_t tbo   = mds32(d, so + 20);
        (void)first;
        (void)last;

        for (uint8_t i = 0; i < nall; i++) {
            size_t bo = tbo + size_t(i) * 80;
            if (bo + 80 > d.size())
                break;

            uint8_t point = mds8(d, bo + 4);
            if (point == 0 || point >= 0xA0)
                continue;

            uint8_t  mode      = mds8(d, bo);
            uint32_t extra     = mds32(d, bo + 12);
            uint16_t ssize     = mds16(d, bo + 16);
            uint32_t start_lba = mds32(d, bo + 36);
            uint64_t start_off = mds64(d, bo + 40);
            uint32_t nfiles    = mds32(d, bo + 48);
            uint32_t footer    = mds32(d, bo + 52);

            CdTrack t;
            t.number   = point;
            uint8_t lm = mode & 0x0F;
            t.audio    = (lm == 0x01 || lm == 0x09);
            t.sector_size = ssize ? ssize : (t.audio ? 2352 : 2048);
            t.start_lba   = start_lba;
            t.file_offset = start_off;

            if (extra && extra + 8 <= d.size())
                t.length_lba = mds32(d, extra + 4);

            tracks_.push_back(t);

            if (data_file_.empty() && footer && nfiles &&
                footer + 16 <= d.size()) {
                uint32_t fn_off = mds32(d, footer);
                uint32_t wide   = mds32(d, footer + 4);
                std::string name;
                if (wide) {
                    for (size_t o = fn_off; o + 1 < d.size(); o += 2) {
                        uint16_t c = mds16(d, o);
                        if (!c)
                            break;
                        name.push_back(c < 128 ? char(c) : '?');
                    }
                } else {
                    for (size_t o = fn_off; o < d.size() && d[o]; o++)
                        name.push_back(char(d[o]));
                }

                if (!name.empty()) {
                    if (name[0] == '*') {
                        size_t dot = name.find('.');
                        std::string ext = dot != std::string::npos
                                        ? name.substr(dot) : ".mdf";
                        std::filesystem::path p = u8_path(path);
                        p.replace_extension(ext);
                        data_file_ = to_u8(p.lexically_normal());
                    } else {
                        std::filesystem::path p = u8_path(name);
                        if (p.is_relative())
                            p = u8_path(path).parent_path() / p;
                        data_file_ = to_u8(p.lexically_normal());
                    }
                }
            }
        }
    }

    if (tracks_.empty()) {
        error = "MDS contains no tracks";
        return false;
    }

    if (data_file_.empty()) {
        std::filesystem::path p = u8_path(path);
        p.replace_extension(".mdf");
        data_file_ = to_u8(p.lexically_normal());
    }
    if (!std::filesystem::exists(u8_path(data_file_))) {
        std::filesystem::path p = u8_path(data_file_);
        p.replace_extension(".MDF");
        if (std::filesystem::exists(p))
            data_file_ = to_u8(p.lexically_normal());
    }
    if (!std::filesystem::exists(u8_path(data_file_))) {
        error = "MDS companion MDF file not found";
        return false;
    }

    std::sort(tracks_.begin(), tracks_.end(),
              [](const CdTrack& a, const CdTrack& b) {
                  return a.start_lba < b.start_lba;
              });

    for (size_t i = 0; i < tracks_.size(); i++) {
        if (tracks_[i].length_lba != 0)
            continue;
        if (i + 1 < tracks_.size())
            tracks_[i].length_lba = tracks_[i + 1].start_lba - tracks_[i].start_lba;
        else if (session_end > tracks_[i].start_lba)
            tracks_[i].length_lba = session_end - tracks_[i].start_lba;
    }

    leadout_ = session_end;
    if (!tracks_.empty() &&
        leadout_ < tracks_.back().start_lba + tracks_.back().length_lba) {
        leadout_ = tracks_.back().start_lba + tracks_.back().length_lba;
    }

    return true;
}
