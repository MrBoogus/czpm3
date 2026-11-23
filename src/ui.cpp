#include "ui.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <set>
#include <sys/stat.h>

namespace cpz {

namespace {

const std::vector<CommandItem>& hf_commands() {
    static const std::vector<CommandItem> items = {
        {"hf search",      "hf search",      "Detect any 13.56MHz HF / NFC tag"},
        {"hf 14a info",    "hf 14a info",    "ISO14443-A / NTAG / MIFARE card info"},
        {"hf mfu info",    "hf mfu info",    "MIFARE Ultralight / NTAG tag info"},
        {"hf mf info",     "hf mf info",     "MIFARE Classic magic & PRNG details"},
        {"hf mf autopwn",  "hf mf autopwn",  "Auto-recover MIFARE keys and dump"},
        {"hf mf dump",     "hf mf dump",     "Dump MIFARE Classic sectors to file"},
        {"hf 14b info",    "hf 14b info",    "ISO14443-B tag identification"},
        {"hf 15 info",     "hf 15 info",     "ISO15693 / Vicinity tag details"},
        {"hf iclass info", "hf iclass info", "HID iCLASS tag details"},
        {"hf felica info", "hf felica info", "Sony FeliCa / NFC-F tag info"},
        {"hf tune",        "hf tune",        "Measure 13.56MHz HF antenna tuning"},
    };
    return items;
}

const std::vector<CommandItem>& lf_commands() {
    static const std::vector<CommandItem> items = {
        {"lf search",        "lf search",                       "Detect any 125kHz / 134kHz LF tag"},
        {"lf em 410x reader","lf em 410x reader",               "Read EM410x / EM4102 ID card"},
        {"lf hid reader",    "lf hid reader",                   "Demodulate HID Prox 26/37-bit tag"},
        {"lf indala reader", "lf indala reader",                "Demodulate Motorola Indala tag"},
        {"lf awid reader",   "lf awid reader",                  "Demodulate AWID proximity card"},
        {"lf t55xx detect",  "lf t55xx detect",                 "Probe T55x7 modulation & config"},
        {"lf t55xx info",    "lf t55xx detect; lf t55xx info",  "Detect & show T55x7 block 0 config"},
        {"lf t55xx dump",    "lf t55xx detect; lf t55xx dump",  "Detect & dump T55x7 Page 0/1 blocks"},
        {"lf tune",          "lf tune",                         "Measure 125kHz LF antenna tuning"},
    };
    return items;
}

const std::vector<CommandItem>& hw_commands() {
    static const std::vector<CommandItem> items = {
        {"hw status",          "hw status",           "MCU memory, FPGA & antenna status"},
        {"hw version",         "hw version",          "Client & Proxmark firmware build"},
        {"hw tune",            "hw tune",             "Full LF + HF antenna voltage check"},
        {"hw ping",            "hw ping",             "Test USB / BT / BLE link latency"},
        {"hw bwm ble status",  "hw bwm ble status",   "PM5 BWM BLE settings & bonded devs"},
        {"hw bwm name",        "hw bwm name",         "Get or set PM5 BWM BLE adv name"},
        {"hw bwm autooff",     "hw bwm autooff",      "Show PM5 BWM auto power-off rules"},
        {"hw bwm powersave",   "hw bwm powersave",    "Show PM5 BWM power-save switch"},
        {"hw bwm wifi status", "hw bwm wifi --status","PM5 BWM WiFi STA status & IP"},
    };
    return items;
}

std::string basename_of(const std::string& p) {
    auto slash = p.rfind('/');
    return (slash == std::string::npos) ? p : p.substr(slash + 1);
}

std::string format_port_badge(const std::string& port) {
    if (port.rfind("ble:", 0) == 0) {
        std::string target = port.substr(4);
        if (target.size() == 17 && target[2] == ':' && target[5] == ':') {
            return "BLE:" + target.substr(9);
        }
        if (target.size() > 10) target.resize(10);
        return "BLE:" + target;
    }
    if (port.rfind("bt:", 0) == 0) {
        std::string target = port.substr(3);
        if (target.size() == 17 && target[2] == ':' && target[5] == ':') {
            return "BT:" + target.substr(9);
        }
        if (target.size() > 10) target.resize(10);
        return "BT:" + target;
    }
    if (port.rfind("tcp:", 0) == 0) {
        std::string target = port.substr(4);
        if (target.size() > 10) target.resize(10);
        return "TCP:" + target;
    }
    return "PM3:" + basename_of(port);
}

std::string format_size(size_t bytes) {
    char buf[32];
    if (bytes < 1024) {
        std::snprintf(buf, sizeof(buf), "%zu B", bytes);
    } else if (bytes < 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    return buf;
}

bool serial_lists_equal(const std::vector<SerialPortInfo>& a, const std::vector<SerialPortInfo>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].path != b[i].path || a[i].label != b[i].label || a[i].is_proxmark != b[i].is_proxmark) {
            return false;
        }
    }
    return true;
}

bool ble_lists_equal(const std::vector<BleDeviceInfo>& a, const std::vector<BleDeviceInfo>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].mac != b[i].mac || a[i].name != b[i].name ||
            a[i].is_proxmark != b[i].is_proxmark || a[i].is_classic_bt != b[i].is_classic_bt ||
            a[i].is_paired != b[i].is_paired) {
            return false;
        }
    }
    return true;
}

int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

AppUI::AppUI(Framebuffer& fb, Pm3Session& session) : fb_(fb), session_(session) {
    last_port_scan_ = std::chrono::steady_clock::now();
    ports_ = session_.scan_ports();
    ble_devices_ = session_.ble_devices_snapshot();
    cli_history_ = session_.load_cli_history();
}

bool AppUI::is_nav_up(const KeyEvent& ev) {
    return ev.action == KeyAction::UP || (ev.action == KeyAction::CHAR && (ev.ch == 'f' || ev.ch == 'F'));
}

bool AppUI::is_nav_down(const KeyEvent& ev) {
    return ev.action == KeyAction::DOWN || (ev.action == KeyAction::CHAR && (ev.ch == 'x' || ev.ch == 'X'));
}

bool AppUI::is_nav_left(const KeyEvent& ev) {
    return ev.action == KeyAction::LEFT || (ev.action == KeyAction::CHAR && (ev.ch == 'z' || ev.ch == 'Z'));
}

bool AppUI::is_nav_right(const KeyEvent& ev) {
    return ev.action == KeyAction::RIGHT || (ev.action == KeyAction::CHAR && (ev.ch == 'c' || ev.ch == 'C'));
}

bool AppUI::is_scroll_up(const KeyEvent& ev) {
    return is_nav_up(ev);
}

bool AppUI::is_scroll_down(const KeyEvent& ev) {
    return is_nav_down(ev);
}

bool AppUI::is_scroll_pgup(const KeyEvent& ev) {
    return ev.action == KeyAction::PGUP || is_nav_left(ev);
}

bool AppUI::is_scroll_pgdn(const KeyEvent& ev) {
    return ev.action == KeyAction::PGDN || is_nav_right(ev);
}

int AppUI::output_wrap_cols() const {
    int px = compact_text_ ? 9 : 11;
    int cw = std::max(5, fb_.char_width(px, false));
    return std::max(28, 300 / cw);
}

int AppUI::output_vis_lines() const {
    return compact_text_ ? 9 : 8;
}

int AppUI::cli_vis_lines() const {
    return compact_text_ ? 8 : 7;
}

IconId AppUI::icon_for_command(const std::string& cmd) {
    if (cmd.find("tune") != std::string::npos) return IconId::TUNE;
    if (cmd.find("ping") != std::string::npos) return IconId::PING;
    if (cmd.find("mf") != std::string::npos || cmd.find("t55xx") != std::string::npos ||
        cmd.find("iclass") != std::string::npos) return IconId::KEY;
    if (cmd.find("hw") != std::string::npos) return IconId::CHIP;
    if (cmd.find("search") != std::string::npos) return IconId::SCAN;
    return IconId::PLAY;
}

void AppUI::show_toast(const std::string& msg) {
    toast_msg_ = msg;
    toast_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    dirty_ = true;
}

