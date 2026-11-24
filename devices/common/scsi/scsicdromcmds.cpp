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

#include <core/memaccess.h>
#include <devices/common/scsi/scsicdromcmds.h>
#include <devices/storage/cdimage.h>
#include <loguru.hpp>

#include <algorithm>
#include <cstring>

ScsiCdromCmds::ScsiCdromCmds() {
    this->set_phys_block_dev(this);

    this->enable_cmd(ScsiCommand::READ_TOC);
    this->enable_cmd(ScsiCommand::SET_CD_SPEED);

    // CD-DA commands
    this->enable_cmd(ScsiCommand::READ_SUBCHANNEL);
    this->enable_cmd(ScsiCommand::PLAY_AUDIO_10);
    this->enable_cmd(ScsiCommand::PLAY_AUDIO_12);
    this->enable_cmd(ScsiCommand::PLAY_CD);
    this->enable_cmd(ScsiCommand::PAUSE_RESUME);
    this->enable_cmd(ScsiCommand::STOP_PLAY_SCAN);

    this->add_page_getter(this, ModePage::CDROM_AUDIO_CONTROL,
                          &ScsiCdromCmds::get_cdrom_audio_control_page);

    this->add_page_getter(this, ModePage::CDROM_CAPABILITIES,
                          &ScsiCdromCmds::get_cd_capabilities_page);

    this->add_page_getter(this, ModePage::VENDOR_PAGE_31,
                          &ScsiCdromCmds::get_apple_page_49);
}

void ScsiCdromCmds::process_command() {
    if (!this->ready_for_command()) {
        phy_impl->switch_phase(ScsiPhase::STATUS);
        return;
    }

    int next_phase;

    LOG_F(9, "Cdrom CDB %02X %02X %02X %02X %02X %02X %02X %02X "
                "%02X %02X %02X %02X (len=%d)",
          this->cdb_ptr[0], this->cdb_ptr[1], this->cdb_ptr[2],
          this->cdb_ptr[3], this->cdb_ptr[4], this->cdb_ptr[5],
          this->cdb_ptr[6], this->cdb_ptr[7], this->cdb_ptr[8],
          this->cdb_ptr[9], this->cdb_ptr[10], this->cdb_ptr[11],
          this->get_cdb_length(this->cdb_ptr[0]));

    // use non-disk buffer by default
    phy_impl->set_buffer(this->buf_ptr);

    switch(this->cdb_ptr[0]) {
    case ScsiCommand::READ_TOC:
        next_phase = this->read_toc();
        break;
    case ScsiCommand::SET_CD_SPEED:
        next_phase = this->set_cd_speed();
        break;
    case ScsiCommand::READ_SUBCHANNEL:
        next_phase = this->read_subchannel();
        break;
    case ScsiCommand::PLAY_AUDIO_10:
        next_phase = this->play_audio(10);
        break;
    case ScsiCommand::PLAY_AUDIO_12:
        next_phase = this->play_audio(12);
        break;
    case ScsiCommand::PLAY_CD:
        next_phase = this->play_cd();
        break;
    case ScsiCommand::PAUSE_RESUME:
        next_phase = this->pause_resume();
        break;
    case ScsiCommand::STOP_PLAY_SCAN:
        next_phase = this->stop_play();
        break;
    case 0x47: { // PLAY AUDIO MSF: used by the Mac OS AppleCD player
        uint32_t start = this->msf_to_lba(this->cdb_ptr[3], this->cdb_ptr[4],
                                          this->cdb_ptr[5]);
        uint32_t end = this->msf_to_lba(this->cdb_ptr[6], this->cdb_ptr[7],
                                        this->cdb_ptr[8]);
        if (start >= 150)
            start -= 150;
        if (end >= 150)
            end -= 150;
        else
            end = 0;

        const CdImage* img = this->get_cd_image();
        const CdTrack* tr = img ? img->track_at(start) : nullptr;
        if (!tr) {
            // 0xFFFFFF means "continue from the current position" on some
            // Apple CD drivers.
            start = this->audio_current_lba();
            tr = img ? img->track_at(start) : nullptr;
        }
        if (end <= start) {
            // The AppleCD player uses start == end to mean "play this track".
            if (tr)
                end = tr->start_lba + tr->length_lba;
            else
                end = 0;
        }

        LOG_F(INFO, "CD-DA: PLAY AUDIO MSF start=%u end=%u", start, end);

        if (!this->start_audio(start, end)) {
            LOG_F(WARNING, "CD-DA: PLAY AUDIO MSF failed at LBA %u", start);
            this->illegal_request(ScsiError::INVALID_CMD, 0, true);
        } else {
            this->phy_impl->set_status(ScsiStatus::GOOD);
        }
        next_phase = ScsiPhase::STATUS;
        break;
    }
    case 0x48: { // PLAY TRACK/INDEX: used by the AppleCD audio player
        uint8_t track = this->cdb_ptr[4];
        uint32_t start = 0;
        for (int i = 0; i < this->num_tracks; i++) {
            if (this->tracks[i].trk_num == track) {
                start = this->tracks[i].start_lba;
                break;
            }
        }
        if (!this->num_tracks) {
            this->illegal_request(ScsiError::INVALID_CMD, 0, true);
        } else if (!this->start_audio(start, 0)) {
            this->illegal_request(ScsiError::INVALID_CMD, 0, true);
        } else {
            phy_impl->set_status(ScsiStatus::GOOD);
            LOG_F(INFO, "CD-DA: PLAY TRACK %u from LBA %u", track, start);
        }
        next_phase = ScsiPhase::STATUS;
        break;
    }
    case 0xA4: // REPORT KEY: return a valid "no key" response
        std::memset(this->buf_ptr, 0, 8);
        phy_impl->set_xfer_len(8);
        phy_impl->set_status(ScsiStatus::GOOD);
        next_phase = ScsiPhase::DATA_IN;
        break;
    case 0x35: // SYNCHRONIZE CACHE: CD media is read-only
        phy_impl->set_status(ScsiStatus::GOOD);
        next_phase = ScsiPhase::STATUS;
        break;
    default:
        ScsiBlockCmds::process_command();
        return;
    }

    phy_impl->switch_phase(next_phase);
}

