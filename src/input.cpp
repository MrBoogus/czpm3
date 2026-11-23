#include "input.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <linux/input.h>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cpz {

namespace {

constexpr int kEscHoldExitMs = 800;

constexpr std::pair<uint16_t, char> kBuiltInCardputerMap[] = {
    {26, '!'}, {27, '@'}, {39, '#'},  {40, '$'}, {41, '%'}, {43, '^'}, {51, '&'},  {52, '*'},
    {53, '('}, {94, ')'}, {55, '~'},  {69, '`'}, {70, '_'}, {71, '-'}, {72, '+'},  {73, '='},
    {74, '['}, {75, ']'}, {76, '{'},  {77, '}'}, {79, ';'}, {80, ':'}, {81, '\''}, {82, '"'},
    {83, '<'}, {85, '>'}, {86, '\\'}, {89, '|'}, {90, ','}, {91, '.'}, {92, '/'},  {93, '?'},
};

char symbol_to_char(const std::string& sym) {
    static const std::pair<const char*, char> kSyms[] = {
        {"exclam", '!'},      {"at", '@'},          {"numbersign", '#'},   {"dollar", '$'},
        {"percent", '%'},     {"asciicircum", '^'}, {"ampersand", '&'},    {"asterisk", '*'},
        {"parenleft", '('},   {"parenright", ')'},  {"asciitilde", '~'},   {"grave", '`'},
        {"underscore", '_'},  {"minus", '-'},       {"plus", '+'},         {"equal", '='},
        {"bracketleft", '['}, {"bracketright", ']'},{"braceleft", '{'},    {"braceright", '}'},
        {"semicolon", ';'},   {"colon", ':'},       {"apostrophe", '\''},  {"quotedbl", '"'},
        {"less", '<'},        {"greater", '>'},     {"backslash", '\\'},   {"bar", '|'},
        {"comma", ','},       {"period", '.'},      {"slash", '/'},        {"question", '?'},
    };
    for (const auto& kv : kSyms) {
        if (sym == kv.first) return kv.second;
    }
    return 0;
}

bool has_usable_keys(int fd) {
    unsigned long bits[(KEY_MAX / (sizeof(unsigned long) * 8)) + 1]{};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0) {
        return false;
    }
    auto has_key = [&](int code) {
        const size_t bpw = sizeof(unsigned long) * 8;
        return (bits[code / bpw] & (1UL << (code % bpw))) != 0;
    };
    return has_key(KEY_ENTER) || has_key(KEY_ESC) || has_key(KEY_A);
}

}  // namespace

Input::~Input() {
    shutdown();
}

void Input::load_keymap(const char* path) {
    cardputer_map_.assign(std::begin(kBuiltInCardputerMap), std::end(kBuiltInCardputerMap));
    std::ifstream ifs(path);
    if (!ifs) return;

    std::vector<std::pair<uint16_t, char>> loaded;
    std::string line;
    while (std::getline(ifs, line)) {
        auto hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        std::istringstream iss(line);
        std::string kw, eq, sym;
        unsigned int kc = 0;
        if ((iss >> kw >> kc >> eq >> sym) && kw == "keycode" && eq == "=" && kc <= 0xFFFF) {
            if (char ch = symbol_to_char(sym)) {
                loaded.emplace_back(static_cast<uint16_t>(kc), ch);
            }
        }
    }
    if (!loaded.empty()) {
        cardputer_map_ = std::move(loaded);
    }
}

char Input::lookup_cardputer_keymap(uint16_t code) const {
    for (const auto& kv : cardputer_map_) {
        if (kv.first == code) return kv.second;
    }
    return 0;
}