void AppUI::open_home() {
    screen_ = Screen::HOME;
    viewing_file_ = false;
    custom_port_mode_ = false;
    dirty_ = true;
}

void AppUI::open_submenu(int category_idx) {
    sub_idx_ = 0;
    sub_scroll_ = 0;
    if (category_idx == 1) {
        sub_title_ = "HF / NFC";
        sub_items_ = hf_commands();
    } else if (category_idx == 2) {
        sub_title_ = "LF / RFID";
        sub_items_ = lf_commands();
    } else {
        sub_title_ = "HW / BWM";
        sub_items_ = hw_commands();
    }
    screen_ = Screen::SUBMENU;
    dirty_ = true;
}

void AppUI::launch_command(const std::string& cmd, Screen return_to) {
    viewing_file_ = false;
    return_screen_ = return_to;
    out_scroll_ = 0;
    out_follow_ = true;
    stop_requested_ = false;
    session_.clear_output();
    session_.run_command(cmd);
    screen_ = Screen::OUTPUT;
    dirty_ = true;
}

void AppUI::open_dumps() {
    dumps_.clear();
    dumps_idx_ = 0;
    dumps_scroll_ = 0;

    const char* home = std::getenv("HOME");
    std::string h = (home && *home) ? std::string(home) : "/home/pi";
    std::vector<std::string> dirs = {
        session_.dumps_dir(),
        h + "/.proxmark3/logs",
        h + "/.proxmark3",
        h,
    };

    std::set<std::string> seen_paths;
    static const char* kExts[] = {
        ".log", ".bin", ".eml", ".dump", ".json", ".key", ".mfd", ".dic", ".trace", ".pm3", ".txt"
    };

    for (const auto& d : dirs) {
        DIR* dir = ::opendir(d.c_str());
        if (!dir) continue;
        struct dirent* ent = nullptr;
        while ((ent = ::readdir(dir)) != nullptr) {
            std::string name(ent->d_name);
            if (name == "." || name == "..") continue;
            bool match = (d == session_.dumps_dir());
            if (!match) {
                for (const char* ext : kExts) {
                    size_t elen = std::strlen(ext);
                    if (name.size() >= elen && name.compare(name.size() - elen, elen, ext) == 0) {
                        match = true;
                        break;
                    }
                }
            }
            if (!match) continue;
            std::string full = d + "/" + name;
            if (!seen_paths.insert(full).second) continue;
            struct stat st {};
            if (::stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                dumps_.push_back({name, full, static_cast<size_t>(st.st_size), st.st_mtime});
            }
        }
        ::closedir(dir);
    }

    std::sort(dumps_.begin(), dumps_.end(), [](const DumpFileEntry& a, const DumpFileEntry& b) {
        if (a.mtime != b.mtime) return a.mtime > b.mtime;
        return a.name > b.name;
    });
    screen_ = Screen::DUMPS;
    dirty_ = true;
}

void AppUI::open_dump_file(const DumpFileEntry& entry) {
    viewing_file_ = true;
    view_title_ = entry.name;
    return_screen_ = Screen::DUMPS;
    out_scroll_ = 0;
    out_follow_ = false;
    file_lines_.clear();
    ++file_version_;

    bool is_bin = (entry.name.size() >= 4 &&
                   (entry.name.compare(entry.name.size() - 4, 4, ".bin") == 0 ||
                    entry.name.compare(entry.name.size() - 4, 4, ".mfd") == 0));

    std::ifstream ifs(entry.full_path, is_bin ? std::ios::binary : std::ios::in);
    if (!ifs) {
        file_lines_.push_back({"[!] Cannot read file", LineKind::ERROR});
    } else if (is_bin) {
        uint8_t buf[8];
        size_t offset = 0;
        while (ifs && file_lines_.size() < 1024) {
            ifs.read(reinterpret_cast<char*>(buf), sizeof(buf));
            std::streamsize got = ifs.gcount();
            if (got <= 0) break;
            char line[64];
            int pos = std::snprintf(line, sizeof(line), "%04zx: ", offset);
            for (int i = 0; i < 8; ++i) {
                if (i < got) {
                    pos += std::snprintf(line + pos, sizeof(line) - static_cast<size_t>(pos), "%02X ", buf[i]);
                } else {
                    pos += std::snprintf(line + pos, sizeof(line) - static_cast<size_t>(pos), "   ");
                }
            }
            for (int i = 0; i < got; ++i) {
                char ch = (buf[i] >= 32 && buf[i] < 127) ? static_cast<char>(buf[i]) : '.';
                line[pos++] = ch;
            }
            line[pos] = '\0';
            file_lines_.push_back({line, LineKind::INFO});
            offset += static_cast<size_t>(got);
        }
        if (file_lines_.empty()) {
            file_lines_.push_back({"(empty binary file)", LineKind::WARN});
        }
    } else {
        std::string line;
        while (std::getline(ifs, line) && file_lines_.size() < 2000) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            LineKind k = LineKind::NORMAL;
            if (line.find("[+]") != std::string::npos) k = LineKind::SUCCESS;
            else if (line.find("[!]") != std::string::npos || line.rfind("ERROR", 0) == 0) k = LineKind::ERROR;
            else if (line.find("[*]") != std::string::npos || line.find("[-]") != std::string::npos) k = LineKind::WARN;
            else if (line.find("[=]") != std::string::npos) k = LineKind::INFO;
            else if (line.find("[?]") != std::string::npos) k = LineKind::HINT;
            else if (line.find("[#]") != std::string::npos) k = LineKind::DEV;
            file_lines_.push_back({line, k});
        }
        if (file_lines_.empty()) {
            file_lines_.push_back({"(empty file)", LineKind::WARN});
        }
    }
    screen_ = Screen::OUTPUT;
    dirty_ = true;
}

bool AppUI::has_custom_active_port_row() const {
    if (session_.is_offline()) return false;
    std::string cur = session_.selected_port();
    if (cur.empty()) return false;
    for (const auto& p : ports_) {
        if (p.path == cur) return false;
    }
    for (const auto& b : ble_devices_) {
        if (cur == "bt:" + b.mac || cur == "ble:" + b.mac || cur == "ble:" + b.name) return false;
    }
    return true;
}

constexpr int kPortsStaticRows = 6;

void AppUI::clamp_ports_selection() {
    int extra = has_custom_active_port_row() ? 1 : 0;
    int total = kPortsStaticRows + static_cast<int>(ports_.size()) + static_cast<int>(ble_devices_.size()) + extra;
    if (ports_idx_ >= total) ports_idx_ = std::max(0, total - 1);
    if (ports_idx_ < 0) ports_idx_ = 0;
    int vis = custom_port_mode_ ? 3 : 4;
    if (ports_idx_ < ports_scroll_) ports_scroll_ = ports_idx_;
    if (ports_idx_ >= ports_scroll_ + vis) ports_scroll_ = ports_idx_ - vis + 1;
    int max_scroll = std::max(0, total - vis);
    if (ports_scroll_ > max_scroll) ports_scroll_ = max_scroll;
    if (ports_scroll_ < 0) ports_scroll_ = 0;
}

void AppUI::open_ports() {
    ports_ = session_.scan_ports();
    if (!session_.is_ble_scanning() && !session_.is_bt_pairing()) {
        session_.refresh_ble_devices_async();
    }
    ble_devices_ = session_.ble_devices_snapshot();
    wifi_on_ = Pm3Session::is_wifi_enabled();
    ports_idx_ = 0;
    ports_scroll_ = 0;
    custom_port_mode_ = false;
    clamp_ports_selection();
    screen_ = Screen::PORTS;
    dirty_ = true;
}

void AppUI::open_cli() {
    cli_scroll_mode_ = false;
    cli_follow_ = true;
    screen_ = Screen::CLI;
    dirty_ = true;
}

void AppUI::set_screen_for_demo(Screen s) {
    if (s == Screen::HOME) open_home();
    else if (s == Screen::CLI) open_cli();
    else if (s == Screen::PORTS) {
        ports_ = session_.scan_ports();
        ble_devices_ = session_.ble_devices_snapshot();
        wifi_on_ = true;
        ports_idx_ = 0;
        ports_scroll_ = 0;
        custom_port_mode_ = false;
        clamp_ports_selection();
        screen_ = Screen::PORTS;
        dirty_ = true;
    } else if (s == Screen::DUMPS) open_dumps();
    else if (s == Screen::OUTPUT) {
        viewing_file_ = false;
        return_screen_ = Screen::HOME;
        screen_ = Screen::OUTPUT;
        dirty_ = true;
    }
}

