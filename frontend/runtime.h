/*
 * PowerMac Emulator - front-end runtime hooks shared by the engine and the
 * SDL/Qt front-ends.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef POWERMAC_RUNTIME_H
#define POWERMAC_RUNTIME_H

#include <string>

/** Set by the UI "Pause" action; checked by the CPU loop. */
extern bool g_emulator_paused;

enum class DeviceActivity : int { Hdd = 0, Cdrom = 1, Floppy = 2, Network = 3 };

void frontend_activity(DeviceActivity kind, bool write);
uint32_t frontend_activity_serial(DeviceActivity kind);
bool frontend_activity_write(DeviceActivity kind);

/** Called while the guest is paused: pumps the UI and sleeps briefly. */
void frontend_idle();

/** Pumps the Qt event loop (no-op for other front-ends). */
void frontend_pump_events();

/** Ejects the currently inserted CD-ROM medium. */
void frontend_eject_cdrom();

/** Ejects current media (if any) and inserts another CD image at runtime. */
bool frontend_swap_cdrom(const std::string& path);

/** Ejects the floppy and inserts a new image. */
bool frontend_swap_floppy(const std::string& path, bool write_protect);
void frontend_eject_floppy();

/** Qt front-end entry points (no-ops unless built with Qt). */
void frontend_qt_init();
void frontend_qt_pump();
void frontend_qt_embed_window(void* native_handle, void* sdl_window,
                               int width, int height);
void frontend_qt_display_destroyed();
void frontend_qt_update_display_size(int width, int height);
void frontend_qt_present_frame(const uint32_t* pixels, int width, int height,
                                  bool guest_cursor);
void frontend_qt_present_blank();
void frontend_qt_release_caps();

#endif // POWERMAC_RUNTIME_H
