/*
 * PowerMac Emulator - 86Box-style front-end
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef POWERMAC_FRONTEND_H
#define POWERMAC_FRONTEND_H

#include <map>
#include <optional>
#include <string>
#include <vector>

union SDL_Event;
struct SDL_Window;
struct SDL_Renderer;

/** Persistent configuration, 86Box-style. Mirrors powermac.ini. */
struct FrontendConfig {
    std::string machine     = "pm8500";   // pm8500 | pm9500
    std::string cpu         = "604";      // 604 | 604e
    std::string rom         = "roms/pm8500.rom";
    std::string working_dir = ".";
    int         ram_mb      = 64;

    // SCSI buses: curio = external SCSI, mesh = internal Fast SCSI
    std::vector<std::string> scsi_hdd;
    std::vector<std::string> scsi_cd;
    std::string              hdd_bus = "curio"; // curio (external SCSI) | mesh
    std::string              cd_bus  = "curio"; // curio (external SCSI) | mesh
    bool                     boot_cd = false;   // hold C while starting up

    // Optional PCI IDE (CMD646)
    std::string ide_hdd;
    std::string ide_cd;

    // PCI graphics adapter used by the 9500 (no on-board video)
    std::string pci_gpu = "none";
    std::string mon_id = "VGA-SVGA"; // "" = DDC/EDID; or Multiscan20in, AppleVision1710

    int  scale      = 2;
    bool fullscreen = false;

    bool        net_enabled  = true;
    bool        auto_grab    = true;
    bool        swap_cmd_opt = false;
    std::string keyboard     = "Eng_USA";
    bool        realtime     = true;
};

namespace frontend {

FrontendConfig& cfg();

std::string default_config_path();
bool        load_config(const std::string& path);
bool        save_config(const std::string& path);
void        set_active_config_path(const std::string& path);
std::string active_config_path();
void        apply_machine_defaults();

/** Values for the DingusPPC machine property system, derived from cfg(). */
std::map<std::string, std::string> build_properties();
std::optional<std::string>        property_override(const std::string& name);

/** SDL/ImGui life-cycle. */
bool ui_init(SDL_Window* window, SDL_Renderer* renderer);
void ui_shutdown();
bool ui_is_ready();
bool ui_process_event(const SDL_Event* event);

/** Draws the menu bar and dialogs, then submits the ImGui draw data. */
void ui_frame(SDL_Renderer* renderer);

/** Pixels (window coordinates) reserved at the top for the menu bar. */
int ui_top_inset();

/** Called once the guest has configured its display. */
void ui_mark_started();
bool ui_has_started();

/** Small blocking message window used before a machine can be started. */
void ui_message_loop(const std::string& title, const std::string& text);

} // namespace frontend

#endif // POWERMAC_FRONTEND_H