void AppUI::set_submenu_for_demo(const std::string& title, int category_idx) {
    open_submenu(category_idx);
    if (!title.empty()) sub_title_ = title;
}

void AppUI::set_cli_scroll_for_demo(bool scroll_mode) {
    open_cli();
    cli_scroll_mode_ = scroll_mode;
}

void AppUI::set_ports_demo_focus(int idx) {
    ports_idx_ = idx;
    clamp_ports_selection();
    dirty_ = true;
}

void AppUI::handle_events(const std::vector<KeyEvent>& events) {
    auto now = std::chrono::steady_clock::now();
    bool running_now = session_.is_running();
    if (was_running_ && !running_now) {
        stop_requested_ = false;
        dirty_ = true;
    }
    was_running_ = running_now;

    if (!running_now &&
        std::chrono::duration_cast<std::chrono::seconds>(now - last_port_scan_).count() >= 3) {
        last_port_scan_ = now;
        auto latest = session_.scan_ports();
        if (!serial_lists_equal(latest, ports_)) {
            ports_ = std::move(latest);
            clamp_ports_selection();
            dirty_ = true;
        }
    }

    bool ble_now = session_.is_ble_scanning();
    if (was_ble_scanning_ && !ble_now) {
        ble_devices_ = session_.ble_devices_snapshot();
        clamp_ports_selection();
        show_toast("BT/BLE: found " + std::to_string(ble_devices_.size()) + " device(s)");
    }
    was_ble_scanning_ = ble_now;

    bool pair_now = session_.is_bt_pairing();
    if (was_bt_pairing_ && !pair_now) {
        ble_devices_ = session_.ble_devices_snapshot();
        wifi_on_ = Pm3Session::is_wifi_enabled();
        clamp_ports_selection();
        show_toast(session_.bt_pair_status_msg());
    }
    was_bt_pairing_ = pair_now;

    if (!ble_now && !pair_now && screen_ == Screen::PORTS) {
        auto snap = session_.ble_devices_snapshot();
        if (!ble_lists_equal(snap, ble_devices_)) {
            ble_devices_ = std::move(snap);
            clamp_ports_selection();
            dirty_ = true;
        }
    }

    for (const auto& ev : events) {
        dirty_ = true;
        if (ev.action == KeyAction::EXIT_HOLD) {
            session_.abort();
            should_exit_ = true;
            return;
        }
        if (ev.action == KeyAction::CTRL_C && session_.is_running()) {
            session_.abort();
            stop_requested_ = true;
            show_toast("Aborted (^C)");
            continue;
        }

        switch (screen_) {
            case Screen::HOME:    handle_home(ev); break;
            case Screen::SUBMENU: handle_submenu(ev); break;
            case Screen::OUTPUT:  handle_output(ev); break;
            case Screen::DUMPS:   handle_dumps(ev); break;
            case Screen::CLI:     handle_cli(ev); break;
            case Screen::PORTS:   handle_ports(ev); break;
        }
    }
}

int AppUI::recommended_poll_ms(bool esc_held) const {
    if (esc_held) return 16;
    if (session_.is_running() || session_.is_ble_scanning() || session_.is_bt_pairing()) return 80;
    auto now = std::chrono::steady_clock::now();
    bool has_toast = !toast_msg_.empty() && now < toast_until_;
    bool has_cursor = (screen_ == Screen::CLI && !cli_scroll_mode_) ||
                      (screen_ == Screen::PORTS && custom_port_mode_);
    if (has_cursor || has_toast) return 250;
    return 500;
}

bool AppUI::needs_render(int esc_hold_ms) const {
    if (dirty_) return true;
    uint64_t ver = session_.version();
    if (ver != last_rendered_ver_) return true;

    int64_t now_ms = steady_ms();
    int spin = session_.is_running() ? static_cast<int>((now_ms / 125) % 4) : -1;
    if (spin != last_spin_frame_) return true;

    bool cursor_active = (screen_ == Screen::CLI && !cli_scroll_mode_) ||
                         (screen_ == Screen::PORTS && custom_port_mode_);
    bool cursor_on = cursor_active ? ((now_ms / 500) % 2 == 0) : false;
    if (cursor_on != last_cursor_on_) return true;

    bool toast_vis = !toast_msg_.empty() && (std::chrono::steady_clock::now() < toast_until_);
    if (toast_vis != last_toast_visible_) return true;

    int esc_bucket = (esc_hold_ms >= 180) ? ((esc_hold_ms / 40) + 1) : 0;
    if (esc_bucket != last_esc_bucket_) return true;

    return false;
}

void AppUI::handle_home(const KeyEvent& ev) {
    constexpr int kHomeCount = 7;
    constexpr int kVis = 4;
    if (is_nav_up(ev)) {
        home_idx_ = (home_idx_ + kHomeCount - 1) % kHomeCount;
    } else if (is_nav_down(ev)) {
        home_idx_ = (home_idx_ + 1) % kHomeCount;
    } else if (ev.action == KeyAction::TAB) {
        open_cli();
        return;
    } else if (ev.action == KeyAction::CHAR && (ev.ch == 'q' || ev.ch == 'Q')) {
        should_exit_ = true;
        return;
    } else if (ev.action == KeyAction::CHAR && ev.ch >= '1' && ev.ch <= '7') {
        home_idx_ = ev.ch - '1';
        if (home_idx_ < home_scroll_) home_scroll_ = home_idx_;
        if (home_idx_ >= home_scroll_ + kVis) home_scroll_ = home_idx_ - kVis + 1;
        KeyEvent enter;
        enter.action = KeyAction::ENTER;
        handle_home(enter);
        return;
    } else if (ev.action == KeyAction::ENTER) {
        switch (home_idx_) {
            case 0: launch_command("auto", Screen::HOME); return;
            case 1: open_submenu(1); return;
            case 2: open_submenu(2); return;
            case 3: open_submenu(3); return;
            case 4: open_dumps(); return;
            case 5: open_cli(); return;
            case 6: open_ports(); return;
        }
    }

    if (home_idx_ < home_scroll_) home_scroll_ = home_idx_;
    if (home_idx_ >= home_scroll_ + kVis) home_scroll_ = home_idx_ - kVis + 1;
}

void AppUI::handle_submenu(const KeyEvent& ev) {
    if (ev.action == KeyAction::BACK) {
        open_home();
        return;
    }
    if (ev.action == KeyAction::TAB) {
        open_cli();
        return;
    }
    int n = static_cast<int>(sub_items_.size());
    if (n == 0) return;

    if (is_nav_up(ev)) {
        sub_idx_ = (sub_idx_ + n - 1) % n;
    } else if (is_nav_down(ev)) {
        sub_idx_ = (sub_idx_ + 1) % n;
    } else if (ev.action == KeyAction::ENTER) {
        launch_command(sub_items_[sub_idx_].cmd, Screen::SUBMENU);
        return;
    }

    constexpr int kVis = 3;
    if (sub_idx_ < sub_scroll_) sub_scroll_ = sub_idx_;
    if (sub_idx_ >= sub_scroll_ + kVis) sub_scroll_ = sub_idx_ - kVis + 1;
}