int ScsiCdromCmds::test_unit_ready() {
    if (this->consume_media_change()) {
        this->sense_key = ScsiSense::UNIT_ATTENTION;
        this->asc       = ScsiError::MEDIUM_CHANGED;
        this->ascq      = 0;
        phy_impl->set_status(ScsiStatus::CHECK_CONDITION, this->sense_key);
        return ScsiPhase::STATUS;
    }

    return ScsiCommonCmds::test_unit_ready();
}

void ScsiCdromCmds::eject_medium() {
    if (this->drive_locked) {
        LOG_F(INFO, "START_STOP_UNIT: medium eject prevented");
        this->field_ptr_valid = false;
        this->bit_ptr_valid   = false;
        this->illegal_request(ScsiError::REMOVAL_PREVENTED, 2, false);
    } else {
        LOG_F(INFO, "START_STOP_UNIT: medium eject requested");
        this->eject_image();
    }
}

int ScsiCdromCmds:: read_toc() {
    uint8_t start_track, session_num;
    int     tot_tracks, resp_len = 0;

    int     alloc_len  = this->get_xfer_len();
    bool    is_msf     = !!(this->cdb_ptr[1] & 2);
    bool    is_alt_fmt = false;

    // go directly to the STATUS phase if alloc_len = 0
    if (!alloc_len) {
        LOG_F(WARNING, "READ_TOC: skip data transfer because alloc_len = 0");
        return ScsiPhase::STATUS;
    }

    // SCSI-2 supports only one response format so the format field in CDB (byte 2)
    // is reserved and should be set to zero.
    // ATAPI CD-ROM draft rev 2.6 advises to grab format code from the upper
    // two bits of byte 9 if byte 2 is zero.
    // This extension is implemented in the most SCSI drives as well as in
    // Apple CD-ROM driver in Mac OS.
    // SCSI-3 devices should use byte 2 to specify the response format.
    // I decided to stick to the above described ATAPI way for now.
    // It should work for both SCSI and ATAPI as well as SCSI-3 devices.
    uint8_t format = this->cdb_ptr[2] & 7;

    if (!format) {
        format = this->cdb_ptr[9] >> 6;
        is_alt_fmt = true;
        LOG_F(9, "READ_TOC: grabbed format code 0x%X from byte 9", format);
    }

    uint8_t *out_ptr = this->buf_ptr;

    switch(format) {
    case 0: // Formatted TOC
        start_track = this->cdb_ptr[6];
        if (!start_track) { // special case: track zero (lead-in) as starting track
            // return all tracks starting with track 1 plus lead-out
            start_track = 1;
            tot_tracks  = this->num_tracks + 1;
        } else if (start_track == LEAD_OUT_TRK_NUM) {
            start_track = this->num_tracks + 1;
            tot_tracks  = 1;
        } else if (start_track <= this->num_tracks) {
            tot_tracks  = (this->num_tracks - start_track) + 2;
        } else {
            LOG_F(ERROR, "READ_TOC: invalid starting track %d", start_track);
            this->set_field_pointer(6),
            this->invalid_cdb();
            return ScsiPhase::STATUS;
        }

        // write TOC data header
        WRITE_WORD_BE_A(out_ptr, tot_tracks * 8 + 2); // TOC data length
        out_ptr[2] = 1; // 1st track number
        out_ptr[3] = this->num_tracks; // last track number
        out_ptr += 4;
        resp_len = 4;

        for (int tx = 0; tx < tot_tracks; tx++, out_ptr += 8, resp_len += 8) {
            if (resp_len >= alloc_len)
                break; // exit the loop if the output buffer is exhausted

            TrackDescriptor& td = this->tracks[start_track + tx - 1];

            // write TOC track descriptor
            out_ptr[0] = 0; // reserved
            out_ptr[1] = td.adr_ctrl;
            out_ptr[2] = td.trk_num;
            out_ptr[3] = 0; // reserved

            if (is_msf) {
                AddrMsf msf = lba_to_msf(td.start_lba + 150);
                out_ptr[4] = 0; // reserved
                out_ptr[5] = msf.min;
                out_ptr[6] = msf.sec;
                out_ptr[7] = msf.frm;
            } else
                WRITE_DWORD_BE_A(&out_ptr[4], td.start_lba);
        }
        break;
    case 1: // Multi-session info --> we currently support only one session
        resp_len = 12;
        std::memset(out_ptr, 0, resp_len);
        out_ptr[1] = 10; // TOC data length
        out_ptr[2] =  1; // first session number
        out_ptr[3] =  1; // last session number
        out_ptr[5] = this->tracks[0].adr_ctrl;
        out_ptr[6] = 1;
        if (is_msf) {
            AddrMsf msf = lba_to_msf(this->tracks[0].start_lba + 150);
            out_ptr[ 8] = 0; // reserved
            out_ptr[ 9] = msf.min;
            out_ptr[10] = msf.sec;
            out_ptr[11] = msf.frm;
        }
        break;
    case 2: // Raw TOC
        session_num = this->cdb_ptr[6];
        if (!session_num)
            session_num = 1;
        if (session_num > 1) {
            LOG_F(ERROR, "READ_TOC: invalid session number %d", session_num);
            this->set_field_pointer(6),
            this->invalid_cdb();
            return ScsiPhase::STATUS;
        }
        resp_len = 48;
        std::memset(out_ptr, 0, resp_len);
        // prepare data header
        out_ptr[1] = resp_len - 2; // TOC data length
        out_ptr[2] = 1; // first session number
        out_ptr[3] = 1; // last session number
        out_ptr += 4;
        // descriptor #1 -> first track
        out_ptr[0] = session_num;
        out_ptr[1] = this->tracks[0].adr_ctrl;
        out_ptr[3] = 0xA0; // point -> first track
        out_ptr[8] = 1; // first track number
        out_ptr += 11;
        // descriptor #2 -> last track
        out_ptr[0] = session_num;
        out_ptr[1] = this->tracks[0].adr_ctrl;
        out_ptr[3] = 0xA1; // point -> last track
        out_ptr[8] = 1; // last track number
        out_ptr += 11;
        // descriptor #3 -> lead-out
        out_ptr[0] = session_num;
        out_ptr[1] = this->tracks[0].adr_ctrl;
        out_ptr[3] = 0xA2; // point -> lead-out
        if (is_msf) {
            AddrMsf msf = lba_to_msf(this->tracks[this->num_tracks > 0 ? this->num_tracks : 1].start_lba + 150);
            out_ptr[ 7] = 0; // reserved
            out_ptr[ 8] = msf.min;
            out_ptr[ 9] = msf.sec;
            out_ptr[10] = msf.frm;
        } else
            WRITE_DWORD_BE_U(&out_ptr[7], this->tracks[this->num_tracks > 0 ? this->num_tracks : 1].start_lba);
        out_ptr += 11;
        // descriptor #4 -> data track
        out_ptr[0] = session_num;
        out_ptr[1] = this->tracks[0].adr_ctrl;
        out_ptr[3] = 1; // point -> data track
        if (is_msf) {
            AddrMsf msf = lba_to_msf(this->tracks[0].start_lba + 150);
            out_ptr[ 7] = 0; // reserved
            out_ptr[ 8] = msf.min;
            out_ptr[ 9] = msf.sec;
            out_ptr[10] = msf.frm;
        }
        break;
    default:
        LOG_F(ERROR, "READ_TOC: unsupported format %d", format);
        this->set_field_pointer(is_alt_fmt ? 9 : 2),
        this->set_bit_pointer(is_alt_fmt ? 7 : 3),
        this->invalid_cdb();
        return ScsiPhase::STATUS;
    }

    phy_impl->set_xfer_len(std::min(alloc_len, resp_len));

    return ScsiPhase::DATA_IN;
}

