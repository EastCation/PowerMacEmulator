/*
 * PowerMac Emulator - virtual machine manager
 *
 * Each virtual machine lives in its own directory below a user-selectable
 * root directory and contains a powermac.ini plus its disk images.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef POWERMAC_VMMANAGER_H
#define POWERMAC_VMMANAGER_H

#include <string>
#include <vector>

namespace vm {

struct VmEntry {
    std::string name;
    std::string folder;
    std::string config_path;
    std::string machine;
    std::string rom;
};

/** Editable per-VM settings (used by the Edit dialog). */
struct VmSettings {
    std::string machine = "pm8500";
    std::string cpu     = "604e";
    std::string rom;
    int         ram_mb  = 128;
    std::string scsi_hdd;              // ';' separated image list
    std::string scsi_cd;               // ';' separated image list
    std::string hdd_bus = "curio";     // curio | mesh
    std::string cd_bus  = "curio";     // curio | mesh
    std::string pci_gpu = "none";
    bool        net_enabled = true;
    bool        rhapsody = false;
};

/** Reads emulator.ini (VM root directory, last used VM). */
void set_program_dir(const std::string& dir);

void load_launcher_settings();
void save_launcher_settings();

std::string root_dir();
void        set_root_dir(const std::string& root);
std::string last_vm();

/** Scans the VM root directory for machines (subfolders with powermac.ini). */
std::vector<VmEntry> scan();

bool create_vm(const std::string& name, const std::string& machine,
               const std::string& cpu, int ram_mb, int disk_gb,
               const std::string& rom, const std::string& install_cd,
               std::string& error);
bool load_vm_settings(const std::string& name, VmSettings& out, std::string& error);
bool update_vm(const std::string& name, const VmSettings& settings,
               std::string& error);
bool delete_vm(const std::string& name, std::string& error);
bool open_folder(const std::string& folder);

/** Launches the emulator for the given VM (separate process). */
bool launch_vm(const std::string& name, const std::string& emulator_exe,
               std::string& error);

/** Requests that the given VM is started after the next emulator restart. */
bool start_vm(const std::string& name);

/** Absolute path of the last used VM at startup (empty if none). */
std::string startup_config_path();

/** Consumes a pending start request (used by the main restart loop). */
bool take_pending_start(std::string& config_path, std::string& work_dir);

} // namespace vm

#endif // POWERMAC_VMMANAGER_H