void AppUI::handle_output(const KeyEvent& ev) {
    if (!viewing_file_ && session_.is_running()) {
        if (ev.action == KeyAction::ENTER) {
            if (session_.send_enter()) {
                show_toast("Sent <Enter>");
            }
            return;
        }
        if (ev.action == KeyAction::BACK) {
            if (!stop_requested_) {
                stop_requested_ = true;
                session_.request_stop();
                show_toast("Stopping (Esc again: force)");
            } else {
                session_.abort();
                show_toast("Force abort (^C)");
            }
            return;
        }
    } else if (ev.action == KeyAction::BACK) {
        screen_ = return_screen_;
        return;
    }

    if (ev.action == KeyAction::TAB) {
        open_cli();
        return;
    }
    if (ev.action == KeyAction::CHAR && (ev.ch == 'w' || ev.ch == 'W')) {
        compact_text_ = !compact_text_;
        show_toast(compact_text_ ? "Font: Compact (9px)" : "Font: Large (11px)");
        return;
    }
    if (!viewing_file_ && ev.action == KeyAction::CHAR) {
        if ((ev.ch == 'r' || ev.ch == 'R') && !session_.is_running()) {
            std::string cmd = session_.active_command();
            if (!cmd.empty()) {
                launch_command(cmd, return_screen_);
                return;
            }
        }
        if (ev.ch == 's' || ev.ch == 'S') {
            std::string saved = session_.save_log();
            show_toast(saved.empty() ? "Save failed" : ("Saved " + basename_of(saved)));
            return;
        }
    }

    int step = 0;
    if (is_scroll_up(ev)) step = -1;
    else if (is_scroll_down(ev)) step = 1;
    else if (is_scroll_pgup(ev)) step = -5;
    else if (is_scroll_pgdn(ev)) step = 5;

    if (step != 0) {
        int vis = output_vis_lines();
        int total = static_cast<int>(get_wrapped_lines().size());
        int max_off = std::max(0, total - vis);
        int base = out_follow_ ? max_off : std::min(out_scroll_, max_off);
        out_scroll_ = std::max(0, std::min(max_off, base + step));
        out_follow_ = (step > 0 && out_scroll_ >= max_off);
    }
}

void AppUI::handle_dumps(const KeyEvent& ev) {
    if (ev.action == KeyAction::BACK) {
        open_home();
        return;
    }
    if (ev.action == KeyAction::CHAR && (ev.ch == 'r' || ev.ch == 'R')) {
        open_dumps();
        show_toast("Refreshed");
        return;
    }
    int n = static_cast<int>(dumps_.size());
    if (n == 0) return;

    if (is_nav_up(ev)) {
        dumps_idx_ = (dumps_idx_ + n - 1) % n;
    } else if (is_nav_down(ev)) {
        dumps_idx_ = (dumps_idx_ + 1) % n;
    } else if (ev.action == KeyAction::ENTER) {
        open_dump_file(dumps_[dumps_idx_]);
        return;
    }

    constexpr int kVis = 4;
    if (dumps_idx_ < dumps_scroll_) dumps_scroll_ = dumps_idx_;
    if (dumps_idx_ >= dumps_scroll_ + kVis) dumps_scroll_ = dumps_idx_ - kVis + 1;
}

void AppUI::handle_ports(const KeyEvent& ev) {
    if (custom_port_mode_) {
        if (ev.action == KeyAction::BACK) {
            custom_port_mode_ = false;
            clamp_ports_selection();
            return;
        }
        if (ev.action == KeyAction::CHAR) {
            if (custom_port_input_.size() < 64) {
                custom_port_input_.push_back(ev.ch);
            }
            return;
        }
        if (ev.action == KeyAction::BACKSPACE) {
            if (!custom_port_input_.empty()) custom_port_input_.pop_back();
            return;
        }
        if (ev.action == KeyAction::ENTER) {
            if (!custom_port_input_.empty()) {
                session_.set_selected_port(custom_port_input_);
                show_toast("Target: " + custom_port_input_);
            }
            custom_port_mode_ = false;
            clamp_ports_selection();
            return;
        }
        return;
    }

    if (ev.action == KeyAction::BACK) {
        open_home();
        return;
    }

    int serial_cnt = static_cast<int>(ports_.size());
    int ble_cnt = static_cast<int>(ble_devices_.size());
    int extra = has_custom_active_port_row() ? 1 : 0;
    int total = kPortsStaticRows + serial_cnt + ble_cnt + extra;

    if (is_nav_up(ev)) {
        ports_idx_ = (ports_idx_ + total - 1) % total;
    } else if (is_nav_down(ev)) {
        ports_idx_ = (ports_idx_ + 1) % total;
    } else if (ev.action == KeyAction::CHAR && (ev.ch == 'p' || ev.ch == 'P')) {
        int b_idx = ports_idx_ - kPortsStaticRows - serial_cnt;
        if (b_idx >= 0 && b_idx < ble_cnt && ble_devices_[b_idx].is_classic_bt) {
            if (session_.start_bt_pair(ble_devices_[b_idx].mac, "1234")) {
                was_bt_pairing_ = true;
                show_toast("Pairing BT (PIN 1234)...");
            } else {
                show_toast("BT busy...");
            }
        }
    } else if (ev.action == KeyAction::ENTER) {
        if (ports_idx_ == 0) {
            if (session_.start_ble_scan()) {
                was_ble_scanning_ = true;
                show_toast("Scanning BT + BLE (~12s)...");
            } else {
                show_toast("Scan/pair already active...");
            }
        } else if (ports_idx_ == 1) {
            bool target_on = !wifi_on_;
            bool ok = Pm3Session::set_wifi_enabled(target_on);
            wifi_on_ = Pm3Session::is_wifi_enabled();
            if (ok) {
                show_toast(wifi_on_ ? "Wi-Fi ON" : "Wi-Fi OFF (BlueShark ready)");
            } else {
                show_toast("Wi-Fi toggle failed");
            }
        } else if (ports_idx_ == 2) {
            ports_ = session_.scan_ports();
            show_toast("USB: " + std::to_string(ports_.size()) + " port(s)");
        } else if (ports_idx_ == 3) {
            if (session_.is_offline()) {
                show_toast("Select a port / BT / BLE first");
            } else {
                launch_command("hw ping", Screen::PORTS);
                return;
            }
        } else if (ports_idx_ == 4) {
            custom_port_mode_ = true;
            std::string cur = session_.selected_port();
            custom_port_input_ = Pm3Session::is_non_serial_port(cur) ? cur : "bt:";
        } else if (ports_idx_ == 5) {
            session_.set_selected_port("");
            show_toast("Switched to OFFLINE");
        } else if (ports_idx_ < kPortsStaticRows + serial_cnt) {
            int p_idx = ports_idx_ - kPortsStaticRows;
            session_.set_selected_port(ports_[p_idx].path);
            show_toast("Port: " + basename_of(ports_[p_idx].path));
        } else if (ports_idx_ < kPortsStaticRows + serial_cnt + ble_cnt) {
            int b_idx = ports_idx_ - kPortsStaticRows - serial_cnt;
            if (b_idx >= 0 && b_idx < ble_cnt) {
                const auto& dev = ble_devices_[b_idx];
                if (dev.is_classic_bt && !dev.is_paired) {
                    if (session_.start_bt_pair(dev.mac, "1234")) {
                        was_bt_pairing_ = true;
                        show_toast("Pairing BT (PIN 1234)...");
                    } else {
                        show_toast("BT busy...");
                    }
                } else if (dev.is_classic_bt) {
                    session_.set_selected_port("bt:" + dev.mac);
                    show_toast("BT: " + dev.name);
                } else {
                    session_.set_selected_port("ble:" + dev.mac);
                    show_toast("BLE: " + dev.name);
                }
            }
        } else if (extra > 0) {
            launch_command("hw ping", Screen::PORTS);
            return;
        }
    }

    clamp_ports_selection();
}

