#include "fb.hpp"
#include "input.hpp"
#include "pm3_session.hpp"
#include "ui.hpp"

#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <string>
#include <thread>
#include <vector>

namespace {

volatile sig_atomic_t g_stop = 0;

void handle_sig(int) {
    g_stop = 1;
}

int render_screenshot(const std::string& mode, const std::string& out_path) {
    ::setenv("HOME", "/tmp", 1);
    cpz::Framebuffer fb;
    if (!fb.init(true)) {
        std::cerr << "Failed to initialize headless framebuffer\n";
        return 1;
    }

    cpz::Pm3Session session;
    cpz::AppUI ui(fb, session);

    if (mode == "home") {
        ui.set_screen_for_demo(cpz::Screen::HOME);
    } else if (mode == "hf_menu" || mode == "hf") {
        ui.set_submenu_for_demo("HF / NFC", 1);
    } else if (mode == "lf_menu" || mode == "lf") {
        ui.set_submenu_for_demo("LF / RFID", 2);
    } else if (mode == "hw_menu" || mode == "hw") {
        ui.set_submenu_for_demo("Hardware / BWM", 3);
    } else if (mode == "cli" || mode == "cli_scroll") {
        session.seed_demo_output("hw bwm ble status", {
            {"[=] Session log started", cpz::LineKind::INFO},
            {"[+] Client: RRG/Proxmark3 (BLE enabled)", cpz::LineKind::SUCCESS},
            {"[+] BWM BLE: ON, Name: Proxmark5_BWM", cpz::LineKind::SUCCESS},
            {"[+] Pairing: disabled, TX: +9 dBm", cpz::LineKind::SUCCESS},
            {"[*] Scroll without Fn using F / X / Z / C", cpz::LineKind::WARN},
            {"[=] Ready for commands.", cpz::LineKind::INFO},
        });
        ui.set_cli_scroll_for_demo(mode == "cli_scroll");
    } else if (mode == "ports" || mode == "ports_bt") {
        session.seed_demo_ble({
            {"AA:BB:CC:11:22:33", "Proxmark5", true, false, true, true},
            {"AA:BB:CC:44:55:66", "PM3_RDV4.0", true, true, false, false},
            {"11:22:33:44:55:66", "BLE-Sensor", false, false, false, false},
        });
        session.set_selected_port("ble:AA:BB:CC:11:22:33");
        ui.set_screen_for_demo(cpz::Screen::PORTS);
        if (mode == "ports_bt") {
            ui.set_ports_demo_focus(9);
        }
    } else if (mode == "output") {
        session.seed_demo_output("hf search", {
            {"[=] Searching for ISO14443-A tag...", cpz::LineKind::INFO},
            {"[+]  UID: 04 A1 B2 C3 D4 E5 80", cpz::LineKind::SUCCESS},
            {"[+] ATQA: 00 44", cpz::LineKind::SUCCESS},
            {"[+]  SAK: 00 [2]", cpz::LineKind::SUCCESS},
            {"[+] Type: NXP NTAG215 (504 bytes)", cpz::LineKind::SUCCESS},
            {"[?] Hint: Try 'hf mfu info' for details", cpz::LineKind::HINT},
        });
        ui.set_screen_for_demo(cpz::Screen::OUTPUT);
    } else {
        std::cerr << "Unknown screenshot mode: " << mode << "\n";
        return 2;
    }

    ui.render(0);
    if (!fb.save_png(out_path)) {
        std::cerr << "Failed to write screenshot: " << out_path << "\n";
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 4 && std::strcmp(argv[1], "--screenshot") == 0) {
        return render_screenshot(argv[2], argv[3]);
    }

    std::signal(SIGINT, handle_sig);
    std::signal(SIGTERM, handle_sig);
    std::signal(SIGPIPE, SIG_IGN);

    cpz::Framebuffer fb;
    if (!fb.init(false)) {
        std::cerr << "Failed to initialize framebuffer: " << fb.device_path() << "\n";
        return 1;
    }

    cpz::Input input;
    input.init(false);

    cpz::Pm3Session session;
    cpz::AppUI ui(fb, session);

    ui.render(0);

    std::vector<cpz::KeyEvent> events;
    std::vector<struct pollfd> pfds;
    while (!g_stop && !ui.should_exit()) {
        const auto& in_fds = input.fds();
        int wait_ms = ui.recommended_poll_ms(input.is_esc_held());
        if (!in_fds.empty()) {
            pfds.clear();
            pfds.reserve(in_fds.size());
            for (int fd : in_fds) {
                pfds.push_back({fd, POLLIN, 0});
            }
            (void)::poll(pfds.data(), pfds.size(), wait_ms);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
        }

        input.poll(events);
        ui.handle_events(events);
        int esc_ms = input.esc_hold_ms();
        if (ui.needs_render(esc_ms)) {
            ui.render(esc_ms);
        }
    }

    session.close_session();
    input.shutdown();
    fb.shutdown();
    return 0;
}
