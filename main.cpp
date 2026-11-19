#include "hp_wmi.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

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

void print_usage() {
    std::cout
        << "victus - Lightweight HP Victus/OMEN Performance & Fan CLI\n\n"
        << "Commands:\n"
        << "  victus                  Interactive menu\n"
        << "  victus status           Read current fan speeds (RPM)\n"
        << "  victus perf             Set Performance mode (0x31)\n"
        << "  victus balanced | bal   Set Balanced mode (0x30)\n"
        << "  victus cool | eco       Set Cool/Eco mode (0x50)\n"
        << "  victus auto             Set fans to BIOS auto\n"
        << "  victus off              Turn off max/manual fan override + set Cool/Eco\n"
        << "  victus max              Set fans to Max speed\n"
        << "  victus fan <n> [n2]     Set manual fan speed in 100-RPM units (0=auto, or 20..60)\n";
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

bool handle_command(hp::WmiController& ctl, const std::string& cmd, int argc, char* argv[]) {
    if (cmd == "status" || cmd == "st") {
        print_status(ctl);
        return true;
    }
    if (cmd == "balanced" || cmd == "bal") {
        auto r = ctl.set_performance_mode(hp::PerformanceMode::Balanced);
        std::cout << (r.ok ? "[OK] Mode set to Balanced.\n" : "[ERROR] " + r.error + "\n");
        return r.ok;
    }
    if (cmd == "perf" || cmd == "performance") {
        auto r = ctl.set_performance_mode(hp::PerformanceMode::Performance);
        std::cout << (r.ok ? "[OK] Mode set to Performance.\n" : "[ERROR] " + r.error + "\n");
        return r.ok;
    }
    if (cmd == "cool" || cmd == "eco") {
        auto r = ctl.set_performance_mode(hp::PerformanceMode::Cool);
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
        auto r_mode = ctl.set_performance_mode(hp::PerformanceMode::Cool);
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
        if (argc < 3) {
            std::cerr << "Usage: victus fan <level> [fan2_level] (0=auto, or 20..60)\n";
            return false;
        }
        int f1 = std::atoi(argv[2]);
        int f2 = (argc >= 4) ? std::atoi(argv[3]) : f1;
        auto clamp_fan = [](int v) -> uint8_t {
            if (v <= 0) return 0;
            return static_cast<uint8_t>(std::clamp(v, 20, 60));
        };
        uint8_t u1 = clamp_fan(f1);
        uint8_t u2 = clamp_fan(f2);
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

void run_interactive(hp::WmiController& ctl) {
    while (true) {
        std::cout << "\n=== HP Victus Control ===\n";
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
        if (choice == 1) handle_command(ctl, "perf", 0, nullptr);
        else if (choice == 2) handle_command(ctl, "balanced", 0, nullptr);
        else if (choice == 3) handle_command(ctl, "cool", 0, nullptr);
        else if (choice == 4) handle_command(ctl, "fan-auto", 0, nullptr);
        else if (choice == 5) handle_command(ctl, "fan-max", 0, nullptr);
        else if (choice == 6) {
            int f1 = 0, f2 = 0;
            std::cout << "Enter Fan1 and Fan2 levels in 100-RPM units (0=auto, or 20..60, e.g. '35 35'): ";
            if (std::cin >> f1 >> f2) {
                auto clamp_fan = [](int v) -> uint8_t {
                    return v <= 0 ? 0 : static_cast<uint8_t>(std::clamp(v, 20, 60));
                };
                auto r = ctl.set_fan_levels(clamp_fan(f1), clamp_fan(f2));
                std::cout << (r.ok ? "[OK] Applied.\n" : "[ERROR] " + r.error + "\n");
            }
        }
    }
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc > 1) {
        std::string cmd = argv[1];
        if (cmd == "-h" || cmd == "--help" || cmd == "help") {
            print_usage();
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

    if (argc > 1) {
        return handle_command(ctl, argv[1], argc, argv) ? 0 : 1;
    }

    run_interactive(ctl);
    return 0;
}