void AppUI::handle_cli(const KeyEvent& ev) {
    if (ev.action == KeyAction::BACK) {
        if (session_.is_running()) {
            if (!stop_requested_) {
                stop_requested_ = true;
                session_.request_stop();
                show_toast("Stopping (Esc again: force)");
            } else {
                session_.abort();
                show_toast("Force abort (^C)");
            }
        } else {
            open_home();
        }
        return;
    }
    if (ev.action == KeyAction::TAB) {
        cli_scroll_mode_ = !cli_scroll_mode_;
        return;
    }

    if (cli_scroll_mode_) {
        int step = 0;
        if (is_scroll_up(ev)) step = -1;
        else if (is_scroll_down(ev)) step = 1;
        else if (is_scroll_pgup(ev)) step = -5;
        else if (is_scroll_pgdn(ev)) step = 5;

        if (step != 0) {
            int vis = cli_vis_lines();
            int total = static_cast<int>(get_wrapped_lines().size());
            int max_off = std::max(0, total - vis);
            int base = cli_follow_ ? max_off : std::min(cli_scroll_, max_off);
            cli_scroll_ = std::max(0, std::min(max_off, base + step));
            cli_follow_ = (step > 0 && cli_scroll_ >= max_off);
            return;
        }
        if (ev.action == KeyAction::CHAR && (ev.ch == 'w' || ev.ch == 'W')) {
            compact_text_ = !compact_text_;
            show_toast(compact_text_ ? "Font: Compact (9px)" : "Font: Large (11px)");
            return;
        }
        if (ev.action == KeyAction::CHAR && (ev.ch == 's' || ev.ch == 'S')) {
            std::string saved = session_.save_log();
            show_toast(saved.empty() ? "Save failed" : ("Saved " + basename_of(saved)));
            return;
        }
        if (ev.action != KeyAction::CHAR) return;
        cli_scroll_mode_ = false;
    }

    if (ev.action == KeyAction::CTRL_U) {
        cli_input_.clear();
        cli_hist_idx_ = -1;
    } else if (ev.action == KeyAction::CTRL_L) {
        session_.clear_output();
        cli_scroll_ = 0;
        cli_follow_ = true;
        show_toast("Cleared");
    } else if (ev.action == KeyAction::CHAR) {
        if (cli_input_.size() < 120) {
            cli_input_.push_back(ev.ch);
        }
    } else if (ev.action == KeyAction::BACKSPACE) {
        if (!cli_input_.empty()) cli_input_.pop_back();
    } else if (ev.action == KeyAction::UP) {
        if (!cli_history_.empty()) {
            if (cli_hist_idx_ < 0) cli_hist_idx_ = static_cast<int>(cli_history_.size()) - 1;
            else if (cli_hist_idx_ > 0) --cli_hist_idx_;
            cli_input_ = cli_history_[cli_hist_idx_];
        }
    } else if (ev.action == KeyAction::DOWN) {
        if (cli_hist_idx_ >= 0) {
            if (cli_hist_idx_ + 1 < static_cast<int>(cli_history_.size())) {
                ++cli_hist_idx_;
                cli_input_ = cli_history_[cli_hist_idx_];
            } else {
                cli_hist_idx_ = -1;
                cli_input_.clear();
            }
        }
    } else if (ev.action == KeyAction::ENTER) {
        if (session_.is_running()) {
            if (session_.send_enter()) {
                show_toast("Sent <Enter>");
            }
        } else if (!cli_input_.empty()) {
            std::string cmd = cli_input_;
            if (cli_history_.empty() || cli_history_.back() != cmd) {
                cli_history_.push_back(cmd);
                if (cli_history_.size() > 50) {
                    cli_history_.erase(cli_history_.begin());
                }
                session_.save_cli_history(cli_history_);
            }
            cli_hist_idx_ = -1;
            cli_input_.clear();
            cli_follow_ = true;
            stop_requested_ = false;
            session_.run_command(cmd);
        }
    }
}

std::vector<OutputLine> AppUI::wrap_lines(const std::vector<OutputLine>& src, int max_cols) const {
    std::vector<OutputLine> out;
    out.reserve(src.size() + 8);
    if (max_cols <= 0) return out;

    for (const auto& item : src) {
        const std::string& s = item.text;
        size_t pos = 0;
        size_t cp_start = 0;
        int cols = 0;

        while (pos < s.size()) {
            unsigned char b = static_cast<unsigned char>(s[pos]);
            size_t adv = 1;
            if ((b & 0xE0) == 0xC0 && pos + 1 < s.size()) adv = 2;
            else if ((b & 0xF0) == 0xE0 && pos + 2 < s.size()) adv = 3;
            else if ((b & 0xF8) == 0xF0 && pos + 3 < s.size()) adv = 4;

            if (cols >= max_cols) {
                out.push_back({s.substr(cp_start, pos - cp_start), item.kind});
                cp_start = pos;
                cols = 0;
            }
            pos += adv;
            ++cols;
        }
        if (cp_start == 0) {
            out.push_back(item);
        } else if (cp_start < s.size()) {
            out.push_back({s.substr(cp_start), item.kind});
        }
    }
    return out;
}

const std::vector<OutputLine>& AppUI::get_wrapped_lines() const {
    int cols = output_wrap_cols();
    if (screen_ == Screen::OUTPUT && viewing_file_) {
        if (!cached_viewing_file_ || cached_file_ver_ != file_version_ || cached_cols_ != cols) {
            cached_wrapped_ = wrap_lines(file_lines_, cols);
            cached_viewing_file_ = true;
            cached_file_ver_ = file_version_;
            cached_cols_ = cols;
        }
        return cached_wrapped_;
    }
    uint64_t ver = session_.version();
    if (cached_viewing_file_ || cached_session_ver_ != ver || cached_cols_ != cols) {
        cached_wrapped_ = wrap_lines(session_.lines_snapshot(), cols);
        cached_viewing_file_ = false;
        cached_session_ver_ = ver;
        cached_cols_ = cols;
    }
    return cached_wrapped_;
}

uint16_t AppUI::color_for_kind(LineKind kind) {
    switch (kind) {
        case LineKind::SUCCESS: return col::GREEN;
        case LineKind::WARN:    return col::AMBER;
        case LineKind::ERROR:   return col::RED;
        case LineKind::INFO:    return col::CYAN;
        case LineKind::HINT:    return col::AMBER;
        case LineKind::DEV:     return rgb565(168, 198, 224);
        case LineKind::CMD:     return col::ACCENT;
        case LineKind::NORMAL:  return col::TEXT;
    }
    return col::TEXT;
}

void AppUI::draw_scrollbar(int x, int y, int h, int total, int visible, int offset) {
    if (total <= visible || visible <= 0) return;
    fb_.fill_rect(x, y, 3, h, col::SURFACE_ALT);
    int thumb_h = std::max(10, (h * visible) / total);
    int max_off = std::max(1, total - visible);
    int thumb_y = y + ((h - thumb_h) * std::min(offset, max_off)) / max_off;
    fb_.fill_rect(x, thumb_y, 3, thumb_h, col::ACCENT);
}

void AppUI::draw_header() {
    fb_.fill_rect(0, 0, SCREEN_W, 22, col::HEADER);
    fb_.draw_hline(0, 22, SCREEN_W, col::BORDER);

    IconId hdr_icon = IconId::CHIP;
    std::string title = "CZPM3";
    if (screen_ == Screen::SUBMENU) {
        title = sub_title_;
        if (sub_title_.find("HF") != std::string::npos) hdr_icon = IconId::NFC;
        else if (sub_title_.find("LF") != std::string::npos) hdr_icon = IconId::RFID;
        else hdr_icon = IconId::CHIP;
    } else if (screen_ == Screen::OUTPUT) {
        title = viewing_file_ ? "FILE VIEW" : "OUTPUT";
        hdr_icon = viewing_file_ ? IconId::FILE_LOG : IconId::PLAY;
    } else if (screen_ == Screen::DUMPS) {
        title = "SAVED DUMPS";
        hdr_icon = IconId::FOLDER;
    } else if (screen_ == Screen::CLI) {
        title = "CLI CONSOLE";
        hdr_icon = IconId::TERMINAL;
    } else if (screen_ == Screen::PORTS) {
        title = "DEVICE / BT";
        hdr_icon = IconId::BLUETOOTH;
    } else {
        hdr_icon = IconId::RADAR;
    }

    std::string badge_txt;
    uint16_t badge_bg = col::AMBER_DIM;
    uint16_t badge_fg = col::AMBER;
    if (session_.is_offline()) {
        badge_txt = "OFFLINE";
    } else {
        std::string base = format_port_badge(session_.selected_port());
        if (session_.is_connected()) {
            badge_txt = "* " + base;
            badge_bg = col::GREEN_DIM;
            badge_fg = col::GREEN;
        } else if (session_.is_session_alive() && !session_.is_running()) {
            badge_txt = "! " + base;
            badge_bg = col::AMBER_DIM;
            badge_fg = col::AMBER;
        } else {
            badge_txt = base;
            badge_bg = col::ACCENT_DIM;
            badge_fg = col::ACCENT;
        }
    }

    int bw = fb_.text_width(badge_txt, 10, true) + 10;
    int bx = SCREEN_W - bw - 4;
    fb_.draw_badge(bx, 3, badge_txt, badge_bg, badge_fg, 10, true);

    int leftmost_x = bx;
    if (session_.is_running()) {
        static const char kSpin[] = {'|', '/', '-', '\\'};
        int frame = static_cast<int>((steady_ms() / 125) % 4);
        std::string run_txt = std::string("RUN ") + kSpin[frame];
        int rw = fb_.text_width(run_txt, 10, true) + 10;
        int rx = bx - rw - 4;
        fb_.draw_badge(rx, 3, run_txt, col::ACCENT_DIM, col::ACCENT, 10, true);
        leftmost_x = rx;
    }

    fb_.draw_icon(hdr_icon, 5, 4, col::ACCENT);
    int max_title_w = std::max(50, leftmost_x - 28);
    fb_.draw_text(23, 4, title, col::TEXT, 13, true, max_title_w);
}

