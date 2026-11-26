/*
 * PowerMac Emulator - persistent configuration handling (86Box-style INI).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "frontend.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace frontend {

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

FrontendConfig g_cfg;
std::map<std::string, std::string> g_props;
bool g_props_valid = false;
std::string g_active_config = "powermac.ini";

std::string trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool parse_bool(const std::string& v, bool def)
{
    std::string t = lower(trim(v));
    if (t.empty())  return def;
    if (t == "1" || t == "true" || t == "on" || t == "yes")  return true;
    if (t == "0" || t == "false" || t == "off" || t == "no") return false;
    return def;
}

std::vector<std::string> split_list(const std::string& s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            std::string t = trim(cur);
            if (!t.empty()) out.push_back(t);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    std::string t = trim(cur);
    if (!t.empty()) out.push_back(t);
    return out;
}

std::string join_list(const std::vector<std::string>& v, char sep)
{
    std::string out;
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i].empty()) continue;
        if (!out.empty()) out.push_back(sep);
        out += v[i];
    }
    return out;
}

int parse_int(const std::string& v, int def)
{
    std::string t = trim(v);
    if (t.empty()) return def;
    try { return std::stoi(t); } catch (...) { return def; }
}

void invalidate()
{
    g_props_valid = false;
    g_props.clear();
}

} // namespace

FrontendConfig& cfg()
{
    return g_cfg;
}

std::string default_config_path()
{
    return "powermac.ini";
}

void set_active_config_path(const std::string& path)
{
    g_active_config = path;
}

std::string active_config_path()
{
    return g_active_config;
}

bool load_config(const std::string& path)
{
    std::ifstream in(u8_path(path));
    if (!in)
        return false;

    FrontendConfig c = g_cfg; // keep defaults for keys not present
    std::string line, section;

    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        if (line.front() == '[' && line.back() == ']') {
            section = lower(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = lower(trim(line.substr(0, eq)));
        std::string val = trim(line.substr(eq + 1));

        if (section == "machine") {
            if      (key == "machine")     c.machine = val;
            else if (key == "cpu")         c.cpu = val;
            else if (key == "rom")         c.rom = val;
            else if (key == "working_dir") c.working_dir = val;
            else if (key == "ram_mb")      c.ram_mb = parse_int(val, c.ram_mb);
        } else if (section == "storage") {
            if      (key == "scsi_hdd") c.scsi_hdd = split_list(val, ';');
            else if (key == "scsi_cd")  c.scsi_cd  = split_list(val, ';');
            else if (key == "hdd_bus")  c.hdd_bus  = trim(val);
            else if (key == "cd_bus")   c.cd_bus   = trim(val);
            else if (key == "boot_cd")  c.boot_cd  = parse_bool(val, c.boot_cd);
            else if (key == "ide_hdd")  c.ide_hdd  = val;
            else if (key == "ide_hdd2") c.ide_hdd2 = val;
            else if (key == "ide_cd")   c.ide_cd   = val;
            else if (key == "fdd_img")  c.fdd_img  = val;
            else if (key == "fdd_wr_prot") c.fdd_wr_prot = parse_bool(val, c.fdd_wr_prot);
            else if (key == "fdd_fmt")  c.fdd_fmt  = val;
        } else if (section == "video") {
            if      (key == "pci_gpu")    c.pci_gpu = val;
            else if (key == "scale")      c.scale = parse_int(val, c.scale);
            else if (key == "fullscreen") c.fullscreen = parse_bool(val, c.fullscreen);
            else if (key == "mon_id")     c.mon_id = val;
        } else if (section == "network") {
            if (key == "enabled") c.net_enabled = parse_bool(val, c.net_enabled);
        } else if (section == "input") {
            if      (key == "auto_grab")            c.auto_grab = parse_bool(val, c.auto_grab);
            else if (key == "swap_command_option")  c.swap_cmd_opt = parse_bool(val, c.swap_cmd_opt);
            else if (key == "keyboard")             c.keyboard = val;
        } else if (section == "emulation") {
            if (key == "realtime") c.realtime = parse_bool(val, c.realtime);
            else if (key == "rhapsody") c.rhapsody = parse_bool(val, c.rhapsody);
            else if (key == "rhapsody_dr1") c.rhapsody_dr1 = parse_bool(val, c.rhapsody_dr1);
            else if (key == "sound") c.sound_enabled = parse_bool(val, c.sound_enabled);
        }
    }

    g_cfg = c;
    g_active_config = path;
    invalidate();
    apply_machine_defaults();
    return true;
}

bool save_config(const std::string& path)
{
    std::ofstream out(u8_path(path));
    if (!out)
        return false;

    out << "# PowerMac Emulator configuration\n";
    out << "# Generated by the 86Box-style settings dialog.\n\n";

    out << "[Machine]\n";
    out << "machine=" << g_cfg.machine << "\n";
    out << "cpu=" << g_cfg.cpu << "\n";
    out << "rom=" << g_cfg.rom << "\n";
    out << "working_dir=" << g_cfg.working_dir << "\n";
    out << "ram_mb=" << g_cfg.ram_mb << "\n\n";

    out << "[Storage]\n";
    out << "scsi_hdd=" << join_list(g_cfg.scsi_hdd, ';') << "\n";
    out << "scsi_cd=" << join_list(g_cfg.scsi_cd, ';') << "\n";
    out << "hdd_bus=" << g_cfg.hdd_bus << "\n";
    out << "cd_bus=" << g_cfg.cd_bus << "\n";
    out << "boot_cd=" << (g_cfg.boot_cd ? 1 : 0) << "\n";
    out << "ide_hdd=" << g_cfg.ide_hdd << "\n";
    out << "ide_hdd2=" << g_cfg.ide_hdd2 << "\n";
    out << "ide_cd=" << g_cfg.ide_cd << "\n";
    out << "fdd_img=" << g_cfg.fdd_img << "\n";
    out << "fdd_wr_prot=" << (g_cfg.fdd_wr_prot ? 1 : 0) << "\n";
    out << "fdd_fmt=" << g_cfg.fdd_fmt << "\n\n";

    out << "[Network]\n";
    out << "enabled=" << (g_cfg.net_enabled ? 1 : 0) << "\n\n";

    out << "[Video]\n";
    out << "pci_gpu=" << g_cfg.pci_gpu << "\n";
    out << "scale=" << g_cfg.scale << "\n";
    out << "fullscreen=" << (g_cfg.fullscreen ? 1 : 0) << "\n";
    out << "mon_id=" << g_cfg.mon_id << "\n\n";

    out << "[Input]\n";
    out << "auto_grab=" << (g_cfg.auto_grab ? 1 : 0) << "\n";
    out << "swap_command_option=" << (g_cfg.swap_cmd_opt ? 1 : 0) << "\n";
    out << "keyboard=" << g_cfg.keyboard << "\n\n";

    out << "[Emulation]\n";
    out << "realtime=" << (g_cfg.realtime ? 1 : 0) << "\n";
    out << "rhapsody=" << (g_cfg.rhapsody ? 1 : 0) << "\n";
    out << "rhapsody_dr1=" << (g_cfg.rhapsody_dr1 ? 1 : 0) << "\n";
    out << "sound=" << (g_cfg.sound_enabled ? 1 : 0) << "\n";

    g_active_config = path;
    return true;
}

void apply_machine_defaults()
{
    if (g_cfg.machine != "pm8500" && g_cfg.machine != "pm9500" &&
        g_cfg.machine != "pm8600" && g_cfg.machine != "pm9600" &&
        g_cfg.machine != "pmg3dt" && g_cfg.machine != "pmg3twr")
        g_cfg.machine = "pm8500";
    // Mac OS 9.2 requires a G3 (750) or later, so a G3 upgrade CPU is
    // available in addition to the stock 604/604e.
    if (g_cfg.cpu != "604" && g_cfg.cpu != "604e" && g_cfg.cpu != "750")
        g_cfg.cpu = "604";
    if (g_cfg.machine == "pm9500" && g_cfg.pci_gpu.empty())
        g_cfg.pci_gpu = "none";
    invalidate();
}

void apply_rhapsody_profile()
{
    g_cfg.rhapsody = true;

    if (g_cfg.rhapsody_dr1) {
        g_cfg.machine = "pm8500";
        g_cfg.cpu = "604";
        if (g_cfg.ram_mb < 512)
            g_cfg.ram_mb = 512;
        g_cfg.hdd_bus = "curio";
        g_cfg.cd_bus  = "curio";
        g_cfg.ide_hdd.clear();
        g_cfg.ide_cd.clear();
        g_cfg.ide_hdd2.clear();
        invalidate();
        return;
    }

    if (g_cfg.machine != "pmg3dt" && g_cfg.machine != "pmg3twr")
        g_cfg.machine = "pmg3dt";
    g_cfg.cpu = "750";

    if (g_cfg.ide_hdd.empty() && !g_cfg.scsi_hdd.empty())
        g_cfg.ide_hdd = g_cfg.scsi_hdd.front();
    if (g_cfg.ide_cd.empty() && !g_cfg.scsi_cd.empty())
        g_cfg.ide_cd = g_cfg.scsi_cd.front();

    g_cfg.scsi_hdd.clear();
    g_cfg.scsi_cd.clear();
    g_cfg.hdd_bus = "ide";
    g_cfg.cd_bus  = "ide";

    invalidate();
}

std::map<std::string, std::string> build_properties()
{
    std::map<std::string, std::string> p;

    p["cpu"] = g_cfg.cpu;

    // Distribute RAM across Hammerhead banks. Each bank accepts
    // 0/4/8/16/32/64/128 MB; bank 1 must be populated (no 0 value).
    for (int i = 0; i < 13; i++)
        p["rambank" + std::to_string(i) + "_size"] = "0";

    auto round_down = [](int v) {
        static const int sz[] = {128, 64, 32, 16, 8, 4};
        int chosen = 4;
        for (int s : sz) {
            if (v >= s) { chosen = s; break; }
        }
        return chosen;
    };

    int remaining = std::max(0, g_cfg.ram_mb);

    // Bank 1 is always present; use it first so the total is exact.
    if (remaining > 0) {
        int b1 = round_down(std::min(128, remaining));
        p["rambank1_size"] = std::to_string(b1);
        remaining -= b1;
    }

    for (int i = 0; i < 13 && remaining > 0; i++) {
        if (i == 1)
            continue;
        int chunk = round_down(std::min(128, remaining));
        p["rambank" + std::to_string(i) + "_size"] = std::to_string(chunk);
        remaining -= chunk;
    }

    // SCSI hard disks: external SCSI (Curio) by default for better
    // compatibility with disk initialization tools.
    if (g_cfg.hdd_bus == "mesh")
        p["hdd_img2"] = join_list(g_cfg.scsi_hdd, ';');
    else
        p["hdd_img"] = join_list(g_cfg.scsi_hdd, ';');

    // CD-ROMs can be attached to the external SCSI bus (Curio) or the
    // internal Fast SCSI bus (MESH). Without an image, do not create an empty
    // drive: the boot ROM may select it and wait for media.
    if (g_cfg.scsi_cd.empty()) {
        p["cdr_img"]  = "";
        p["cdr_img2"] = "";
        p["cdr_config"] = "";
    } else if (g_cfg.cd_bus == "mesh") {
        p["cdr_img2"] = join_list(g_cfg.scsi_cd, ';');
        p["cdr_config"] = "ScsiMesh:3";
    } else {
        p["cdr_img"] = join_list(g_cfg.scsi_cd, ';');
        p["cdr_config"] = g_cfg.rhapsody_dr1 ? "ScsiCurio:1" : "ScsiCurio:3";
    }

    // Optional IDE controller. Gossamer/Beige G3 uses the built-in MacIO
    // channels (Ide0/Ide1); TNT machines use the optional CMD646.
    const bool g3_ide = (g_cfg.machine == "pmg3dt" || g_cfg.machine == "pmg3twr");
    p["hdd_config"] = g3_ide ? "Ide0:0" : "CmdAta0:0";
    p["ide_hdd_img"] = g_cfg.ide_hdd;
    if (g3_ide) {
        p["hdd_config2"] = "Ide0:1";
        p["ide_hdd_img2"] = g_cfg.ide_hdd2;
    }
    if (!g_cfg.ide_cd.empty()) {
        p["cdr_config"] = g3_ide ? "Ide1:0" : "CmdAta0:1";
        p["cdr_img"] = g_cfg.ide_cd;
    }

    // Network card can be unplugged from the VM settings.
    p["fdd_img"] = g_cfg.fdd_img;
    p["fdd_wr_prot"] = g_cfg.fdd_wr_prot ? "1" : "0";
    p["fdd_fmt"] = g_cfg.fdd_fmt;
    p["net_enabled"] = g_cfg.net_enabled ? "1" : "0";
    p["screamer"] = g_cfg.sound_enabled ? "1" : "0";

    // The 9500 has no on-board video; plug a PCI graphics card into slot D2.
    p["mon_id"] = g_cfg.mon_id;

    if (g_cfg.machine == "pm9500" || g_cfg.machine == "pm9600") {
        p["pci_D2"] = (g_cfg.pci_gpu == "none") ? "" : g_cfg.pci_gpu;
    }

    g_props = p;
    g_props_valid = true;
    return p;
}

std::optional<std::string> property_override(const std::string& name)
{
    if (!g_props_valid)
        build_properties();

    auto it = g_props.find(name);
    if (it != g_props.end())
        return it->second;
    return std::nullopt;
}

} // namespace frontend