int ScsiCdromCmds::set_cd_speed() {
    phy_impl->set_status(ScsiStatus::GOOD);

    uint16_t new_rd_speed = READ_WORD_BE_U(&this->cdb_ptr[2]);

    if (new_rd_speed == 0xFFFFU || new_rd_speed > this->max_rd_speed)
        this->cur_rd_speed = this->max_rd_speed;
    else
        this->cur_rd_speed = new_rd_speed;

    LOG_F(INFO, "SET_CD_SPEED: drive speed set to %d kBps", this->cur_rd_speed);

    return ScsiPhase::STATUS;
}

int ScsiCdromCmds::get_cd_capabilities_page(uint8_t subpage, uint8_t ctrl,
                                            uint8_t *out_ptr, int avail_len)
{
    if (subpage && subpage != 0xFFU)
        return FORMAT_ERR_BAD_SUBPAGE;

    if (ctrl == 3)
        return FORMAT_ERR_BAD_CONTROL;

    int page_size = 28; // min length defined in MMC-3

    if (page_size > avail_len)
        return FORMAT_ERR_DATA_TOO_BIG;

    std::memset(out_ptr, 0, page_size); // clear everything

    out_ptr[ 0] = this->read_cap;
    out_ptr[ 1] = this->write_cap;
    out_ptr[ 2] = this->fmt_support;
    out_ptr[ 3] = this->ext_support;
    out_ptr[ 4] = (this->mech_type    << 5) | this->sw_lock_sup         |
                  (this->drive_locked << 1) | (this->prevent_jump << 2) |
                  (this->sw_eject_sup << 3);
    out_ptr[ 5] = this->more_support;
    out_ptr[15] = this->dgt_out_desc;

    WRITE_WORD_BE_A(&out_ptr[ 6], this->max_rd_speed); // max read speed
    WRITE_WORD_BE_A(&out_ptr[ 8], this->max_vol_levs); // max of volume levels
    WRITE_WORD_BE_A(&out_ptr[10], (this->cache_size / 1024)); // buffer size kB
    WRITE_WORD_BE_A(&out_ptr[12], this->cur_rd_speed); // current read speed

    return page_size;
}