void AppUI::draw_footer(int esc_hold_ms) {
    fb_.fill_rect(0, 152, SCREEN_W, 18, col::HEADER);
    fb_.draw_hline(0, 152, SCREEN_W, col::BORDER);

    if (esc_hold_ms >= 180) {
        int clamped = std::min(800, esc_hold_ms);
        int fill_w = std::max(10, (clamped * (SCREEN_W - 8)) / 800);
        fb_.fill_round_rect(4, 154, SCREEN_W - 8, 14, 3, col::RED_DIM);
        fb_.fill_round_rect(4, 154, fill_w, 14, 3, col::RED);
        fb_.draw_text(10, 155, "Hold Esc to exit...", col::TEXT, 10, true, SCREEN_W - 20);
        return;
    }

    auto now = std::chrono::steady_clock::now();
    if (!toast_msg_.empty() && now < toast_until_) {
        fb_.draw_text(6, 155, toast_msg_, col::GREEN, 10, true, SCREEN_W - 12);
        return;
    }

    std::string hint;
    switch (screen_) {
        case Screen::HOME:
            hint = "F/X:Nav Ent:Open Tab:CLI HoldEsc:Exit";
            break;
        case Screen::SUBMENU:
            hint = "F/X:Nav Ent:Run Tab:CLI Esc:Back";
            break;
        case Screen::OUTPUT:
            if (viewing_file_) {
                hint = "F/X:Scroll Z/C:Pg W:Font Esc:Back";
            } else if (session_.is_running()) {
                hint = "F/X:Scroll Ent/Esc:Stop ^C:Kill";
            } else {
                hint = "F/X:Scrl Z/C:Pg W:Font R:Run S:Save";
            }
            break;
        case Screen::DUMPS:
            hint = "F/X:Sel Ent:View R:Refr Esc:Back";
            break;
        case Screen::CLI:
            if (cli_scroll_mode_) {
                hint = "SCRL F/X Z/C W:Font S:Save Tab:In";
            } else if (session_.is_running()) {
                hint = "BUSY Ent/Esc:Stop ^C:Kill Tab:Scrl";
            } else {
                hint = "Ent:Run ^U:ClrIn ^L:Clr Tab:Scrl";
            }
            break;
        case Screen::PORTS:
            hint = custom_port_mode_
                ? "ble:/bt:/tcp: Ent:Apply Esc:Back"
                : "F/X:Sel Ent:Use/Pair P:Pair Esc:Bk";
            break;
    }
    fb_.draw_text(6, 155, hint, col::TEXT_DIM, 10, false, SCREEN_W - 12);
}

void AppUI::draw_home() {
    struct Entry {
        const char* label;
        IconId icon;
        uint16_t icon_col;
        std::string tag;
        uint16_t tag_col;
    };

    std::string cur = session_.selected_port();
    std::string port_tag = session_.is_offline() ? "OFF" : format_port_badge(cur);
    IconId dev_icon = IconId::BLUETOOTH;
    if (session_.is_offline()) dev_icon = IconId::OFFLINE;
    else if (cur.rfind("bt:", 0) == 0) dev_icon = IconId::BLUESHARK;
    else if (cur.rfind("ble:", 0) == 0) dev_icon = IconId::BLUETOOTH;
    else dev_icon = IconId::USB;

    Entry entries[7] = {
        {"1. Auto Scan",    IconId::RADAR,    col::GREEN,  "auto", col::GREEN},
        {"2. HF / NFC",     IconId::NFC,      col::ACCENT, std::to_string(hf_commands().size()) + ">", col::TEXT_DIM},
        {"3. LF / RFID",    IconId::RFID,     col::AMBER,  std::to_string(lf_commands().size()) + ">", col::TEXT_DIM},
        {"4. Hardware/BWM", IconId::CHIP,     col::CYAN,   std::to_string(hw_commands().size()) + ">", col::TEXT_DIM},
        {"5. Saved Dumps",  IconId::FOLDER,   col::AMBER,  "logs", col::TEXT_DIM},
        {"6. CLI Console",  IconId::TERMINAL, col::CYAN,   "pm3>", col::CYAN},
        {"7. Device / BT",  dev_icon,         session_.is_offline() ? col::AMBER : col::GREEN,
         port_tag, session_.is_offline() ? col::AMBER : (session_.is_connected() ? col::GREEN : col::CYAN)},
    };

    constexpr int kVis = 4;
    for (int row = 0; row < kVis; ++row) {
        int i = home_scroll_ + row;
        if (i >= 7) break;
        int y = 24 + row * 31;
        bool sel = (i == home_idx_);
        if (sel) {
            fb_.fill_round_rect(6, y, 304, 29, 5, col::SELECT_BG);
            fb_.draw_round_rect(6, y, 304, 29, 5, col::ACCENT);
            fb_.fill_rect(8, y + 5, 3, 19, col::ACCENT);
        } else {
            fb_.fill_round_rect(6, y, 304, 29, 5, col::SURFACE);
        }

        fb_.fill_round_rect(13, y + 4, 22, 21, 4, sel ? col::HEADER : col::SURFACE_ALT);
        fb_.draw_icon(entries[i].icon, 17, y + 7, entries[i].icon_col);

        int tw = fb_.text_width(entries[i].tag, 11, true);
        int tag_x = 303 - tw;
        fb_.draw_text(tag_x, y + 8, entries[i].tag, sel ? col::ACCENT : entries[i].tag_col, 11, true);
        fb_.draw_text(40, y + 6, entries[i].label, col::TEXT, 14, true, std::max(80, tag_x - 46));
    }
    draw_scrollbar(313, 24, 122, 7, kVis, home_scroll_);
}

void AppUI::draw_submenu() {
    constexpr int kVis = 3;
    int n = static_cast<int>(sub_items_.size());
    for (int row = 0; row < kVis; ++row) {
        int idx = sub_scroll_ + row;
        if (idx >= n) break;
        int y = 24 + row * 31;
        bool sel = (idx == sub_idx_);
        if (sel) {
            fb_.fill_round_rect(6, y, 304, 28, 5, col::SELECT_BG);
            fb_.draw_round_rect(6, y, 304, 28, 5, col::ACCENT);
            fb_.fill_rect(8, y + 5, 3, 18, col::ACCENT);
        } else {
            fb_.fill_round_rect(6, y, 304, 28, 5, col::SURFACE);
        }
        IconId ic = icon_for_command(sub_items_[idx].cmd);
        fb_.fill_round_rect(13, y + 4, 22, 20, 4, sel ? col::HEADER : col::SURFACE_ALT);
        fb_.draw_icon(ic, 17, y + 7, sel ? col::ACCENT : col::CYAN);
        fb_.draw_text(40, y + 6, sub_items_[idx].label, col::TEXT, 13, true, 264);
    }
    draw_scrollbar(313, 24, 90, n, kVis, sub_scroll_);

    // 2-line description footer card
    fb_.fill_round_rect(6, 118, 308, 31, 4, col::SURFACE_ALT);
    fb_.draw_round_rect(6, 118, 308, 31, 4, col::BORDER);
    if (sub_idx_ >= 0 && sub_idx_ < n) {
        const std::string& desc = sub_items_[sub_idx_].desc;
        int cw = std::max(6, fb_.char_width(11, false));
        size_t max_chars = static_cast<size_t>(std::max(24, 294 / cw));
        if (desc.size() <= max_chars) {
            fb_.draw_text(12, 127, desc, col::CYAN, 11, false, 296);
        } else {
            size_t brk = desc.rfind(' ', max_chars);
            if (brk == std::string::npos || brk < max_chars / 2) brk = max_chars;
            std::string l1 = desc.substr(0, brk);
            std::string l2 = (brk < desc.size() && desc[brk] == ' ') ? desc.substr(brk + 1) : desc.substr(brk);
            fb_.draw_text(12, 120, l1, col::CYAN, 11, false, 296);
            fb_.draw_text(12, 134, l2, col::CYAN, 11, false, 296);
        }
    }
}

