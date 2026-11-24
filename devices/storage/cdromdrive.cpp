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

/** @file Virtual CD-ROM device implementation. */

#include <core/hostevents.h>
#include <devices/storage/cdromdrive.h>
#include <devices/storage/cdimage.h>
#include <devices/storage/cdaudioplayer.h>
#include <loguru.hpp>

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <filesystem>
#include <cstring>
#include <string>

CdromDrive::CdromDrive() : BlockStorageDevice(31, CDR_STD_DATA_SIZE, 0xfffffffe) {
    EventManager::get_instance()->add_cdrom_handler(
        this, &CdromDrive::insert_image_event_handler);

    this->is_writeable = false;
    this->fmt_support |= CDFMT_AUDIO_PLAY | CDFMT_COMPOSITE |
                         CDFMT_PORT_1 | CDFMT_PORT_2;
    this->cd_audio = std::make_unique<CdAudioPlayer>();
}

CdromDrive::~CdromDrive()
{
    this->stop_audio();
}

void CdromDrive::insert_image_event_handler(CdromImageEvent& event) {
    if (event.handled)
        return;

    if (event.eject) {
        event.handled = true;
        if (this->eject_image()) {
            this->media_change_pending = true;
            event.result = CdromInsertionResult::SUCCESS;
        } else {
            event.result = CdromInsertionResult::NO_DRIVE;
        }
        return;
    }

    if (this->medium_present()) {
        event.result = CdromInsertionResult::MEDIA_PRESENT;
        return;
    }

    event.handled = true;
    if (this->insert_image(event.image_path)) {
        this->media_change_pending = true;
        event.result = CdromInsertionResult::SUCCESS;
    } else {
        event.result = CdromInsertionResult::IMAGE_OPEN_FAILED;
    }
}

bool CdromDrive::consume_media_change() {
    bool pending = this->media_change_pending;
    this->media_change_pending = false;
    return pending;
}

bool CdromDrive::insert_image(const std::string& filename) {
    if (this->medium_present()) {
        LOG_F(ERROR, "Cannot insert CD-ROM image while media is present");
        return false;
    }

    this->stop_audio();
    this->cd_image.reset();

    std::string ext;
    size_t dot = filename.find_last_of('.');
    if (dot != std::string::npos) {
        ext = filename.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
    }

    if (ext == ".cue" || ext == ".ccd" || ext == ".mds") {
        auto img = std::make_unique<CdImage>();
        std::string err;
        if (!img->load(filename, err)) {
            LOG_F(ERROR, "CD image: %s", err.c_str());
            return false;
        }

        if (!this->attach_image(img->data_file())) {
            LOG_F(ERROR, "Could not open CD data file, %s", img->data_file().c_str());
            return false;
        }

        bool any_2048 = false;
        bool first_data_2352 = false;
        const auto& tr = img->tracks();
        if (!tr.empty() && !tr[0].audio && tr[0].sector_size == 2352)
            first_data_2352 = true;
        for (const auto& t : tr)
            if (t.sector_size == 2048)
                any_2048 = true;

        if (any_2048) {
            this->set_block_size(2048);
            this->data_offset = 0;
        } else {
            this->set_block_size(2352);
            this->data_offset = first_data_2352 ? 16 : 0;
            if (first_data_2352)
                this->detect_raw_image();
        }

        int n = int(tr.size());
        if (n > CDROM_MAX_TRACKS - 1)
            n = CDROM_MAX_TRACKS - 1;
        for (int i = 0; i < n; i++) {
            this->tracks[i].trk_num  = uint8_t(tr[size_t(i)].number);
            this->tracks[i].adr_ctrl = uint8_t(0x10 | (tr[size_t(i)].audio ? 0x00 : 0x04));
            this->tracks[i].start_lba = tr[size_t(i)].start_lba;
        }
        this->num_tracks = n;
        this->tracks[n].trk_num  = LEAD_OUT_TRK_NUM;
        this->tracks[n].adr_ctrl = 0x14;
        this->tracks[n].start_lba = img->leadout_lba();

        this->cd_image = std::move(img);
        LOG_F(INFO, "CD image loaded: %d track(s), audio=%s", n,
              this->cd_image->has_audio() ? "yes" : "no");
        return true;
    }

    if (!this->attach_image(filename)) {
        LOG_F(ERROR, "Could not open CD-ROM image file, %s", filename.c_str());
        return false;
    }

    this->detect_raw_image();

    // create single track descriptor
    this->tracks[0]  = {1, /*.trk_num*/ 0x14, /*.adr_ctrl*/ 0 /*.start_lba*/};
    this->num_tracks = 1;

    // create Lead-out descriptor containing all data
    this->tracks[1] = {LEAD_OUT_TRK_NUM, /*.trk_num*/ 0x14, /*.adr_ctrl*/
        static_cast<uint32_t>(this->size_blocks + 1) /*.start_lba*/};

    return true;
}

