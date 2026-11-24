/*
 * PowerMac Emulator - CD image descriptor parsing (CUE/CCD) for CD-DA.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef CDIMAGE_H
#define CDIMAGE_H

#include <cinttypes>
#include <string>
#include <vector>

#include <utils/imgfile.h>

struct CdTrack {
    int      number      = 0;
    bool     audio       = false;
    uint32_t start_lba   = 0;
    uint32_t sector_size = 2352;
    uint64_t file_offset = 0;
    uint32_t length_lba  = 0;
};

class CdImage {
public:
    bool load(const std::string& path, std::string& error);

    const std::vector<CdTrack>& tracks() const { return tracks_; }
    uint32_t leadout_lba() const { return leadout_; }
    const std::string& data_file() const { return data_file_; }
    bool has_audio() const;

    const CdTrack* track_at(uint32_t lba) const;
    const CdTrack* audio_track_at(uint32_t lba) const;
    bool audio_offset(uint32_t lba, uint64_t& offset) const;

private:
    bool parse_cue(const std::string& path, std::string& error);
    bool parse_ccd(const std::string& path, std::string& error);
    bool parse_mds(const std::string& path, std::string& error);
    void finalize(const std::string& path, uint32_t sector_hint);

    std::vector<CdTrack> tracks_;
    std::string data_file_;
    uint32_t leadout_ = 0;
};

#endif // CDIMAGE_H