void AppUI::draw_output() {
    fb_.fill_round_rect(6, 24, 308, 19, 4, col::SURFACE_ALT);
    std::string cmd_str = viewing_file_ ? view_title_ : ("$ " + session_.active_command());

    std::string status = "DONE";
    uint16_t sbg = col::GREEN_DIM;
    uint16_t sfg = col::GREEN;
    if (viewing_file_) {
        status = "FILE";
        sbg = col::ACCENT_DIM;
        sfg = col::ACCENT;
    } else if (session_.is_running()) {
        status = "BUSY";
        sbg = col::AMBER_DIM;
        sfg = col::AMBER;
    } else {
        int rc = session_.last_exit_code();
        if (rc != 0) {
            status = "RC:" + std::to_string(rc);
            sbg = col::RED_DIM;
            sfg = col::RED;
        } else {
            switch (session_.last_cmd_status()) {
                case CmdStatus::ERR:
                    status = "ERR";
                    sbg = col::RED_DIM;
                    sfg = col::RED;
                    break;
                case CmdStatus::WARN:
                    status = "WARN";
                    sbg = col::AMBER_DIM;
                    sfg = col::AMBER;
                    break;
                case CmdStatus::OK:
                case CmdStatus::NONE:
                    status = "DONE";
                    sbg = col::GREEN_DIM;
                    sfg = col::GREEN;
                    break;
            }
        }
    }

    int sbw = fb_.text_width(status, 10, true) + 10;
    int sbx = 310 - sbw;
    fb_.draw_badge(sbx, 26, status, sbg, sfg, 10, true);
    fb_.draw_text(11, 27, cmd_str, col::ACCENT, 11, true, std::max(80, sbx - 16));

    const auto& wrapped = get_wrapped_lines();

    int px = compact_text_ ? 9 : 11;
    int pitch = compact_text_ ? 11 : 13;
    int vis = output_vis_lines();
    int total = static_cast<int>(wrapped.size());
    int max_off = std::max(0, total - vis);
    if (out_follow_) {
        out_scroll_ = max_off;
    } else {
        out_scroll_ = std::min(out_scroll_, max_off);
    }

    for (int i = 0; i < vis; ++i) {
        int idx = out_scroll_ + i;
        if (idx >= total) break;
        int y = 46 + i * pitch;
        fb_.draw_text(6, y, wrapped[idx].text, color_for_kind(wrapped[idx].kind), px, false, 304);
    }
    draw_scrollbar(313, 46, 102, total, vis, out_scroll_);
}

void AppUI::draw_dumps() {
    if (dumps_.empty()) {
        fb_.fill_round_rect(12, 46, 296, 66, 6, col::SURFACE);
        fb_.draw_round_rect(12, 46, 296, 66, 6, col::BORDER);
        fb_.draw_text(22, 58, "No saved dumps or logs found.", col::TEXT, 12, true);
        fb_.draw_text(22, 82, "Press 'S' on Output screen to save.", col::TEXT_DIM, 11, false);
        return;
    }

    constexpr int kVis = 4;
    int n = static_cast<int>(dumps_.size());
    for (int row = 0; row < kVis; ++row) {
        int idx = dumps_scroll_ + row;
        if (idx >= n) break;
        int y = 24 + row * 31;
        bool sel = (idx == dumps_idx_);
        if (sel) {
            fb_.fill_round_rect(6, y, 304, 29, 5, col::SELECT_BG);
            fb_.draw_round_rect(6, y, 304, 29, 5, col::ACCENT);
        } else {
            fb_.fill_round_rect(6, y, 304, 29, 5, col::SURFACE);
        }
        const std::string& nm = dumps_[idx].name;
        bool is_bin = (nm.size() >= 4 &&
                       (nm.compare(nm.size() - 4, 4, ".bin") == 0 ||
                        nm.compare(nm.size() - 4, 4, ".mfd") == 0 ||
                        nm.compare(nm.size() - 5, 5, ".dump") == 0));
        fb_.fill_round_rect(12, y + 4, 22, 21, 4, sel ? col::HEADER : col::SURFACE_ALT);
        fb_.draw_icon(is_bin ? IconId::FILE_BIN : IconId::FILE_LOG, 16, y + 7, is_bin ? col::AMBER : col::CYAN);

        std::string sz = format_size(dumps_[idx].size_bytes);
        int sw = fb_.text_width(sz, 10, false);
        int sz_x = 304 - sw;
        fb_.draw_text(sz_x, y + 9, sz, col::TEXT_DIM, 10, false);
        fb_.draw_text(39, y + 7, nm, col::TEXT, 12, true, std::max(80, sz_x - 45));
    }
    draw_scrollbar(313, 24, 122, n, kVis, dumps_scroll_);
}

