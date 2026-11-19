#include "hp_wmi.hpp"

#include <shellapi.h>
#include <io.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr const char* kVersion = "1.1.0";

bool is_elevated() {
    BOOL admin = FALSE;
    SID_IDENTIFIER_AUTHORITY nt_auth = SECURITY_NT_AUTHORITY;
    PSID admin_group = nullptr;
    if (AllocateAndInitializeSid(
            &nt_auth, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &admin_group)) {
        CheckTokenMembership(nullptr, admin_group, &admin);
        FreeSid(admin_group);
    }
    return admin == TRUE;
}

/** Relaunch self via UAC prompt when started interactively without elevation. */
bool try_relaunch_elevated() {
    wchar_t exe_path[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;

    HINSTANCE res = ShellExecuteW(nullptr, L"runas", exe_path, nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(res) > 32;
}

uint8_t clamp_fan_level(int level_x100) {
    if (level_x100 <= 0) return 0;
    return static_cast<uint8_t>(std::clamp(level_x100, 20, 60));
}

void print_usage() {
    std::cout
        << "victus - Lightweight HP Victus/OMEN Performance & Fan CLI (v" << kVersion << ")\n\n"
        << "Commands:\n"
        << "  victus                  Interactive menu (auto-prompts Admin if double-clicked)\n"
        << "  victus status           Read current fan speeds (RPM)\n"
        << "  victus perf             Set Performance mode (v1: 0x31, v0: 0x01)\n"
        << "  victus balanced | bal   Set Balanced mode (v1: 0x30, v0: 0x00)\n"
        << "  victus cool | eco       Set Cool/Eco mode (v1: 0x50, v0: 0x02)\n"
        << "  victus auto             Set fans to BIOS auto\n"
        << "  victus off              Turn off max/manual fan override + set Cool/Eco\n"
        << "  victus max              Set fans to Max speed\n"
        << "  victus fan <n> [n2]     Set manual fan speed in 100-RPM units (0=auto, or 20..60)\n\n"
        << "Options:\n"
        << "  --v0                    Force legacy v0 thermal policy bytes (older Victus/OMEN)\n"
        << "  --v1                    Force modern v1 thermal policy bytes (default: auto)\n"
        << "  -v, --version           Print version\n"
        << "  -h, --help              Print help\n";
}

void print_status(hp::WmiController& ctl) {
    std::string err;
    auto st = ctl.get_fan_status(err);
    if (!st) {
        std::cerr << "[ERROR] Could not read fan speeds: " << err << "\n";
        return;
    }
    std::cout << "Fan 1: " << st->fan1_rpm << " RPM | Fan 2: " << st->fan2_rpm << " RPM\n";
}

bool handle_command(
    hp::WmiController& ctl,
    const std::vector<std::string>& args,
    hp::PolicyVersion policy_ver)
{
    const std::string& cmd = args[0];

    if (cmd == "status" || cmd == "st") {
        print_status(ctl);
        return true;
    }
    if (cmd == "balanced" || cmd == "bal") {
        auto r = ctl.set_thermal_profile(hp::ThermalProfile::Balanced, policy_ver);
        std::cout << (r.ok ? "[OK] Mode set to Balanced.\n" : "[ERROR] " + r.error + "\n");
        return r.ok;
    }
    if (cmd == "perf" || cmd == "performance") {
        auto r = ctl.set_thermal_profile(hp::ThermalProfile::Performance, policy_ver);
        std::cout << (r.ok ? "[OK] Mode set to Performance.\n" : "[ERROR] " + r.error + "\n");
        return r.ok;
    }
    if (cmd == "cool" || cmd == "eco") {
        auto r = ctl.set_thermal_profile(hp::ThermalProfile::Cool, policy_ver);
        std::cout << (r.ok ? "[OK] Mode set to Cool/Eco.\n" : "[ERROR] " + r.error + "\n");
        return r.ok;
    }
    if (cmd == "auto" || cmd == "fan-auto") {
        ctl.set_fan_max(false);
        auto r = ctl.set_fan_levels(0, 0);
        std::cout << (r.ok ? "[OK] Fans set to Auto.\n" : "[ERROR] " + r.error + "\n");
        return r.ok;
    }
    if (cmd == "off") {
        auto r_max = ctl.set_fan_max(false);
        auto r_lvl = ctl.set_fan_levels(0, 0);
        auto r_mode = ctl.set_thermal_profile(hp::ThermalProfile::Cool, policy_ver);
        if (r_mode.ok && (r_max.ok || r_lvl.ok)) {
            std::cout << "[OK] Max/manual fan override off; Cool/Eco mode active (BIOS manages fan minimum).\n";
            return true;
        }
        std::cerr << "[ERROR] Failed: "
                  << (!r_mode.ok ? r_mode.error : (!r_max.ok ? r_max.error : r_lvl.error)) << "\n";
        return false;
    }
    if (cmd == "max" || cmd == "fan-max") {
        auto r = ctl.set_fan_max(true);
        std::cout << (r.ok ? "[OK] Fans set to Max.\n" : "[ERROR] " + r.error + "\n");
        return r.ok;
    }
    if (cmd == "fan" || cmd == "fan-set") {
        if (args.size() < 2) {
            std::cerr << "Usage: victus fan <level> [fan2_level] (0=auto, or 20..60)\n";
            return false;
        }
        uint8_t u1 = clamp_fan_level(std::atoi(args[1].c_str()));
        uint8_t u2 = clamp_fan_level(args.size() >= 3 ? std::atoi(args[2].c_str()) : u1);
        auto r = ctl.set_fan_levels(u1, u2);
        if (r.ok) {
            std::cout << "[OK] Manual fan levels set: Fan1=" << (u1 * 100)
                      << " RPM, Fan2=" << (u2 * 100) << " RPM.\n";
        } else {
            std::cerr << "[ERROR] " << r.error << " (Some Victus 15 BIOS versions only allow auto / max.)\n";
        }
        return r.ok;
    }

    print_usage();
    return false;
}

void run_interactive(hp::WmiController& ctl, hp::PolicyVersion policy_ver) {
    while (true) {
        std::cout << "\n=== victus-lite (v" << kVersion << ") ===\n";
        print_status(ctl);
        std::cout
            << "1) Performance Mode\n"
            << "2) Balanced Mode\n"
            << "3) Cool / Eco Mode\n"
            << "4) Fan: Auto\n"
            << "5) Fan: Max\n"
            << "6) Fan: Manual RPM (if supported by BIOS)\n"
            << "7) Refresh Status\n"
            << "0) Exit\n"
            << "Select> ";

        int choice = -1;
        if (!(std::cin >> choice)) {
            std::cin.clear();
            std::cin.ignore(1024, '\n');
            continue;
        }

        if (choice == 0) break;
        if (choice == 1) handle_command(ctl, {"perf"}, policy_ver);
        else if (choice == 2) handle_command(ctl, {"balanced"}, policy_ver);
        else if (choice == 3) handle_command(ctl, {"cool"}, policy_ver);
        else if (choice == 4) handle_command(ctl, {"auto"}, policy_ver);
        else if (choice == 5) handle_command(ctl, {"max"}, policy_ver);
        else if (choice == 6) {
            int f1 = 0, f2 = 0;
            std::cout << "Enter Fan1 and Fan2 levels in 100-RPM units (0=auto, or 20..60, e.g. '35 35'): ";
            if (std::cin >> f1 >> f2) {
                auto r = ctl.set_fan_levels(clamp_fan_level(f1), clamp_fan_level(f2));
                std::cout << (r.ok ? "[OK] Applied.\n" : "[ERROR] " + r.error + "\n");
            }
        }
    }
}

} // namespace

int main(int argc, char* argv[]) {
    hp::PolicyVersion policy_ver = hp::PolicyVersion::Auto;
    std::vector<std::string> args;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help" || a == "help") {
            print_usage();
            return 0;
        }
        if (a == "-v" || a == "--version" || a == "version") {
            std::cout << "victus-lite v" << kVersion << "\n";
            return 0;
        }
        if (a == "--v0") {
            policy_ver = hp::PolicyVersion::V0;
            continue;
        }
        if (a == "--v1") {
            policy_ver = hp::PolicyVersion::V1;
            continue;
        }
        args.push_back(std::move(a));
    }

    if (args.empty() && !is_elevated() && _isatty(_fileno(stdin))) {
        if (try_relaunch_elevated()) {
            return 0;
        }
    }

    if (!is_elevated()) {
        std::cerr << "[WARNING] Not running as Administrator. HP WMI BIOS calls will likely fail.\n";
    }

    hp::WmiController ctl;
    std::string err;
    if (!ctl.init(err)) {
        std::cerr << "[FATAL] " << err << "\n";
        return 1;
    }

    if (!args.empty()) {
        return handle_command(ctl, args, policy_ver) ? 0 : 1;
    }

    run_interactive(ctl, policy_ver);
    return 0;
}