int ScsiCdromCmds::get_cdrom_audio_control_page(uint8_t subpage, uint8_t ctrl,
                                                uint8_t *out_ptr, int avail_len)
{
    if (subpage && subpage != 0xFFU)
        return FORMAT_ERR_BAD_SUBPAGE;

    if (ctrl == 3)
        return FORMAT_ERR_BAD_CONTROL;

    int page_size = 14;

    if (page_size > avail_len)
        return FORMAT_ERR_DATA_TOO_BIG;

    std::memset(out_ptr, 0, page_size); // clear everything

    out_ptr[ 0] = (1 << 2); // Immed defaults to 1, SOTC defaults to 0

    // logical block per second of audio playback defaults to 75
    WRITE_WORD_BE_A(&out_ptr[4], 75);

    out_ptr[ 6] = 0x01; // Left Channel
    out_ptr[ 7] = 0xFF; // Volume (0-255)
    out_ptr[ 8] = 0x02; // Right Channel
    out_ptr[ 9] = 0xFF; // Volume (0-255)

    out_ptr[10] = 0x04; // Output Port 2
    out_ptr[11] = 0x00; // Volume (0-255)
    out_ptr[12] = 0x08; // Output Port 3
    out_ptr[13] = 0x00; // Volume (0-255)

    return page_size;
}