void AppUI::draw_ports() {
    int serial_cnt = static_cast<int>(ports_.size());
    int ble_cnt = static_cast<int>(ble_devices_.size());
    int extra = has_custom_active_port_row() ? 1 : 0;
    int total = kPortsStaticRows + serial_cnt + ble_cnt + extra;
    int vis = custom_port_mode_ ? 3 : 4;
    if (ports_scroll_ > std::max(0, total - vis)) {
        ports_scroll_ = std::max(0, total - vis);
    }

    std::string cur_port = session_.selected_port();
    bool is_off = session_.is_offline();

    auto draw_row = [&](int row_y, bool sel, IconId icon, uint16_t icon_col,
                        const std::string& label, uint16_t label_col,
                        const std::string& badge, uint16_t bbg, uint16_t bfg) {
        if (sel) {
            fb_.fill_round_rect(6, row_y, 304, 29, 5, col::SELECT_BG);
            fb_.draw_round_rect(6, row_y, 304, 29, 5, col::ACCENT);
        } else {
            fb_.fill_round_rect(6, row_y, 304, 29, 5, col::SURFACE);
        }
        fb_.fill_round_rect(11, row_y + 4, 22, 21, 4, sel ? col::HEADER : col::SURFACE_ALT);
        fb_.draw_icon(icon, 15, row_y + 7, icon_col);

        if (!badge.empty()) {
            int bw = fb_.text_width(badge, 10, true) + 10;
            int bx = 304 - bw;
            fb_.draw_badge(bx, row_y + 6, badge, bbg, bfg, 10, true);
            fb_.draw_text(38, row_y + 7, label, label_col, 12, true, std::max(70, bx - 43));
        } else {
            fb_.draw_text(38, row_y + 7, label, label_col, 12, true, 266);
        }
    };

    for (int row = 0; row < vis; ++row) {
        int idx = ports_scroll_ + row;
        if (idx >= total) break;
        int y = 24 + row * 31;
        bool sel = (!custom_port_mode_ && idx == ports_idx_);

        if (idx == 0) {
            bool scanning = session_.is_ble_scanning();
            draw_row(y, sel, IconId::SCAN, col::CYAN,
                     "Scan BT & BLE (12s)", col::CYAN,
                     scanning ? "SCAN" : "", col::AMBER_DIM, col::AMBER);
        } else if (idx == 1) {
            draw_row(y, sel, IconId::WIFI, wifi_on_ ? col::AMBER : col::GREEN,
                     "Wi-Fi Radio (Off=BT)", col::TEXT,
                     wifi_on_ ? "ON" : "OFF/BT",
                     wifi_on_ ? col::AMBER_DIM : col::GREEN_DIM,
                     wifi_on_ ? col::AMBER : col::GREEN);
        } else if (idx == 2) {
            draw_row(y, sel, IconId::USB, col::CYAN,
                     "Rescan USB Serial", col::CYAN, "", 0, 0);
        } else if (idx == 3) {
            std::string badge = is_off ? "OFFLINE" : (session_.is_connected() ? "OK" : "PING");
            uint16_t bbg = is_off ? col::SURFACE_ALT : (session_.is_connected() ? col::GREEN_DIM : col::ACCENT_DIM);
            uint16_t bfg = is_off ? col::TEXT_DIM : (session_.is_connected() ? col::GREEN : col::ACCENT);
            draw_row(y, sel, IconId::PING, col::AMBER,
                     "Test / Ping Target", col::CYAN, badge, bbg, bfg);
        } else if (idx == 4) {
            draw_row(y, sel, IconId::TERMINAL, col::ACCENT,
                     "Custom ble:/bt:/tcp:", col::ACCENT, "", 0, 0);
        } else if (idx == 5) {
            draw_row(y, sel, IconId::OFFLINE, col::AMBER,
                     "Offline Mode", col::TEXT,
                     is_off ? "ACTIVE" : "", col::AMBER_DIM, col::AMBER);
        } else if (idx < kPortsStaticRows + serial_cnt) {
            const auto& p = ports_[idx - kPortsStaticRows];
            bool active = (!is_off && cur_port == p.path);
            if (active) {
                draw_row(y, sel, IconId::USB, col::GREEN, p.label, col::TEXT, "ACTIVE", col::GREEN_DIM, col::GREEN);
            } else if (p.is_proxmark) {
                draw_row(y, sel, IconId::USB, col::ACCENT, p.label, col::TEXT, "PM3", col::ACCENT_DIM, col::ACCENT);
            } else {
                draw_row(y, sel, IconId::USB, col::TEXT_DIM, p.label, col::TEXT, "", 0, 0);
            }
        } else if (idx < kPortsStaticRows + serial_cnt + ble_cnt) {
            const auto& b = ble_devices_[idx - kPortsStaticRows - serial_cnt];
            bool pairing_this = (session_.is_bt_pairing() && session_.bt_pair_target() == b.mac);
            bool active = (!is_off && (cur_port == "bt:" + b.mac || cur_port == "ble:" + b.mac || cur_port == "ble:" + b.name));
            IconId ic = b.is_classic_bt ? IconId::BLUESHARK : IconId::BLUETOOTH;
            uint16_t ic_col = pairing_this ? col::AMBER : (active ? col::GREEN : (b.is_classic_bt ? col::ACCENT : col::CYAN));

            std::string badge;
            uint16_t bbg = col::SURFACE_ALT;
            uint16_t bfg = col::TEXT_DIM;
            if (pairing_this) {
                badge = "PAIR..";
                bbg = col::AMBER_DIM;
                bfg = col::AMBER;
            } else if (active) {
                badge = "ACTIVE";
                bbg = col::GREEN_DIM;
                bfg = col::GREEN;
            } else if (b.is_classic_bt && !b.is_paired) {
                badge = "PAIR BT";
                bbg = col::AMBER_DIM;
                bfg = col::AMBER;
            } else if (b.is_classic_bt) {
                badge = b.is_proxmark ? "RDV4 BT" : "BT";
                bbg = col::ACCENT_DIM;
                bfg = col::ACCENT;
            } else {
                badge = b.is_proxmark ? "PM5 BLE" : "BLE";
                bbg = b.is_proxmark ? col::ACCENT_DIM : col::SURFACE_ALT;
                bfg = b.is_proxmark ? col::ACCENT : col::TEXT_DIM;
            }
            draw_row(y, sel, ic, ic_col, b.name, col::TEXT, badge, bbg, bfg);
        } else {
            IconId ic = (cur_port.rfind("bt:", 0) == 0) ? IconId::BLUESHARK
                      : ((cur_port.rfind("ble:", 0) == 0) ? IconId::BLUETOOTH : IconId::TERMINAL);
            draw_row(y, sel, ic, col::GREEN, cur_port, col::TEXT, "ACTIVE", col::GREEN_DIM, col::GREEN);
        }
    }
    draw_scrollbar(313, 24, custom_port_mode_ ? 91 : 122, total, vis, ports_scroll_);

    if (custom_port_mode_) {
        fb_.fill_round_rect(6, 123, 308, 26, 4, col::SURFACE_ALT);
        fb_.draw_round_rect(6, 123, 308, 26, 4, col::ACCENT);
        std::string shown = custom_port_input_;
        if (shown.size() > 30) shown = ".." + shown.substr(shown.size() - 28);
        std::string line = "port> " + shown;
        if ((steady_ms() / 500) % 2 == 0) line.push_back('_');
        fb_.draw_text(11, 129, line, col::TEXT, 12, true, 298);
    }
}

void AppUI::draw_cli() {
    static const std::vector<OutputLine> kEmptyPlaceholder = {
        {"CZPM3 interactive console ready.", LineKind::INFO},
        {"Type a command (e.g. 'hw ping', 'hf search')", LineKind::NORMAL},
    };
    const auto& cached = get_wrapped_lines();
    const auto& wrapped = cached.empty() ? kEmptyPlaceholder : cached;

    int px = compact_text_ ? 9 : 11;
    int pitch = compact_text_ ? 11 : 13;
    int vis = cli_vis_lines();
    int total = static_cast<int>(wrapped.size());
    int max_off = std::max(0, total - vis);
    if (cli_follow_) {
        cli_scroll_ = max_off;
    } else {
        cli_scroll_ = std::min(cli_scroll_, max_off);
    }

    for (int i = 0; i < vis; ++i) {
        int idx = cli_scroll_ + i;
        if (idx >= total) break;
        int y = 24 + i * pitch;
        fb_.draw_text(6, y, wrapped[idx].text, color_for_kind(wrapped[idx].kind), px, false, 304);
    }
    draw_scrollbar(313, 24, 94, total, vis, cli_scroll_);

    uint16_t border = cli_scroll_mode_ ? col::BORDER : col::ACCENT;
    fb_.fill_round_rect(6, 123, 308, 26, 4, col::SURFACE_ALT);
    fb_.draw_round_rect(6, 123, 308, 26, 4, border);

    int max_prompt_w = 298;
    if (cli_scroll_mode_) {
        int bw = fb_.text_width("SCROLL", 10, true) + 10;
        int bx = 309 - bw;
        fb_.draw_badge(bx, 128, "SCROLL", col::ACCENT_DIM, col::ACCENT, 10, true);
        max_prompt_w = std::max(80, bx - 16);
    }

    std::string shown = cli_input_;
    if (shown.size() > 30) {
        shown = ".." + shown.substr(shown.size() - 28);
    }
    std::string prompt_line = "pm3> " + shown;
    if (!cli_scroll_mode_ && ((steady_ms() / 500) % 2 == 0)) {
        prompt_line.push_back('_');
    }
    fb_.draw_text(11, 129, prompt_line, cli_scroll_mode_ ? col::TEXT_DIM : col::TEXT, 12, true, max_prompt_w);
}

void AppUI::render(int esc_hold_ms) {
    fb_.clear(col::BG);
    draw_header();

    switch (screen_) {
        case Screen::HOME:    draw_home(); break;
        case Screen::SUBMENU: draw_submenu(); break;
        case Screen::OUTPUT:  draw_output(); break;
        case Screen::DUMPS:   draw_dumps(); break;
        case Screen::CLI:     draw_cli(); break;
        case Screen::PORTS:   draw_ports(); break;
    }

    draw_footer(esc_hold_ms);
    fb_.present();

    int64_t now_ms = steady_ms();
    dirty_ = false;
    last_rendered_ver_ = session_.version();
    last_spin_frame_ = session_.is_running() ? static_cast<int>((now_ms / 125) % 4) : -1;
    bool cursor_active = (screen_ == Screen::CLI && !cli_scroll_mode_) ||
                         (screen_ == Screen::PORTS && custom_port_mode_);
    last_cursor_on_ = cursor_active ? ((now_ms / 500) % 2 == 0) : false;
    last_toast_visible_ = !toast_msg_.empty() && (std::chrono::steady_clock::now() < toast_until_);
    last_esc_bucket_ = (esc_hold_ms >= 180) ? ((esc_hold_ms / 40) + 1) : 0;
}

}  // namespace cpz
