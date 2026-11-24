/*
 * PowerMac Emulator - front-end runtime hooks.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "runtime.h"

#include <core/hostevents.h>

#include <chrono>
#include <thread>

bool g_emulator_paused = false;

void frontend_idle()
{
    frontend_pump_events();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

void frontend_pump_events()
{
#ifdef POWERMAC_QT_UI
    frontend_qt_pump();
#endif
}

void frontend_eject_cdrom()
{
    CdromImageEvent ev{};
    ev.eject = true;
    EventManager::get_instance()->post_cdrom_event(ev);
}

bool frontend_swap_cdrom(const std::string& path)
{
    if (path.empty())
        return false;

    CdromImageEvent eject{};
    eject.eject = true;
    EventManager::get_instance()->post_cdrom_event(eject);

    CdromImageEvent insert{};
    insert.image_path = path;
    EventManager::get_instance()->post_cdrom_event(insert);

    return insert.result == CdromInsertionResult::SUCCESS;
}

#ifndef POWERMAC_QT_UI
void frontend_qt_init() {}
void frontend_qt_pump() {}
void frontend_qt_embed_window(void*, void*, int, int) {}
void frontend_qt_display_destroyed() {}
void frontend_qt_update_display_size(int, int) {}
void frontend_qt_present_frame(const uint32_t*, int, int, bool) {}
void frontend_qt_present_blank() {}
void frontend_qt_release_caps() {}
#endif
