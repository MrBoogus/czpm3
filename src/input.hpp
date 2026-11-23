#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace cpz {

enum class KeyAction {
    NONE,
    UP,
    DOWN,
    LEFT,
    RIGHT,
    ENTER,
    BACK,       // Short Esc press
    EXIT_HOLD,  // Long Esc hold (>= 800ms)
    TAB,
    CHAR,
    BACKSPACE,
    DELETE_KEY,
    PGUP,
    PGDN,
    HOME,
    END,
    CTRL_C,
    CTRL_L,
    CTRL_U,
};

struct KeyEvent {
    KeyAction action = KeyAction::NONE;
    char ch = 0;
    bool shift = false;
    bool ctrl = false;
};

class Input {
public:
    Input() = default;
    ~Input();

    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;

    bool init(bool headless = false);
    void shutdown();

    // Poll evdev devices and update long-press timer; returns queued events.
    void poll(std::vector<KeyEvent>& out);

    const std::string& device_path() const { return dev_path_; }
    const std::vector<int>& fds() const { return fds_; }
    bool is_esc_held() const { return esc_held_ && !esc_exit_emitted_; }
    int esc_hold_ms() const;

private:
    void load_keymap(const char* path);
    char lookup_cardputer_keymap(uint16_t code) const;
    char translate_char(uint16_t code, bool shifted) const;
    void handle_ev_key(uint16_t code, int32_t value, bool is_cardputer);

    std::vector<int> fds_;
    std::vector<bool> grabbed_;
    std::vector<bool> is_cardputer_;
    std::string dev_path_;
    std::vector<std::pair<uint16_t, char>> cardputer_map_;
    std::deque<KeyEvent> queue_;

    bool lshift_ = false;
    bool rshift_ = false;
    bool lctrl_ = false;
    bool rctrl_ = false;

    bool esc_held_ = false;
    bool esc_exit_emitted_ = false;
    std::chrono::steady_clock::time_point esc_press_time_{};
};

}  // namespace cpz