bool Input::init(bool headless) {
    load_keymap("/usr/share/keymaps/tca8418_keypad_m5stack_keymap.map");
    if (headless) {
        dev_path_ = "headless";
        return true;
    }

    std::vector<dev_t> seen_rdevs;
    auto try_open = [&](const std::string& path, bool check_keys, bool is_cpz) -> bool {
        int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) return false;
        struct stat st {};
        if (::fstat(fd, &st) == 0) {
            for (dev_t seen : seen_rdevs) {
                if (seen == st.st_rdev) {
                    ::close(fd);
                    return false;
                }
            }
        }
        if (check_keys && !has_usable_keys(fd)) {
            ::close(fd);
            return false;
        }
        bool is_grabbed = (ioctl(fd, EVIOCGRAB, 1) == 0);
        fds_.push_back(fd);
        grabbed_.push_back(is_grabbed);
        is_cardputer_.push_back(is_cpz);
        if (st.st_rdev != 0) seen_rdevs.push_back(st.st_rdev);
        if (dev_path_.empty()) dev_path_ = path;
        return true;
    };

    const char* env_dev = std::getenv("CPZ_KEYBOARD_DEVICE");
    if (env_dev && *env_dev && try_open(env_dev, false, true)) {
        return true;
    }

    // Prefer CardputerZero's tca8418c keypad by-path
    try_open("/dev/input/by-path/platform-3f804000.i2c-event", false, true);

    // Also scan /dev/input/event* for tca8418c or attached USB keyboards
    for (int i = 0; i < 16; ++i) {
        std::string path = "/dev/input/event" + std::to_string(i);
        int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        struct stat st {};
        bool dup = false;
        if (::fstat(fd, &st) == 0) {
            for (dev_t seen : seen_rdevs) {
                if (seen == st.st_rdev) {
                    dup = true;
                    break;
                }
            }
        }
        if (dup) {
            ::close(fd);
            continue;
        }
        char name[256]{};
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        std::string sname(name);
        if (sname.find("tca8418") != std::string::npos) {
            bool is_grabbed = (ioctl(fd, EVIOCGRAB, 1) == 0);
            fds_.push_back(fd);
            grabbed_.push_back(is_grabbed);
            is_cardputer_.push_back(true);
            if (st.st_rdev != 0) seen_rdevs.push_back(st.st_rdev);
            if (dev_path_.empty()) dev_path_ = path;
            continue;
        } else if (has_usable_keys(fd) &&
                   sname.find("hdmi") == std::string::npos &&
                   sname.find("vc4") == std::string::npos &&
                   sname.find("pwr") == std::string::npos &&
                   sname.find("button") == std::string::npos) {
            bool is_grabbed = (ioctl(fd, EVIOCGRAB, 1) == 0);
            fds_.push_back(fd);
            grabbed_.push_back(is_grabbed);
            is_cardputer_.push_back(false);
            if (st.st_rdev != 0) seen_rdevs.push_back(st.st_rdev);
            if (dev_path_.empty()) dev_path_ = path;
            continue;
        }
        ::close(fd);
    }

    return !fds_.empty();
}

void Input::shutdown() {
    for (size_t i = 0; i < fds_.size(); ++i) {
        if (fds_[i] >= 0) {
            if (i < grabbed_.size() && grabbed_[i]) {
                ioctl(fds_[i], EVIOCGRAB, 0);
            }
            ::close(fds_[i]);
        }
    }
    fds_.clear();
    grabbed_.clear();
    is_cardputer_.clear();
}

