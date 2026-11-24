/*
 * PowerMac Emulator - CD-DA audio playback (cubeb).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef CD_AUDIO_PLAYER_H
#define CD_AUDIO_PLAYER_H

#include <cstdint>
#include <memory>
#include <string>

class CdAudioPlayer {
public:
    CdAudioPlayer();
    ~CdAudioPlayer();

    bool start(const std::string& file, uint64_t start_offset,
               uint64_t end_offset, uint32_t start_lba);
    void pause(bool p);
    void stop();

    bool active() const;
    bool playing() const;
    bool paused() const;
    uint32_t current_lba() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

#endif // CD_AUDIO_PLAYER_H