bool CdromDrive::eject_image() {
    this->stop_audio();
    this->cd_image.reset();

    if (!this->detach_image())
        return false;

    this->num_tracks           = 0;
    this->media_change_pending = false;
    std::memset(this->tracks, 0, sizeof(this->tracks));

    return true;
}

bool CdromDrive::has_audio() const {
    return this->cd_image && this->cd_image->has_audio();
}

bool CdromDrive::start_audio(uint32_t start_lba, uint32_t end_lba) {
    if (!this->cd_image || !this->cd_audio)
        return false;

    const CdTrack* tr = this->cd_image->audio_track_at(start_lba);
    if (!tr)
        return false;

    uint64_t start_off;
    if (!this->cd_image->audio_offset(start_lba, start_off))
        return false;

    // Play across consecutive audio tracks, but stop at the next data track
    // or the lead-out.
    uint32_t limit = this->cd_image->leadout_lba();
    for (const auto& t : this->cd_image->tracks()) {
        if (t.start_lba > start_lba && !t.audio) {
            limit = std::min(limit, t.start_lba);
            break;
        }
    }
    if (end_lba == 0 || end_lba > limit)
        end_lba = limit;
    if (end_lba <= start_lba)
        return false;

    uint64_t end_off = start_off +
        uint64_t(end_lba - start_lba) * uint64_t(tr->sector_size);

    LOG_F(INFO, "CD-DA play LBA %u..%u (track %d)", start_lba, end_lba, tr->number);
    return this->cd_audio->start(this->cd_image->data_file(), start_off, end_off,
                                 start_lba);
}

void CdromDrive::pause_audio(bool pause) {
    if (this->cd_audio)
        this->cd_audio->pause(pause);
}

void CdromDrive::stop_audio() {
    if (this->cd_audio)
        this->cd_audio->stop();
}

bool CdromDrive::audio_playing() const {
    return this->cd_audio && this->cd_audio->playing();
}

bool CdromDrive::audio_paused() const {
    return this->cd_audio && this->cd_audio->paused();
}

uint32_t CdromDrive::audio_current_lba() const {
    return this->cd_audio ? this->cd_audio->current_lba() : 0;
}

bool CdromDrive::detect_raw_image() {
    uint8_t block_hdr[16];

    // let's see if the image data starts with the Mode 1/2 sync pattern
    this->img_file.read(block_hdr, 0, sizeof(block_hdr));

    for (int i = 1; i <= 10; i++)
        if (block_hdr[i] != 0xFF)
            return false;

    if (block_hdr[0] != 0 || block_hdr[11] != 0)
        return false;

    // for now, we only support Mode 1 images
    if (block_hdr[15] == 1) {
        this->set_block_size(2352);
        this->data_offset = 16;
        return true;
    }

    return false;
}

uint8_t CdromDrive::hex_to_bcd(const uint8_t val) {
    uint8_t hi = val / 10;
    uint8_t lo = val % 10;
    return (hi << 4) | lo;
}

AddrMsf CdromDrive::lba_to_msf(const int lba) {
    return {hex_to_bcd( lba / 4500),     /*.min*/
            hex_to_bcd((lba / 75) % 60), /*.sec*/
            hex_to_bcd( lba % 75)        /*.frm*/
    };
}