char Input::translate_char(uint16_t code, bool shifted) const {
    switch (code) {
        case KEY_A: return shifted ? 'A' : 'a';
        case KEY_B: return shifted ? 'B' : 'b';
        case KEY_C: return shifted ? 'C' : 'c';
        case KEY_D: return shifted ? 'D' : 'd';
        case KEY_E: return shifted ? 'E' : 'e';
        case KEY_F: return shifted ? 'F' : 'f';
        case KEY_G: return shifted ? 'G' : 'g';
        case KEY_H: return shifted ? 'H' : 'h';
        case KEY_I: return shifted ? 'I' : 'i';
        case KEY_J: return shifted ? 'J' : 'j';
        case KEY_K: return shifted ? 'K' : 'k';
        case KEY_L: return shifted ? 'L' : 'l';
        case KEY_M: return shifted ? 'M' : 'm';
        case KEY_N: return shifted ? 'N' : 'n';
        case KEY_O: return shifted ? 'O' : 'o';
        case KEY_P: return shifted ? 'P' : 'p';
        case KEY_Q: return shifted ? 'Q' : 'q';
        case KEY_R: return shifted ? 'R' : 'r';
        case KEY_S: return shifted ? 'S' : 's';
        case KEY_T: return shifted ? 'T' : 't';
        case KEY_U: return shifted ? 'U' : 'u';
        case KEY_V: return shifted ? 'V' : 'v';
        case KEY_W: return shifted ? 'W' : 'w';
        case KEY_X: return shifted ? 'X' : 'x';
        case KEY_Y: return shifted ? 'Y' : 'y';
        case KEY_Z: return shifted ? 'Z' : 'z';
        case KEY_0: case KEY_KP0: return shifted ? ')' : '0';
        case KEY_1: case KEY_KP1: return shifted ? '!' : '1';
        case KEY_2: case KEY_KP2: return shifted ? '@' : '2';
        case KEY_3: case KEY_KP3: return shifted ? '#' : '3';
        case KEY_4: case KEY_KP4: return shifted ? '$' : '4';
        case KEY_5: case KEY_KP5: return shifted ? '%' : '5';
        case KEY_6: case KEY_KP6: return shifted ? '^' : '6';
        case KEY_7: case KEY_KP7: return shifted ? '&' : '7';
        case KEY_8: case KEY_KP8: return shifted ? '*' : '8';
        case KEY_9: case KEY_KP9: return shifted ? '(' : '9';
        case KEY_SPACE: return ' ';
        case KEY_MINUS: return shifted ? '_' : '-';
        case KEY_EQUAL: return shifted ? '+' : '=';
        case KEY_LEFTBRACE: return shifted ? '{' : '[';
        case KEY_RIGHTBRACE: return shifted ? '}' : ']';
        case KEY_BACKSLASH: return shifted ? '|' : '\\';
        case KEY_SEMICOLON: return shifted ? ':' : ';';
        case KEY_APOSTROPHE: return shifted ? '"' : '\'';
        case KEY_GRAVE: return shifted ? '~' : '`';
        case KEY_COMMA: return shifted ? '<' : ',';
        case KEY_DOT: return shifted ? '>' : '.';
        case KEY_SLASH: return shifted ? '?' : '/';
        case KEY_KPMINUS: return '-';
        case KEY_KPPLUS: return '+';
        case KEY_KPASTERISK: return '*';
        case KEY_KPSLASH: return '/';
        case KEY_KPDOT: return '.';
        default: return 0;
    }
}