// Apple SCSI/ATAPI driver requests this page in the case of a device error.
// PearPC implements it under the name "Apple Features".
int ScsiCdromCmds::get_apple_page_49(uint8_t subpage, uint8_t ctrl,
                                     uint8_t *out_ptr, int avail_len)
{
    LOG_F(WARNING, "Page 0x31 requested");

    if (subpage && subpage != 0xFFU)
        return FORMAT_ERR_BAD_SUBPAGE;

    if (ctrl == 3)
        return FORMAT_ERR_BAD_CONTROL;

    int page_size = 6;

    if (page_size > avail_len)
        return FORMAT_ERR_DATA_TOO_BIG;

    std::memset(out_ptr, 0, page_size);

    out_ptr[0] = '.';
    out_ptr[1] = 'A';
    out_ptr[2] = 'p';
    out_ptr[3] = 'p';

    return page_size;
}

// ---------------------------------------------------------------------------
// CD-DA (Audio CD) commands
// ---------------------------------------------------------------------------

uint32_t ScsiCdromCmds::msf_to_lba(uint8_t m, uint8_t s, uint8_t f)
{
    auto bcd = [](uint8_t v) -> uint32_t {
        return uint32_t(((v >> 4) & 0xF) * 10 + (v & 0xF));
    };
    return (bcd(m) * 60 + bcd(s)) * 75 + bcd(f);
}

int ScsiCdromCmds::play_audio(int cdb_len)
{
    bool is_msf = (this->cdb_ptr[1] & 0x01) != 0;
    uint32_t start = 0;
    uint32_t length = 0;

    if (is_msf) {
        start = this->msf_to_lba(this->cdb_ptr[3], this->cdb_ptr[4],
                                 this->cdb_ptr[5]);
        if (start >= 150)
            start -= 150;
    } else {
        start = (uint32_t(this->cdb_ptr[2]) << 24) |
                (uint32_t(this->cdb_ptr[3]) << 16) |
                (uint32_t(this->cdb_ptr[4]) << 8)  |
                 uint32_t(this->cdb_ptr[5]);
    }

    if (cdb_len == 10) {
        length = (uint32_t(this->cdb_ptr[7]) << 8) | this->cdb_ptr[8];
    } else {
        length = (uint32_t(this->cdb_ptr[6]) << 24) |
                 (uint32_t(this->cdb_ptr[7]) << 16) |
                 (uint32_t(this->cdb_ptr[8]) << 8)  |
                  uint32_t(this->cdb_ptr[9]);
    }

    LOG_F(INFO, "CD-DA: PLAY AUDIO start=%u length=%u msf=%d", start, length,
          is_msf ? 1 : 0);

    uint32_t end = length ? start + length : 0;
    if (!this->start_audio(start, end)) {
        LOG_F(WARNING, "CD-DA: PLAY AUDIO failed (no audio track at LBA %u)", start);
        this->illegal_request(ScsiError::INVALID_CMD, 0, true);
        return ScsiPhase::STATUS;
    }

    return ScsiPhase::STATUS;
}

int ScsiCdromCmds::play_cd()
{
    // Apple vendor PLAY CD command (0xBC). The layout used by most drivers is
    // start MSF in bytes 2..4 and end MSF in bytes 5..7.
    uint32_t start = this->msf_to_lba(this->cdb_ptr[2], this->cdb_ptr[3],
                                      this->cdb_ptr[4]);
    if (start >= 150)
        start -= 150;

    uint32_t end = this->msf_to_lba(this->cdb_ptr[5], this->cdb_ptr[6],
                                    this->cdb_ptr[7]);
    if (end >= 150)
        end -= 150;
    else
        end = 0;

    LOG_F(INFO, "CD-DA: PLAY CD start=%u end=%u", start, end);

    if (!this->start_audio(start, end)) {
        LOG_F(WARNING, "CD-DA: PLAY CD failed (no audio track at LBA %u)", start);
        this->illegal_request(ScsiError::INVALID_CMD, 0, true);
        return ScsiPhase::STATUS;
    }

    return ScsiPhase::STATUS;
}

