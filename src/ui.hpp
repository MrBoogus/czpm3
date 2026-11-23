#pragma once

#include "fb.hpp"
#include "input.hpp"
#include "pm3_session.hpp"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace cpz {

enum class Screen {
    HOME,
    SUBMENU,
    OUTPUT,
    DUMPS,
    CLI,
    PORTS,
};

struct CommandItem {
    std::string label;
    std::string cmd;
    std::string desc;
};

struct DumpFileEntry {
    std::string name;
    std::string full_path;
    size_t size_bytes = 0;
    std::time_t mtime = 0;
};

class AppUI {
public:
    AppUI(Framebuffer& fb, Pm3Session& session);

    void handle_events(const std::vector<KeyEvent>& events);
    bool needs_render(int esc_hold_ms = 0) const;
    int recommended_poll_ms(bool esc_held) const;
    void render(int esc_hold_ms = 0);

    bool should_exit() const { return should_exit_; }

    // Helpers for headless screenshot generation
    void set_screen_for_demo(Screen s);
    void set_submenu_for_demo(const std::string& title, int category_idx);
    void set_cli_scroll_for_demo(bool scroll_mode);
    void set_ports_demo_focus(int idx);

private:
    void open_home();
    void open_submenu(int category_idx);
    void launch_command(const std::string& cmd, Screen return_to);
    void open_dumps();
    void open_dump_file(const DumpFileEntry& entry);
    void open_ports();
    void open_cli();

    void clamp_ports_selection();
    bool has_custom_active_port_row() const;
    int output_wrap_cols() const;
    int output_vis_lines() const;
    int cli_vis_lines() const;

    void handle_home(const KeyEvent& ev);
    void handle_submenu(const KeyEvent& ev);
    void handle_output(const KeyEvent& ev);
    void handle_dumps(const KeyEvent& ev);
    void handle_cli(const KeyEvent& ev);
    void handle_ports(const KeyEvent& ev);

    void draw_header();
    void draw_footer(int esc_hold_ms);
    void draw_home();
    void draw_submenu();
    void draw_output();
    void draw_dumps();
    void draw_cli();
    void draw_ports();
    void draw_scrollbar(int x, int y, int h, int total, int visible, int offset);
    void show_toast(const std::string& msg);

    const std::vector<OutputLine>& get_wrapped_lines() const;
    std::vector<OutputLine> wrap_lines(const std::vector<OutputLine>& src, int max_cols) const;
    static IconId icon_for_command(const std::string& cmd);
    static uint16_t color_for_kind(LineKind kind);
    static bool is_nav_up(const KeyEvent& ev);
    static bool is_nav_down(const KeyEvent& ev);
    static bool is_nav_left(const KeyEvent& ev);
    static bool is_nav_right(const KeyEvent& ev);
    static bool is_scroll_up(const KeyEvent& ev);
    static bool is_scroll_down(const KeyEvent& ev);
    static bool is_scroll_pgup(const KeyEvent& ev);
    static bool is_scroll_pgdn(const KeyEvent& ev);

    Framebuffer& fb_;
    Pm3Session& session_;

    Screen screen_ = Screen::HOME;
    Screen return_screen_ = Screen::HOME;
    bool should_exit_ = false;
    bool dirty_ = true;

    int home_idx_ = 0;
    int home_scroll_ = 0;

    std::string sub_title_;
    std::vector<CommandItem> sub_items_;
    int sub_idx_ = 0;
    int sub_scroll_ = 0;

    // Output / file viewer state
    bool viewing_file_ = false;
    bool compact_text_ = false;
    std::string view_title_;
    std::vector<OutputLine> file_lines_;
    uint64_t file_version_ = 0;
    int out_scroll_ = 0;
    bool out_follow_ = true;
    bool stop_requested_ = false;
    bool was_running_ = false;

    // Wrapped output cache
    mutable uint64_t cached_session_ver_ = 0;
    mutable uint64_t cached_file_ver_ = 0;
    mutable bool cached_viewing_file_ = false;
    mutable int cached_cols_ = 0;
    mutable std::vector<OutputLine> cached_wrapped_;

    // Dumps browser
    std::vector<DumpFileEntry> dumps_;
    int dumps_idx_ = 0;
    int dumps_scroll_ = 0;

    // Ports / BT / BLE selector
    std::vector<SerialPortInfo> ports_;
    std::vector<BleDeviceInfo> ble_devices_;
    int ports_idx_ = 0;
    int ports_scroll_ = 0;
    bool was_ble_scanning_ = false;
    bool was_bt_pairing_ = false;
    bool was_wifi_busy_ = false;
    bool wifi_on_ = true;
    bool custom_port_mode_ = false;
    std::string custom_port_input_;

    // CLI console
    std::string cli_input_;
    std::string cli_pending_write_cmd_;
    std::vector<std::string> cli_history_;
    int cli_hist_idx_ = -1;
    bool cli_scroll_mode_ = false;
    int cli_scroll_ = 0;
    bool cli_follow_ = true;

    // Toast & render-state tracking
    std::string toast_msg_;
    std::chrono::steady_clock::time_point toast_until_{};
    std::chrono::steady_clock::time_point last_port_scan_{};
    uint64_t last_rendered_ver_ = 0;
    int last_spin_frame_ = -1;
    bool last_cursor_on_ = false;
    bool last_toast_visible_ = false;
    int last_esc_bucket_ = 0;
};

}  // namespace cpz