void Input::handle_ev_key(uint16_t code, int32_t value, bool is_cardputer) {
    if (code == KEY_LEFTSHIFT) {
        lshift_ = (value != 0);
        return;
    }
    if (code == KEY_RIGHTSHIFT) {
        rshift_ = (value != 0);
        return;
    }
    if (code == KEY_LEFTCTRL) {
        lctrl_ = (value != 0);
        return;
    }
    if (code == KEY_RIGHTCTRL) {
        rctrl_ = (value != 0);
        return;
    }

    if (code == KEY_ESC) {
        if (value == 1) {
            esc_held_ = true;
            esc_exit_emitted_ = false;
            esc_press_time_ = std::chrono::steady_clock::now();
        } else if (value == 0) {
            if (esc_held_ && !esc_exit_emitted_) {
                KeyEvent ev;
                ev.action = KeyAction::BACK;
                queue_.push_back(ev);
            }
            esc_held_ = false;
            esc_exit_emitted_ = false;
        }
        return;
    }

    if (value != 1 && value != 2) {
        return;  // only press (1) and repeat (2)
    }

    const bool shifted = lshift_ || rshift_;
    const bool ctrled = lctrl_ || rctrl_;

    // Ctrl+C abort, Ctrl+L clear output, Ctrl+U clear input
    if (ctrled && value == 1) {
        if (code == KEY_C) {
            KeyEvent ev;
            ev.action = KeyAction::CTRL_C;
            ev.ctrl = true;
            queue_.push_back(ev);
            return;
        }
        if (code == KEY_L) {
            KeyEvent ev;
            ev.action = KeyAction::CTRL_L;
            ev.ctrl = true;
            queue_.push_back(ev);
            return;
        }
        if (code == KEY_U) {
            KeyEvent ev;
            ev.action = KeyAction::CTRL_U;
            ev.ctrl = true;
            queue_.push_back(ev);
            return;
        }
    }

    // Prioritize CardputerZero tca8418c symbol map only for the onboard keypad
    if (is_cardputer) {
        if (char mapped = lookup_cardputer_keymap(code)) {
            KeyEvent ev;
            ev.action = KeyAction::CHAR;
            ev.ch = mapped;
            ev.shift = shifted;
            queue_.push_back(ev);
            return;
        }
    }

    KeyEvent ev;
    ev.shift = shifted;
    ev.ctrl = ctrled;

    switch (code) {
        case KEY_UP:        ev.action = KeyAction::UP; break;
        case KEY_DOWN:      ev.action = KeyAction::DOWN; break;
        case KEY_LEFT:      ev.action = KeyAction::LEFT; break;
        case KEY_RIGHT:     ev.action = KeyAction::RIGHT; break;
        case KEY_ENTER:
        case KEY_KPENTER:   if (value == 1) ev.action = KeyAction::ENTER; break;
        case KEY_TAB:       if (value == 1) ev.action = KeyAction::TAB; break;
        case KEY_BACKSPACE: ev.action = KeyAction::BACKSPACE; break;
        case KEY_DELETE:    ev.action = KeyAction::DELETE_KEY; break;
        case KEY_PAGEUP:    ev.action = KeyAction::PGUP; break;
        case KEY_PAGEDOWN:  ev.action = KeyAction::PGDN; break;
        case KEY_HOME:      ev.action = KeyAction::HOME; break;
        case KEY_END:       ev.action = KeyAction::END; break;
        default: {
            char ch = translate_char(code, shifted);
            if (ch != 0) {
                ev.action = KeyAction::CHAR;
                ev.ch = ch;
            }
            break;
        }
    }

    if (ev.action != KeyAction::NONE) {
        queue_.push_back(ev);
    }
}

int Input::esc_hold_ms() const {
    if (!esc_held_ || esc_exit_emitted_) return 0;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - esc_press_time_).count();
    return (ms > 0) ? static_cast<int>(ms) : 0;
}

void Input::poll(std::vector<KeyEvent>& out) {
    out.clear();
    for (size_t idx = 0; idx < fds_.size(); ++idx) {
        int fd = fds_[idx];
        if (fd < 0) continue;
        bool is_cpz = (idx < is_cardputer_.size()) ? is_cardputer_[idx] : true;
        struct input_event ev {};
        while (true) {
            ssize_t n = ::read(fd, &ev, sizeof(ev));
            if (n == sizeof(ev)) {
                if (ev.type == EV_KEY) {
                    handle_ev_key(ev.code, ev.value, is_cpz);
                }
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                break;
            }
            break;
        }
    }

    if (esc_held_ && !esc_exit_emitted_) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - esc_press_time_).count();
        if (elapsed >= kEscHoldExitMs) {
            esc_exit_emitted_ = true;
            KeyEvent ev;
            ev.action = KeyAction::EXIT_HOLD;
            queue_.push_back(ev);
        }
    }

    while (!queue_.empty()) {
        out.push_back(queue_.front());
        queue_.pop_front();
    }
}

}  // namespace cpz