int ScsiCdromCmds::pause_resume()
{
    bool resume = (this->cdb_ptr[8] & 0x01) != 0;
    this->pause_audio(!resume);
    LOG_F(INFO, "CD-DA: %s", resume ? "RESUME" : "PAUSE");
    return ScsiPhase::STATUS;
}

int ScsiCdromCmds::stop_play()
{
    this->stop_audio();
    LOG_F(INFO, "CD-DA: STOP");
    return ScsiPhase::STATUS;
}

int ScsiCdromCmds::read_subchannel()
{
    uint32_t alloc = (uint32_t(this->cdb_ptr[7]) << 8) | this->cdb_ptr[8];
    uint8_t  format = this->cdb_ptr[3] & 0x0F;

    if (!alloc)
        return ScsiPhase::STATUS;

    uint8_t* out = this->buf_ptr;
    if (!out)
        return ScsiPhase::STATUS;

    uint8_t audio_status = 0x15; // no media
    if (this->medium_present()) {
        if (this->audio_playing())
            audio_status = 0x11; // playing
        else if (this->audio_paused())
            audio_status = 0x12; // paused
        else
            audio_status = 0x13; // stopped
    }

    if (format == 0 || format == 1) {
        uint32_t lba = this->medium_present() ? this->audio_current_lba() : 0;

        int track = 0;
        uint8_t adr_ctrl = 0x10;
        uint32_t track_lba = 0;

        const CdImage* img = this->get_cd_image();
        if (img) {
            const CdTrack* t = img->track_at(lba);
            if (t) {
                track = t->number;
                adr_ctrl = uint8_t(0x10 | (t->audio ? 0x00 : 0x04));
                track_lba = t->start_lba;
            }
        }

        AddrMsf abs_msf = this->lba_to_msf(int(lba) + 150);
        AddrMsf rel_msf = this->lba_to_msf(lba >= track_lba ? int(lba - track_lba) : 0);

        out[0] = 0;
        out[1] = audio_status;
        out[2] = 0;
        out[3] = 12; // subchannel data length

        out[4]  = 0;           // reserved
        out[5]  = adr_ctrl;
        out[6]  = uint8_t(track);
        out[7]  = 1;           // index
        out[8]  = abs_msf.min;
        out[9]  = abs_msf.sec;
        out[10] = abs_msf.frm;
        out[11] = rel_msf.min;
        out[12] = rel_msf.sec;
        out[13] = rel_msf.frm;
        out[14] = 0;
        out[15] = 0;

        uint32_t xfer = std::min<uint32_t>(alloc, 16);
        LOG_F(9, "READ_SUBCHANNEL fmt=%u alloc=%u status=0x%02X track=%d lba=%u",
              format, alloc, audio_status, track, lba);
        this->phy_impl->set_xfer_len(xfer);
        this->phy_impl->set_buffer(out);
        return ScsiPhase::DATA_IN;
    }

    if (format == 2) { // Media Catalog Number
        std::memset(out, 0, 24);
        out[0] = 0;
        out[1] = audio_status;
        out[2] = 0;
        out[3] = 24;
        uint32_t xfer = std::min<uint32_t>(alloc, 24);
        LOG_F(9, "READ_SUBCHANNEL fmt=2 alloc=%u status=0x%02X",
              alloc, audio_status);
        this->phy_impl->set_xfer_len(xfer);
        this->phy_impl->set_buffer(out);
        return ScsiPhase::DATA_IN;
    }

    // Unsupported subchannel format: return just the header.
    out[0] = 0;
    out[1] = audio_status;
    out[2] = 0;
    out[3] = 0;
    uint32_t xfer = std::min<uint32_t>(alloc, 4);
    this->phy_impl->set_xfer_len(xfer);
    this->phy_impl->set_buffer(out);
    return ScsiPhase::DATA_IN;
}
