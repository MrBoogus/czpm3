#include "pm3_session.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <poll.h>
#include <pty.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace cpz {

namespace {

struct ChildEnv {
    std::vector<std::string> storage;
    std::vector<char*> ptrs;

    void build(const std::vector<std::pair<std::string, std::string>>& overrides) {
        storage.clear();
        ptrs.clear();
        std::vector<bool> applied(overrides.size(), false);
        if (::environ) {
            for (char** p = ::environ; *p; ++p) {
                std::string entry(*p);
                auto eq = entry.find('=');
                std::string key = (eq == std::string::npos) ? entry : entry.substr(0, eq);
                bool replaced = false;
                for (size_t i = 0; i < overrides.size(); ++i) {
                    if (key == overrides[i].first) {
                        storage.push_back(overrides[i].first + "=" + overrides[i].second);
                        applied[i] = true;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) {
                    storage.push_back(std::move(entry));
                }
            }
        }
        for (size_t i = 0; i < overrides.size(); ++i) {
            if (!applied[i]) {
                storage.push_back(overrides[i].first + "=" + overrides[i].second);
            }
        }
        ptrs.reserve(storage.size() + 1);
        for (auto& s : storage) {
            ptrs.push_back(const_cast<char*>(s.c_str()));
        }
        ptrs.push_back(nullptr);
    }

    char* const* data() const { return ptrs.data(); }
};

std::string resolve_executable_path(const std::string& prog) {
    if (prog.empty()) return {};
    if (prog.find('/') != std::string::npos) {
        return ::access(prog.c_str(), X_OK) == 0 ? prog : std::string{};
    }
    const char* path_env = std::getenv("PATH");
    std::string search = (path_env && *path_env) ? std::string(path_env) : "/usr/local/bin:/usr/bin:/bin";
    size_t start = 0;
    while (start <= search.size()) {
        size_t sep = search.find(':', start);
        std::string dir = (sep == std::string::npos) ? search.substr(start) : search.substr(start, sep - start);
        if (!dir.empty()) {
            std::string cand = dir + "/" + prog;
            if (::access(cand.c_str(), X_OK) == 0) {
                return cand;
            }
        }
        if (sep == std::string::npos) break;
        start = sep + 1;
    }
    return prog;
}

std::string read_trimmed_file(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs) return {};
    std::string s;
    std::getline(ifs, s);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
        s.pop_back();
    }
    return s;
}

std::string lower_copy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim_copy(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

std::string mac_dashed(std::string mac) {
    for (char& c : mac) {
        if (c == ':') c = '-';
    }
    return mac;
}

std::string strip_ansi_str(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    enum class S { N, E, C, O } st = S::N;
    for (unsigned char c : in) {
        if (st == S::E) {
            if (c == '[') st = S::C;
            else if (c == ']') st = S::O;
            else st = S::N;
        } else if (st == S::C) {
            if (c >= 0x40 && c <= 0x7E) st = S::N;
        } else if (st == S::O) {
            if (c == 0x07) st = S::N;
            else if (c == 0x1B) st = S::E;
        } else {
            if (c == 0x1B) st = S::E;
            else if (c == '\r') continue;
            else if (c >= 0x20 || c == '\n' || c == '\t') out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

bool ensure_dir_recursive(const std::string& dir) {
    std::string cur;
    for (size_t i = 0; i < dir.size(); ++i) {
        cur.push_back(dir[i]);
        if (dir[i] == '/' || i + 1 == dir.size()) {
            if (cur.size() > 1) {
                ::mkdir(cur.c_str(), 0755);
            }
        }
    }
    struct stat st {};
    return ::stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_bdaddr(const std::string& s) {
    if (s.size() != 17) return false;
    for (size_t i = 0; i < 17; ++i) {
        if ((i % 3) == 2) {
            if (s[i] != ':') return false;
        } else {
            if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
        }
    }
    return true;
}

bool write_all(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::write(fd, data + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

std::string run_pipe_cmd(const std::vector<const char*>& argv,
                         const std::string* stdin_data,
                         int timeout_ms,
                         int* exit_code = nullptr) {
    if (exit_code) *exit_code = -1;
    if (argv.empty() || !argv[0]) return {};

    std::string exe = resolve_executable_path(argv[0]);
    if (exe.empty()) return {};

    std::vector<std::string> arg_storage;
    arg_storage.reserve(argv.size());
    arg_storage.push_back(exe);
    for (size_t i = 1; i < argv.size() && argv[i] != nullptr; ++i) {
        arg_storage.emplace_back(argv[i]);
    }
    std::vector<char*> c_argv;
    c_argv.reserve(arg_storage.size() + 1);
    for (auto& s : arg_storage) {
        c_argv.push_back(const_cast<char*>(s.c_str()));
    }
    c_argv.push_back(nullptr);

    ChildEnv env;
    env.build({{"NO_COLOR", "1"}, {"TERM", "dumb"}});

    int in_pipe[2]{-1, -1};
    int out_pipe[2]{-1, -1};
    if (stdin_data && ::pipe2(in_pipe, O_CLOEXEC) != 0) {
        return {};
    }
    if (::pipe2(out_pipe, O_CLOEXEC) != 0) {
        if (in_pipe[0] >= 0) ::close(in_pipe[0]);
        if (in_pipe[1] >= 0) ::close(in_pipe[1]);
        return {};
    }

    pid_t pid = ::fork();
    if (pid == 0) {
        if (in_pipe[0] >= 0) {
            ::dup2(in_pipe[0], STDIN_FILENO);
            ::close(in_pipe[0]);
            ::close(in_pipe[1]);
        }
        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::dup2(out_pipe[1], STDERR_FILENO);
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        ::execve(exe.c_str(), c_argv.data(), env.data());
        ::_exit(127);
    }

    if (in_pipe[0] >= 0) ::close(in_pipe[0]);
    ::close(out_pipe[1]);

    if (pid < 0) {
        if (in_pipe[1] >= 0) ::close(in_pipe[1]);
        ::close(out_pipe[0]);
        return {};
    }

    int out_fl = ::fcntl(out_pipe[0], F_GETFL, 0);
    if (out_fl >= 0) ::fcntl(out_pipe[0], F_SETFL, out_fl | O_NONBLOCK);
    if (in_pipe[1] >= 0) {
        int in_fl = ::fcntl(in_pipe[1], F_GETFL, 0);
        if (in_fl >= 0) ::fcntl(in_pipe[1], F_SETFL, in_fl | O_NONBLOCK);
        if (stdin_data->empty()) {
            ::close(in_pipe[1]);
            in_pipe[1] = -1;
        }
    }

    std::string out;
    size_t in_sent = 0;
    bool timed_out = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    char buf[2048];

    while (out_pipe[0] >= 0) {
        auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (rem <= 0) {
            timed_out = true;
            break;
        }

        struct pollfd pfds[2]{};
        nfds_t nfds = 0;
        int out_idx = -1;
        int in_idx = -1;
        if (out_pipe[0] >= 0) {
            out_idx = static_cast<int>(nfds);
            pfds[nfds++] = {out_pipe[0], POLLIN, 0};
        }
        if (in_pipe[1] >= 0) {
            in_idx = static_cast<int>(nfds);
            pfds[nfds++] = {in_pipe[1], POLLOUT, 0};
        }

        int r = ::poll(pfds, nfds, static_cast<int>(rem));
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) {
            timed_out = true;
            break;
        }

        if (in_idx >= 0 && (pfds[in_idx].revents & (POLLOUT | POLLERR | POLLHUP))) {
            if (pfds[in_idx].revents & (POLLERR | POLLHUP)) {
                ::close(in_pipe[1]);
                in_pipe[1] = -1;
            } else {
                ssize_t w = ::write(in_pipe[1], stdin_data->data() + in_sent,
                                    stdin_data->size() - in_sent);
                if (w > 0) {
                    in_sent += static_cast<size_t>(w);
                    if (in_sent >= stdin_data->size()) {
                        ::close(in_pipe[1]);
                        in_pipe[1] = -1;
                    }
                } else if (w < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                    ::close(in_pipe[1]);
                    in_pipe[1] = -1;
                }
            }
        }

        if (out_idx >= 0 && (pfds[out_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            while (true) {
                ssize_t n = ::read(out_pipe[0], buf, sizeof(buf));
                if (n > 0) {
                    out.append(buf, static_cast<size_t>(n));
                } else if (n == 0) {
                    ::close(out_pipe[0]);
                    out_pipe[0] = -1;
                    break;
                } else if (errno == EINTR) {
                    continue;
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                } else {
                    ::close(out_pipe[0]);
                    out_pipe[0] = -1;
                    break;
                }
            }
        }
    }

    if (in_pipe[1] >= 0) ::close(in_pipe[1]);
    if (out_pipe[0] >= 0) ::close(out_pipe[0]);

    int st = 0;
    if (timed_out) {
        ::kill(pid, SIGTERM);
        bool reaped = false;
        for (int i = 0; i < 4; ++i) {
            if (::waitpid(pid, &st, WNOHANG) > 0) {
                reaped = true;
                break;
            }
            ::usleep(35000);
        }
        if (!reaped) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &st, 0);
        }
        if (exit_code) *exit_code = -1;
    } else if (::waitpid(pid, &st, 0) > 0) {
        if (exit_code) {
            *exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        }
    }
    return out;
}

std::string run_cmd_args(const std::vector<const char*>& argv, int* exit_code = nullptr, int timeout_ms = 5000) {
    return run_pipe_cmd(argv, nullptr, timeout_ms, exit_code);
}

std::string run_bluetoothctl_batch(const std::string& cmds, int timeout_ms = 5000) {
    std::vector<const char*> argv = {"bluetoothctl", nullptr};
    return run_pipe_cmd(argv, &cmds, timeout_ms, nullptr);
}

void read_pty_for_ms(int master_fd, int duration_ms, std::string& accum,
                     const std::atomic<bool>* stop_flag = nullptr) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(duration_ms);
    char buf[1024];
    while (std::chrono::steady_clock::now() < deadline) {
        if (stop_flag && stop_flag->load()) break;
        auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (rem <= 0) break;
        struct pollfd pfd{master_fd, POLLIN, 0};
        int r = ::poll(&pfd, 1, static_cast<int>(std::min<long long>(rem, 150)));
        if (r > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = ::read(master_fd, buf, sizeof(buf));
            if (n > 0) {
                accum.append(strip_ansi_str(std::string(buf, static_cast<size_t>(n))));
            }
        } else if (r > 0 && (pfd.revents & (POLLERR | POLLHUP))) {
            break;
        }
    }
}

void close_bt_pty(int master_fd, pid_t pid) {
    if (master_fd >= 0) {
        const char q[] = "quit\n";
        (void)::write(master_fd, q, sizeof(q) - 1);
    }
    for (int i = 0; i < 6; ++i) {
        int st = 0;
        if (::waitpid(pid, &st, WNOHANG) > 0) {
            if (master_fd >= 0) ::close(master_fd);
            return;
        }
        ::usleep(30000);
    }
    ::kill(pid, SIGTERM);
    for (int i = 0; i < 4; ++i) {
        int st = 0;
        if (::waitpid(pid, &st, WNOHANG) > 0) {
            if (master_fd >= 0) ::close(master_fd);
            return;
        }
        ::usleep(25000);
    }
    ::kill(pid, SIGKILL);
    int st = 0;
    ::waitpid(pid, &st, 0);
    if (master_fd >= 0) ::close(master_fd);
}

}  // namespace

Pm3Session::Pm3Session() {
    if (::access(wifi_paused_flag_path().c_str(), F_OK) == 0) {
        (void)set_wifi_enabled(true);
        (void)::unlink(wifi_paused_flag_path().c_str());
    }
    load_port_config();
    scan_ports();
    refresh_ble_devices_async();
    refresh_wifi_state_async();
}

Pm3Session::~Pm3Session() {
    stop_bt_worker_.store(true);
    close_session();
    if (ble_worker_.joinable()) {
        ble_worker_.join();
    }
    if (wifi_worker_.joinable()) {
        wifi_worker_.join();
    }
}

bool Pm3Session::is_non_serial_port(const std::string& port) {
    return port.rfind("ble:", 0) == 0 ||
           port.rfind("bt:", 0) == 0 ||
           port.rfind("tcp:", 0) == 0 ||
           port.rfind("udp:", 0) == 0 ||
           port.rfind("socket:", 0) == 0;
}

bool Pm3Session::is_wifi_enabled() {
    int rc = -1;
    std::string out = lower_copy(trim_copy(run_cmd_args({"nmcli", "radio", "wifi", nullptr}, &rc)));
    return (rc == 0 && out == "enabled");
}

bool Pm3Session::set_wifi_enabled(bool enable) {
    int rc = -1;
    (void)run_cmd_args({"nmcli", "radio", "wifi", enable ? "on" : "off", nullptr}, &rc);
    return rc == 0;
}

bool Pm3Session::looks_like_blueshark(const std::string& s) {
    std::string lo = lower_copy(s);
    return lo.find("rdv4") != std::string::npos ||
           lo.find("blueshark") != std::string::npos ||
           lo.find("hc-05") != std::string::npos ||
           lo.find("hc-06") != std::string::npos ||
           lo.find("pm3_rdv") != std::string::npos;
}

bool Pm3Session::looks_like_proxmark(const std::string& s) {
    if (looks_like_blueshark(s)) return true;
    std::string lo = lower_copy(s);
    return lo.find("proxmark") != std::string::npos ||
           lo.find("pm5") != std::string::npos ||
           lo.find("pm3") != std::string::npos ||
           lo.find("bwm") != std::string::npos ||
           lo.find("iceman") != std::string::npos ||
           lo.find("rfidresearch") != std::string::npos;
}

std::string Pm3Session::config_dir() const {
    const char* home = std::getenv("HOME");
    std::string base = (home && *home) ? std::string(home) : "/home/pi";
    return base + "/.config/czpm3";
}

std::string Pm3Session::config_file_path() const {
    return config_dir() + "/port.conf";
}

std::string Pm3Session::history_file_path() const {
    return config_dir() + "/history.txt";
}

std::string Pm3Session::wifi_paused_flag_path() const {
    return config_dir() + "/wifi_paused";
}

void Pm3Session::mark_wifi_paused_flag(bool paused) const {
    if (paused) {
        if (ensure_dir_recursive(config_dir())) {
            std::ofstream ofs(wifi_paused_flag_path(), std::ios::trunc);
            if (ofs) {
                ofs << "1\n";
            }
        }
    } else {
        (void)::unlink(wifi_paused_flag_path().c_str());
    }
}

void Pm3Session::refresh_wifi_state_async() {
    if (wifi_busy_.load()) return;
    if (wifi_worker_.joinable()) {
        wifi_worker_.join();
    }
    wifi_busy_.store(true);
    {
        std::lock_guard<std::mutex> lk(mu_);
        wifi_status_msg_.clear();
    }
    wifi_worker_ = std::thread([this]() {
        bool on = is_wifi_enabled();
        wifi_on_cached_.store(on);
        wifi_busy_.store(false);
        bump_version();
    });
}

bool Pm3Session::start_wifi_toggle(bool enable, bool clear_paused_flag) {
    if (wifi_busy_.load()) return false;
    if (wifi_worker_.joinable()) {
        wifi_worker_.join();
    }
    wifi_busy_.store(true);
    {
        std::lock_guard<std::mutex> lk(mu_);
        wifi_status_msg_.clear();
    }
    wifi_worker_ = std::thread([this, enable, clear_paused_flag]() {
        bool ok = set_wifi_enabled(enable);
        if (enable && (clear_paused_flag || ok)) {
            wifi_paused_for_bt_.store(false);
            mark_wifi_paused_flag(false);
        }
        bool now_on = is_wifi_enabled();
        wifi_on_cached_.store(now_on);
        std::string msg;
        if (clear_paused_flag) {
            msg = now_on ? "Wi-Fi restored" : "Wi-Fi restore failed";
        } else if (ok) {
            msg = now_on ? "Wi-Fi ON" : "Wi-Fi OFF (BlueShark ready)";
        } else {
            msg = "Wi-Fi toggle failed";
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            wifi_status_msg_ = std::move(msg);
        }
        wifi_busy_.store(false);
        bump_version();
    });
    return true;
}

std::string Pm3Session::wifi_status_msg() const {
    std::lock_guard<std::mutex> lk(mu_);
    return wifi_status_msg_;
}

void Pm3Session::load_port_config() {
    std::string val = read_trimmed_file(config_file_path());
    if (val.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (val == "offline") {
        selected_port_.clear();
        force_offline_ = true;
        bump_version();
    } else if (is_non_serial_port(val) || ::access(val.c_str(), F_OK) == 0) {
        selected_port_ = val;
        force_offline_ = false;
        bump_version();
    }
}

void Pm3Session::save_port_config(const std::string& val) const {
    std::string dir = config_dir();
    if (!ensure_dir_recursive(dir)) return;
    std::ofstream ofs(config_file_path());
    if (ofs) {
        ofs << (val.empty() ? "offline" : val) << '\n';
    }
}

std::vector<std::string> Pm3Session::load_cli_history() const {
    std::vector<std::string> out;
    std::ifstream ifs(history_file_path());
    if (!ifs) return out;
    std::string line;
    while (std::getline(ifs, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        if (line.empty()) continue;
        if (out.empty() || out.back() != line) {
            out.push_back(std::move(line));
        }
    }
    if (out.size() > 50) {
        out.erase(out.begin(), out.begin() + static_cast<long>(out.size() - 50));
    }
    return out;
}

void Pm3Session::save_cli_history(const std::vector<std::string>& hist) const {
    if (!ensure_dir_recursive(config_dir())) return;
    std::ofstream ofs(history_file_path());
    if (!ofs) return;
    size_t start = (hist.size() > 50) ? (hist.size() - 50) : 0;
    for (size_t i = start; i < hist.size(); ++i) {
        if (!hist[i].empty()) {
            ofs << hist[i] << '\n';
        }
    }
}

std::vector<SerialPortInfo> Pm3Session::scan_ports() {
    std::vector<SerialPortInfo> found;
    DIR* dir = ::opendir("/sys/class/tty");
    if (dir) {
        struct dirent* ent = nullptr;
        while ((ent = ::readdir(dir)) != nullptr) {
            std::string name(ent->d_name);
            if (name.rfind("ttyACM", 0) != 0 && name.rfind("ttyUSB", 0) != 0) {
                continue;
            }
            std::string dev_path = "/dev/" + name;
            if (::access(dev_path.c_str(), F_OK) != 0) {
                continue;
            }

            std::string sys_dev = "/sys/class/tty/" + name + "/device";
            std::string vid = lower_copy(read_trimmed_file(sys_dev + "/../idVendor"));
            std::string pid = lower_copy(read_trimmed_file(sys_dev + "/../idProduct"));
            std::string prod = read_trimmed_file(sys_dev + "/../product");
            std::string mfg = read_trimmed_file(sys_dev + "/../manufacturer");

            bool is_pm3 = (vid == "9ac4" && pid == "4b8f") ||
                          (vid == "2d2d" && pid == "504d") ||
                          looks_like_proxmark(prod) ||
                          looks_like_proxmark(mfg);

            SerialPortInfo info;
            info.path = dev_path;
            info.is_proxmark = is_pm3;
            if (is_pm3) {
                info.label = name + " (Proxmark3)";
            } else if (!prod.empty()) {
                info.label = name + " (" + prod + ")";
            } else {
                info.label = name;
            }
            found.push_back(std::move(info));
        }
        ::closedir(dir);
    }

    std::sort(found.begin(), found.end(), [](const SerialPortInfo& a, const SerialPortInfo& b) {
        if (a.is_proxmark != b.is_proxmark) return a.is_proxmark > b.is_proxmark;
        return a.path < b.path;
    });

    std::lock_guard<std::mutex> lk(mu_);
    if (!force_offline_ && !is_non_serial_port(selected_port_)) {
        bool still_present = false;
        if (!selected_port_.empty()) {
            for (const auto& p : found) {
                if (p.path == selected_port_) {
                    still_present = true;
                    break;
                }
            }
        }
        if (!still_present) {
            std::string prev = selected_port_;
            selected_port_.clear();
            for (const auto& p : found) {
                if (p.is_proxmark) {
                    selected_port_ = p.path;
                    break;
                }
            }
            if (selected_port_.empty() && !found.empty()) {
                selected_port_ = found.front().path;
            }
            if (selected_port_ != prev) {
                bump_version();
            }
        }
    }
    return found;
}

std::vector<BleDeviceInfo> Pm3Session::list_known_ble_devices() {
    std::vector<BleDeviceInfo> found;
    std::map<std::string, size_t> mac_index;

    std::string raw_devs = strip_ansi_str(run_cmd_args({"bluetoothctl", "devices", nullptr}));
    std::istringstream dss(raw_devs);
    std::string line;
    while (std::getline(dss, line)) {
        line = trim_copy(line);
        if (line.rfind("Device ", 0) != 0) continue;
        std::string rest = trim_copy(line.substr(7));
        auto sp = rest.find(' ');
        std::string mac = (sp == std::string::npos) ? rest : rest.substr(0, sp);
        if (!is_bdaddr(mac) || mac_index.count(mac)) continue;
        std::string name = (sp == std::string::npos) ? mac : trim_copy(rest.substr(sp + 1));
        if (name.empty()) name = mac;

        BleDeviceInfo dev;
        dev.mac = mac;
        dev.name = name;
        dev.is_classic_bt = looks_like_blueshark(name);
        dev.is_proxmark = looks_like_proxmark(name);
        mac_index[mac] = found.size();
        found.push_back(std::move(dev));
    }

    if (!found.empty()) {
        std::string batch;
        for (const auto& d : found) {
            batch += "info " + d.mac + "\n";
        }
        batch += "quit\n";

        std::string raw_info = strip_ansi_str(run_bluetoothctl_batch(batch));
        std::vector<bool> has_real_name(found.size(), false);
        std::istringstream iss(raw_info);
        ssize_t cur = -1;
        while (std::getline(iss, line)) {
            std::string trimmed = trim_copy(line);
            if (trimmed.rfind("Device ", 0) == 0) {
                std::string rest = trim_copy(trimmed.substr(7));
                auto sp = rest.find(' ');
                std::string mac = (sp == std::string::npos) ? rest : rest.substr(0, sp);
                auto it = mac_index.find(mac);
                cur = (it != mac_index.end()) ? static_cast<ssize_t>(it->second) : -1;
                continue;
            }
            if (cur < 0) continue;
            size_t cidx = static_cast<size_t>(cur);
            BleDeviceInfo& dev = found[cidx];
            if (trimmed.rfind("Name:", 0) == 0) {
                std::string val = trim_copy(trimmed.substr(5));
                if (!val.empty()) {
                    dev.name = val;
                    has_real_name[cidx] = true;
                }
            } else if (trimmed.rfind("Alias:", 0) == 0) {
                std::string val = trim_copy(trimmed.substr(6));
                if (!has_real_name[cidx] && !val.empty()) {
                    dev.name = val;
                }
            } else if (trimmed.rfind("Class:", 0) == 0) {
                dev.is_classic_bt = true;
                std::string cls = lower_copy(trimmed.substr(6));
                if (cls.find("1f00") != std::string::npos) {
                    dev.is_proxmark = true;
                }
            } else if (trimmed.rfind("Paired:", 0) == 0) {
                dev.is_paired = (lower_copy(trimmed.substr(7)).find("yes") != std::string::npos);
            } else if (trimmed.rfind("Trusted:", 0) == 0) {
                dev.is_trusted = (lower_copy(trimmed.substr(8)).find("yes") != std::string::npos);
            } else if (trimmed.rfind("UUID:", 0) == 0) {
                std::string u = lower_copy(trimmed.substr(5));
                if (u.find("00001101") != std::string::npos) {
                    dev.is_classic_bt = true;
                    dev.is_proxmark = true;
                } else if (u.find("0000ae86") != std::string::npos) {
                    dev.is_classic_bt = false;
                    dev.is_proxmark = true;
                }
            }
        }

        for (auto& dev : found) {
            if (looks_like_blueshark(dev.name)) {
                dev.is_classic_bt = true;
                dev.is_proxmark = true;
            } else if (looks_like_proxmark(dev.name)) {
                dev.is_proxmark = true;
            }
            if (dev.is_classic_bt &&
                (dev.name == dev.mac || lower_copy(dev.name) == lower_copy(mac_dashed(dev.mac)))) {
                dev.name = std::string(dev.is_proxmark ? "BlueShark/BT (" : "BT Device (") +
                           dev.mac.substr(9) + ")";
            }
        }
    }

    std::sort(found.begin(), found.end(), [](const BleDeviceInfo& a, const BleDeviceInfo& b) {
        if (a.is_proxmark != b.is_proxmark) return a.is_proxmark > b.is_proxmark;
        if (a.is_paired != b.is_paired) return a.is_paired > b.is_paired;
        return a.name < b.name;
    });

    std::lock_guard<std::mutex> lk(mu_);
    ble_devices_ = found;
    bump_version();
    return found;
}

void Pm3Session::refresh_ble_devices_async() {
    if (ble_scanning_.load() || bt_pairing_.load() || ble_worker_busy_.load()) return;
    if (ble_worker_.joinable()) {
        ble_worker_.join();
    }
    stop_bt_worker_.store(false);
    ble_worker_busy_.store(true);
    ble_worker_ = std::thread([this]() {
        list_known_ble_devices();
        ble_worker_busy_.store(false);
    });
}

bool Pm3Session::start_ble_scan() {
    if (ble_scanning_.load() || bt_pairing_.load() || ble_worker_busy_.load()) return false;
    if (ble_worker_.joinable()) {
        ble_worker_.join();
    }
    stop_bt_worker_.store(false);
    ble_scanning_.store(true);
    ble_worker_busy_.store(true);
    bump_version();
    ble_worker_ = std::thread([this]() {
        std::string bt_exe = resolve_executable_path("bluetoothctl");
        ChildEnv env;
        env.build({{"NO_COLOR", "1"}, {"TERM", "dumb"}});
        char* const argv[] = {const_cast<char*>(bt_exe.c_str()), nullptr};

        int mfd = -1;
        pid_t pid = ::forkpty(&mfd, nullptr, nullptr, nullptr);
        if (pid == 0) {
            ::execve(bt_exe.c_str(), argv, env.data());
            ::_exit(127);
        }
        if (pid > 0 && mfd >= 0) {
            ::fcntl(mfd, F_SETFD, FD_CLOEXEC);
            std::string accum;
            read_pty_for_ms(mfd, 600, accum, &stop_bt_worker_);
            if (!stop_bt_worker_.load()) {
                const char bredr_cmd[] = "power on\nscan bredr\n";
                (void)write_all(mfd, bredr_cmd, sizeof(bredr_cmd) - 1);
                read_pty_for_ms(mfd, 8000, accum, &stop_bt_worker_);
                const char stop1[] = "scan off\n";
                (void)write_all(mfd, stop1, sizeof(stop1) - 1);
                read_pty_for_ms(mfd, 350, accum, &stop_bt_worker_);
            }
            if (!stop_bt_worker_.load()) {
                const char le_cmd[] = "scan le\n";
                (void)write_all(mfd, le_cmd, sizeof(le_cmd) - 1);
                read_pty_for_ms(mfd, 4000, accum, &stop_bt_worker_);
                const char stop2[] = "scan off\n";
                (void)write_all(mfd, stop2, sizeof(stop2) - 1);
                read_pty_for_ms(mfd, 250, accum, &stop_bt_worker_);
            }
            close_bt_pty(mfd, pid);
        }
        if (!stop_bt_worker_.load()) {
            list_known_ble_devices();
        }
        ble_scanning_.store(false);
        ble_worker_busy_.store(false);
        bump_version();
    });
    return true;
}

bool Pm3Session::start_bt_pair(const std::string& mac, const std::string& pin) {
    if (!is_bdaddr(mac)) return false;
    if (ble_scanning_.load() || bt_pairing_.load() || ble_worker_busy_.load()) return false;
    if (ble_worker_.joinable()) {
        ble_worker_.join();
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        bt_pair_target_ = mac;
        bt_pair_status_msg_ = "Pairing " + mac + "...";
    }
    stop_bt_worker_.store(false);
    bt_pair_ok_.store(false);
    bt_pairing_.store(true);
    ble_worker_busy_.store(true);
    bump_version();

    ble_worker_ = std::thread([this, mac, pin]() {
        bool paired = false;
        std::string status = "Pair failed (" + mac.substr(9) + ")";

        if (is_wifi_enabled()) {
            mark_wifi_paused_flag(true);
            if (set_wifi_enabled(false)) {
                wifi_paused_for_bt_.store(true);
                wifi_on_cached_.store(false);
                for (int i = 0; i < 12 && !stop_bt_worker_.load(); ++i) {
                    ::usleep(100000);
                }
            } else {
                mark_wifi_paused_flag(false);
            }
        }

        std::string bt_exe = resolve_executable_path("bluetoothctl");
        ChildEnv env;
        env.build({{"NO_COLOR", "1"}, {"TERM", "dumb"}});
        char* const argv[] = {const_cast<char*>(bt_exe.c_str()), nullptr};

        int mfd = -1;
        pid_t pid = ::forkpty(&mfd, nullptr, nullptr, nullptr);
        if (pid == 0) {
            ::execve(bt_exe.c_str(), argv, env.data());
            ::_exit(127);
        }
        if (pid > 0 && mfd >= 0) {
            ::fcntl(mfd, F_SETFD, FD_CLOEXEC);
            std::string accum;
            read_pty_for_ms(mfd, 700, accum, &stop_bt_worker_);

            auto send_step = [&](const std::string& s, int wait_ms) {
                if (stop_bt_worker_.load()) return;
                (void)write_all(mfd, s.data(), s.size());
                read_pty_for_ms(mfd, wait_ms, accum, &stop_bt_worker_);
            };

            send_step("power on\n", 150);
            send_step("pairable on\n", 150);
            send_step("agent off\n", 250);
            send_step("agent KeyboardOnly\n", 250);
            send_step("default-agent\n", 250);

            accum.clear();
            send_step("info " + mac + "\n", 600);
            std::string info_lo = lower_copy(accum);
            bool was_paired_before = (info_lo.find("not available") == std::string::npos &&
                                      info_lo.find("paired: yes") != std::string::npos);
            if (was_paired_before) {
                send_step("trust " + mac + "\n", 250);
                paired = true;
                status = "Paired BT: " + mac.substr(9);
            } else {
                if (info_lo.find("not available") != std::string::npos && !stop_bt_worker_.load()) {
                    accum.clear();
                    send_step("scan bredr\n", 200);
                    auto s_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
                    std::string lo_mac = lower_copy(mac);
                    while (std::chrono::steady_clock::now() < s_deadline && !stop_bt_worker_.load()) {
                        read_pty_for_ms(mfd, 300, accum, &stop_bt_worker_);
                        if (lower_copy(accum).find(lo_mac) != std::string::npos) break;
                    }
                    send_step("scan off\n", 400);
                }

                if (!stop_bt_worker_.load()) {
                    accum.clear();
                    std::string pcmd = "pair " + mac + "\n";
                    (void)write_all(mfd, pcmd.data(), pcmd.size());

                    bool sent_pin = false;
                    std::string lo_mac = lower_copy(mac);
                    auto p_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
                    while (std::chrono::steady_clock::now() < p_deadline && !stop_bt_worker_.load()) {
                        read_pty_for_ms(mfd, 200, accum, &stop_bt_worker_);

                        bool done = false;
                        size_t pos = 0;
                        while (pos <= accum.size()) {
                            size_t nl = accum.find('\n', pos);
                            std::string line = (nl == std::string::npos)
                                                   ? accum.substr(pos)
                                                   : accum.substr(pos, nl - pos);
                            std::string lo = lower_copy(line);

                            bool is_pin_prompt =
                                (lo.find("enter pin") != std::string::npos ||
                                 lo.find("pin code") != std::string::npos ||
                                 lo.find("enter passkey") != std::string::npos ||
                                 lo.find("request pin") != std::string::npos);
                            if (is_pin_prompt) {
                                if (!sent_pin && lo.find(lo_mac) != std::string::npos) {
                                    std::string p_reply = pin + "\n";
                                    (void)write_all(mfd, p_reply.data(), p_reply.size());
                                    sent_pin = true;
                                } else {
                                    const char no[] = "no\n";
                                    (void)write_all(mfd, no, sizeof(no) - 1);
                                }
                                accum.clear();
                                break;
                            }

                            if (lo.find("(yes/no)") != std::string::npos ||
                                lo.find("confirm passkey") != std::string::npos ||
                                lo.find("authorize service") != std::string::npos ||
                                lo.find("request confirmation") != std::string::npos ||
                                lo.find("request authorization") != std::string::npos) {
                                const char no[] = "no\n";
                                (void)write_all(mfd, no, sizeof(no) - 1);
                                accum.clear();
                                break;
                            }

                            if (lo.find("pairing successful") != std::string::npos ||
                                lo.find("alreadyexists") != std::string::npos) {
                                paired = true;
                                status = "Paired BT: " + mac.substr(9);
                                done = true;
                                break;
                            }
                            if (lo.find("connectionattemptfailed") != std::string::npos ||
                                lo.find("page timeout") != std::string::npos) {
                                paired = false;
                                status = is_wifi_enabled()
                                             ? "Timeout: turn Wi-Fi OFF"
                                             : ("Pair timeout (" + mac.substr(9) + ")");
                                done = true;
                                break;
                            }
                            if (lo.find("authenticationfailed") != std::string::npos ||
                                lo.find("authenticationrejected") != std::string::npos) {
                                paired = false;
                                status = "Wrong PIN (" + mac.substr(9) + ")";
                                done = true;
                                break;
                            }
                            if (lo.find("failed to pair") != std::string::npos ||
                                lo.find("authenticationcanceled") != std::string::npos) {
                                paired = false;
                                status = "Pair failed (" + mac.substr(9) + ")";
                                done = true;
                                break;
                            }

                            if (nl == std::string::npos) break;
                            pos = nl + 1;
                        }
                        if (done) break;
                        size_t last_nl = accum.rfind('\n');
                        if (last_nl != std::string::npos) {
                            accum.erase(0, last_nl + 1);
                        }
                    }

                    if (paired) {
                        send_step("trust " + mac + "\n", 250);
                    } else if (!was_paired_before) {
                        send_step("untrust " + mac + "\n", 250);
                        send_step("remove " + mac + "\n", 250);
                    }
                }
            }
            const char poff[] = "pairable off\n";
            (void)write_all(mfd, poff, sizeof(poff) - 1);
            read_pty_for_ms(mfd, 150, accum, &stop_bt_worker_);
            close_bt_pty(mfd, pid);
        }

        if (!stop_bt_worker_.load()) {
            auto devs = list_known_ble_devices();
            for (const auto& d : devs) {
                if (lower_copy(d.mac) == lower_copy(mac) && d.is_paired) {
                    paired = true;
                    break;
                }
            }
            if (paired) {
                status = "Paired BT: " + mac.substr(9);
                set_selected_port("bt:" + mac);
            }
        }
        if (!paired && wifi_paused_for_bt_.exchange(false)) {
            (void)set_wifi_enabled(true);
            mark_wifi_paused_flag(false);
            wifi_on_cached_.store(is_wifi_enabled());
        }

        {
            std::lock_guard<std::mutex> lk(mu_);
            bt_pair_status_msg_ = status;
        }
        bt_pair_ok_.store(paired);
        bt_pairing_.store(false);
        ble_worker_busy_.store(false);
        bump_version();
    });
    return true;
}

std::string Pm3Session::bt_pair_target() const {
    std::lock_guard<std::mutex> lk(mu_);
    return bt_pair_target_;
}

std::string Pm3Session::bt_pair_status_msg() const {
    std::lock_guard<std::mutex> lk(mu_);
    return bt_pair_status_msg_;
}

std::vector<BleDeviceInfo> Pm3Session::ble_devices_snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return ble_devices_;
}

void Pm3Session::set_selected_port(const std::string& port) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::string prev = force_offline_ ? "" : selected_port_;
        if (port.empty()) {
            selected_port_.clear();
            force_offline_ = true;
        } else {
            selected_port_ = port;
            force_offline_ = false;
        }
        std::string next = force_offline_ ? "" : selected_port_;
        changed = (prev != next);
        bump_version();
    }
    if (changed) {
        close_session(port.rfind("bt:", 0) != 0);
    }
    save_port_config(port);
}

std::string Pm3Session::selected_port() const {
    std::lock_guard<std::mutex> lk(mu_);
    return selected_port_;
}

bool Pm3Session::is_offline() const {
    std::lock_guard<std::mutex> lk(mu_);
    return force_offline_ || selected_port_.empty();
}

std::string Pm3Session::active_command() const {
    std::lock_guard<std::mutex> lk(mu_);
    return active_cmd_;
}

std::string Pm3Session::find_pm3_binary() const {
    if (const char* env = std::getenv("PM3_BIN")) {
        if (*env && ::access(env, X_OK) == 0) return env;
    }
    const char* candidates[] = {
        "/usr/bin/proxmark3",
        "/usr/local/bin/proxmark3",
    };
    for (const char* p : candidates) {
        if (::access(p, X_OK) == 0) return p;
    }
    return "proxmark3";
}

bool Pm3Session::write_to_master(const char* data, size_t len) {
    std::lock_guard<std::mutex> lk(mu_);
    if (master_fd_ < 0) return false;
    return write_all(master_fd_, data, len);
}

void Pm3Session::close_session(bool restore_wifi) {
    abort_launch_.store(true);
    int pid = child_pid_.load();

    if (pid > 0 && session_alive_.load()) {
        const char etx_quit[] = "\x03\nquit\n";
        (void)write_to_master(etx_quit, sizeof(etx_quit) - 1);
        for (int i = 0; i < 10 && session_alive_.load(); ++i) {
            ::usleep(15000);
        }
        pid = child_pid_.load();
        if (pid > 0 && session_alive_.load()) {
            ::kill(pid, SIGTERM);
            for (int i = 0; i < 7 && session_alive_.load(); ++i) {
                ::usleep(15000);
            }
        }
        pid = child_pid_.load();
        if (pid > 0 && session_alive_.load()) {
            ::kill(pid, SIGKILL);
        }
    }

    if (worker_.joinable()) {
        worker_.join();
    }
    session_alive_.store(false);
    session_connected_.store(false);
    prompt_ready_.store(false);
    running_.store(false);
    if (restore_wifi && wifi_paused_for_bt_.exchange(false)) {
        (void)set_wifi_enabled(true);
        mark_wifi_paused_flag(false);
        wifi_on_cached_.store(true);
    }
    bump_version();
}

bool Pm3Session::run_command(const std::string& cmd) {
    return run_command_internal(cmd, true);
}

bool Pm3Session::run_command_internal(const std::string& cmd, bool log_cmd) {
    if (cmd.empty()) return false;
    if (running_.load()) return false;

    std::string desired_port;
    bool need_restart = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        desired_port = force_offline_ ? std::string() : selected_port_;
        need_restart = !session_alive_.load() || !prompt_ready_.load() ||
                       (session_port_ != desired_port) || (master_fd_ < 0);
        if (!desired_port.empty() && !session_connected_.load()) {
            need_restart = true;
        }
    }

    if (need_restart) {
        bool is_bt = (desired_port.rfind("bt:", 0) == 0);
        close_session(!is_bt);
        if (worker_.joinable()) {
            worker_.join();
        }

        {
            std::lock_guard<std::mutex> lk(mu_);
            session_port_ = desired_port;
            active_cmd_ = cmd;
            pending_cmd_ = cmd;
            last_sent_cmd_.clear();
            first_prompt_seen_ = false;
            pending_cr_ = false;
            cmd_had_err_ = false;
            cmd_had_warn_ = false;
            cur_line_.clear();
            cur_ansi_kind_ = LineKind::NORMAL;
            esc_state_ = EscState::NONE;
            csi_buf_.clear();
            if (log_cmd) {
                lines_.push_back({"$ " + cmd, LineKind::CMD});
            }
            bump_version();
        }

        abort_launch_.store(false);
        child_pid_.store(-1);
        last_exit_code_.store(0);
        last_cmd_status_.store(CmdStatus::NONE);
        prompt_ready_.store(false);
        session_connected_.store(false);
        session_alive_.store(true);
        running_.store(true);
        bump_version();

        std::string bin = resolve_executable_path(find_pm3_binary());
        worker_ = std::thread([this, desired_port, is_bt, bin]() {
            if (is_bt && is_wifi_enabled()) {
                append_local_line("[=] Pausing Wi-Fi for Classic BT...", LineKind::INFO);
                mark_wifi_paused_flag(true);
                if (set_wifi_enabled(false)) {
                    wifi_paused_for_bt_.store(true);
                    wifi_on_cached_.store(false);
                    for (int i = 0; i < 12 && !abort_launch_.load(); ++i) {
                        ::usleep(100000);
                    }
                } else {
                    mark_wifi_paused_flag(false);
                }
            }

            if (abort_launch_.load()) {
                if (wifi_paused_for_bt_.exchange(false)) {
                    (void)set_wifi_enabled(true);
                    mark_wifi_paused_flag(false);
                    wifi_on_cached_.store(true);
                }
                session_alive_.store(false);
                running_.store(false);
                bump_version();
                return;
            }

            if (bin.empty()) {
                append_local_line("[!] proxmark3 binary not found", LineKind::ERROR);
                last_cmd_status_.store(CmdStatus::ERR);
                session_alive_.store(false);
                running_.store(false);
                bump_version();
                return;
            }

            ChildEnv env;
            env.build({{"TERM", "xterm-256color"}, {"COLUMNS", "48"}, {"LINES", "16"}});

            std::vector<std::string> arg_storage;
            arg_storage.push_back(bin);
            if (!desired_port.empty()) {
                arg_storage.emplace_back("-p");
                arg_storage.push_back(desired_port);
            }
            arg_storage.emplace_back("-f");

            std::vector<char*> args;
            args.reserve(arg_storage.size() + 1);
            for (auto& a : arg_storage) {
                args.push_back(const_cast<char*>(a.c_str()));
            }
            args.push_back(nullptr);

            struct winsize ws {};
            ws.ws_row = 16;
            ws.ws_col = 48;
            ws.ws_xpixel = 320;
            ws.ws_ypixel = 170;

            int master_fd = -1;
            pid_t pid = ::forkpty(&master_fd, nullptr, nullptr, &ws);
            if (pid == 0) {
                ::execve(bin.c_str(), args.data(), env.data());
                ::_exit(127);
            }
            if (pid < 0 || master_fd < 0) {
                if (master_fd >= 0) ::close(master_fd);
                append_local_line("[!] forkpty failed", LineKind::ERROR);
                last_cmd_status_.store(CmdStatus::ERR);
                session_alive_.store(false);
                running_.store(false);
                bump_version();
                return;
            }

            ::fcntl(master_fd, F_SETFD, FD_CLOEXEC);
            {
                std::lock_guard<std::mutex> lk(mu_);
                master_fd_ = master_fd;
            }
            child_pid_.store(pid);
            if (abort_launch_.load()) {
                ::kill(pid, SIGTERM);
            }
            bump_version();
            reader_loop(master_fd, static_cast<int>(pid));
        });
        return true;
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        active_cmd_ = cmd;
        pending_cmd_.clear();
        last_sent_cmd_ = cmd;
        pending_cr_ = false;
        cmd_had_err_ = false;
        cmd_had_warn_ = false;
        cur_line_.clear();
        cur_ansi_kind_ = LineKind::NORMAL;
        if (log_cmd) {
            lines_.push_back({"$ " + cmd, LineKind::CMD});
        }
        bump_version();
    }

    prompt_ready_.store(false);
    last_exit_code_.store(0);
    last_cmd_status_.store(CmdStatus::NONE);
    running_.store(true);
    bump_version();

    std::string payload = cmd + "\n";
    if (!write_to_master(payload.data(), payload.size())) {
        close_session();
        return run_command_internal(cmd, false);
    }
    return true;
}

bool Pm3Session::send_enter() {
    if (!running_.load()) return false;
    const char nl = '\n';
    return write_to_master(&nl, 1);
}

bool Pm3Session::request_stop() {
    return send_enter();
}

void Pm3Session::abort() {
    abort_launch_.store(true);
    int pid = child_pid_.load();
    if (pid > 0 && running_.load()) {
        const char etx = 0x03;
        (void)write_to_master(&etx, 1);
        ::kill(pid, SIGINT);
    }
}

void Pm3Session::reader_loop(int master_fd, int pid) {
    std::array<char, 4096> buf{};
    while (true) {
        ssize_t n = ::read(master_fd, buf.data(), buf.size());
        if (n > 0) {
            process_raw_chunk(buf.data(), static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        finalize_current_line();
        if (master_fd_ == master_fd) {
            master_fd_ = -1;
        }
    }
    ::close(master_fd);

    int status = 0;
    int exit_rc = 0;
    if (::waitpid(pid, &status, 0) > 0) {
        if (WIFEXITED(status)) {
            exit_rc = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            exit_rc = 128 + WTERMSIG(status);
        }
    }
    bool died_mid_cmd = running_.load();
    if (died_mid_cmd && exit_rc == 0) {
        exit_rc = 1;
    }
    last_exit_code_.store(exit_rc);
    if (died_mid_cmd || exit_rc != 0) {
        last_cmd_status_.store(CmdStatus::ERR);
    }
    child_pid_.store(-1);
    session_alive_.store(false);
    session_connected_.store(false);
    prompt_ready_.store(false);
    running_.store(false);
    bump_version();
}

bool Pm3Session::parse_prompt_line(const std::string& s, bool* out_connected) {
    if (s.empty()) return false;

    size_t idx = 0;
    bool connected = false;
    if (s[0] == '[') {
        size_t rb = s.find(']', 1);
        if (rb == std::string::npos || rb == 1) return false;
        std::string tag = lower_copy(s.substr(1, rb - 1));
        connected = (tag.find("offline") == std::string::npos);
        idx = rb + 1;
        if (idx >= s.size() || s[idx] != ' ') return false;
        ++idx;
    }

    bool match_pm3 = (s.compare(idx, 7, "pm3 -->") == 0);
    bool match_pm5 = (s.compare(idx, 7, "pm5 -->") == 0);
    if (!match_pm3 && !match_pm5) return false;

    size_t tail = idx + 7;
    if (tail == s.size() || (tail + 1 == s.size() && s[tail] == ' ')) {
        if (out_connected) *out_connected = connected;
        return true;
    }
    return false;
}

LineKind Pm3Session::parse_sgr_kind(const std::string& params, LineKind current) {
    if (params.empty()) return current;
    size_t start = 0;
    bool reset = false;
    LineKind picked = LineKind::NORMAL;
    while (start <= params.size()) {
        size_t sep = params.find(';', start);
        size_t end = (sep == std::string::npos) ? params.size() : sep;
        int code = 0;
        bool valid = (end > start);
        for (size_t i = start; i < end; ++i) {
            if (!std::isdigit(static_cast<unsigned char>(params[i]))) {
                valid = false;
                break;
            }
            code = code * 10 + (params[i] - '0');
        }
        if (valid) {
            if (code == 0) {
                reset = true;
            } else if (code == 38 || code == 48) {
                break;
            } else if (picked == LineKind::NORMAL) {
                if (code == 32 || code == 92) picked = LineKind::SUCCESS;
                else if (code == 31 || code == 91) picked = LineKind::ERROR;
                else if (code == 33 || code == 93) picked = LineKind::WARN;
                else if (code == 36 || code == 96) picked = LineKind::INFO;
            }
        }
        if (sep == std::string::npos) break;
        start = sep + 1;
    }
    if (current != LineKind::NORMAL) return current;
    if (picked != LineKind::NORMAL) return picked;
    return reset ? LineKind::NORMAL : current;
}

LineKind Pm3Session::classify_line(const std::string& clean, LineKind ansi_hint) {
    if (clean.find("[+]") != std::string::npos) return LineKind::SUCCESS;
    if (clean.find("[!]") != std::string::npos || clean.find("[!!]") != std::string::npos) return LineKind::ERROR;
    if (clean.find("[*]") != std::string::npos || clean.find("[-]") != std::string::npos) return LineKind::WARN;
    if (clean.find("[=]") != std::string::npos) return LineKind::INFO;
    if (clean.find("[?]") != std::string::npos) return LineKind::HINT;
    if (clean.find("[#]") != std::string::npos) return LineKind::DEV;
    if (clean.rfind("ERROR", 0) == 0) return LineKind::ERROR;
    return ansi_hint;
}

void Pm3Session::finalize_current_line() {
    if (cur_line_.empty()) return;
    if (parse_prompt_line(cur_line_)) {
        cur_line_.clear();
        cur_ansi_kind_ = LineKind::NORMAL;
        return;
    }
    if (!last_sent_cmd_.empty()) {
        if (cur_line_ == last_sent_cmd_ ||
            (cur_line_.rfind("--> " + last_sent_cmd_) != std::string::npos &&
             cur_line_.size() < last_sent_cmd_.size() + 32)) {
            last_sent_cmd_.clear();
            cur_line_.clear();
            cur_ansi_kind_ = LineKind::NORMAL;
            return;
        }
    }
    bool bt_conn_err = (cur_line_.find("cannot connect device") != std::string::npos &&
                        cur_line_.find("over Bluetooth") != std::string::npos);
    if (!first_prompt_seen_) {
        bool keep = (bt_conn_err ||
                     cur_line_.find("[+]") != std::string::npos ||
                     cur_line_.find("[!]") != std::string::npos ||
                     cur_line_.find("[!!]") != std::string::npos ||
                     cur_line_.find("[*]") != std::string::npos ||
                     cur_line_.find("[=]") != std::string::npos ||
                     cur_line_.find("[#]") != std::string::npos ||
                     cur_line_.find("[?]") != std::string::npos);
        if (!keep) {
            cur_line_.clear();
            cur_ansi_kind_ = LineKind::NORMAL;
            return;
        }
    }
    LineKind kind = classify_line(cur_line_, cur_ansi_kind_);
    if (bt_conn_err) {
        bt_wifi_hint_needed_.store(true);
    }
    if (first_prompt_seen_ && running_.load()) {
        if (kind == LineKind::ERROR ||
            cur_line_.find("Timed out") != std::string::npos ||
            cur_line_.find("timeout while waiting") != std::string::npos ||
            cur_line_.find("Communicating with Proxmark3 device failed") != std::string::npos ||
            cur_line_.find("Command not found") != std::string::npos) {
            cmd_had_err_ = true;
        } else if (cur_line_.find("[-]") != std::string::npos ||
                   cur_line_.find("No known") != std::string::npos ||
                   cur_line_.find("Can't select") != std::string::npos ||
                   cur_line_.find("not found") != std::string::npos) {
            cmd_had_warn_ = true;
        }
    }
    lines_.push_back({cur_line_, kind});
    if (lines_.size() > 600) {
        lines_.erase(lines_.begin(), lines_.begin() + static_cast<long>(lines_.size() - 600));
    }
    cur_line_.clear();
    cur_ansi_kind_ = LineKind::NORMAL;
}

void Pm3Session::check_interactive_prompt() {
    bool connected = false;
    if (!parse_prompt_line(cur_line_, &connected)) {
        return;
    }

    first_prompt_seen_ = true;
    session_connected_.store(connected);
    cur_line_.clear();
    cur_ansi_kind_ = LineKind::NORMAL;

    if (!pending_cmd_.empty() && master_fd_ >= 0) {
        std::string to_send = pending_cmd_;
        pending_cmd_.clear();
        last_sent_cmd_ = to_send;
        cmd_had_err_ = false;
        cmd_had_warn_ = false;
        std::string payload = to_send + "\n";
        (void)write_all(master_fd_, payload.data(), payload.size());
    } else {
        CmdStatus st = cmd_had_err_ ? CmdStatus::ERR
                                    : (cmd_had_warn_ ? CmdStatus::WARN : CmdStatus::OK);
        cmd_had_err_ = false;
        cmd_had_warn_ = false;
        last_cmd_status_.store(st);
        prompt_ready_.store(true);
        last_exit_code_.store(0);
        running_.store(false);
    }
}

void Pm3Session::process_raw_chunk(const char* data, size_t len) {
    {
        std::lock_guard<std::mutex> lk(mu_);

        for (size_t i = 0; i < len; ++i) {
            unsigned char c = static_cast<unsigned char>(data[i]);

            if (esc_state_ == EscState::ESC) {
                if (c == '[') {
                    esc_state_ = EscState::CSI;
                    csi_buf_.clear();
                } else if (c == ']') {
                    esc_state_ = EscState::OSC;
                } else {
                    esc_state_ = EscState::NONE;
                }
                continue;
            }

            if (esc_state_ == EscState::CSI) {
                if (c >= 0x40 && c <= 0x7E) {
                    if (c == 'm') {
                        cur_ansi_kind_ = parse_sgr_kind(csi_buf_, cur_ansi_kind_);
                    } else if (c == 'K' && (csi_buf_.empty() || csi_buf_ == "2")) {
                        cur_line_.clear();
                        cur_ansi_kind_ = LineKind::NORMAL;
                    }
                    esc_state_ = EscState::NONE;
                } else {
                    csi_buf_.push_back(static_cast<char>(c));
                }
                continue;
            }

            if (esc_state_ == EscState::OSC) {
                if (c == 0x07) {
                    esc_state_ = EscState::NONE;
                } else if (c == 0x1B) {
                    esc_state_ = EscState::ESC;
                }
                continue;
            }

            if (c == 0x1B) {
                esc_state_ = EscState::ESC;
                continue;
            }
            if (c == '\r') {
                pending_cr_ = true;
                continue;
            }
            if (c == '\n') {
                pending_cr_ = false;
                finalize_current_line();
                continue;
            }
            if (c == '\t') {
                if (pending_cr_) {
                    cur_line_.clear();
                    cur_ansi_kind_ = LineKind::NORMAL;
                    pending_cr_ = false;
                }
                cur_line_.append("  ");
                continue;
            }
            if (c >= 0x20 && c != 0x7F) {
                if (pending_cr_) {
                    cur_line_.clear();
                    cur_ansi_kind_ = LineKind::NORMAL;
                    pending_cr_ = false;
                }
                cur_line_.push_back(static_cast<char>(c));
                check_interactive_prompt();
            }
        }
        if (len > 0) {
            bump_version();
        }
    }

    if (bt_wifi_hint_needed_.exchange(false)) {
        if (selected_port().rfind("bt:", 0) == 0 && is_wifi_enabled()) {
            append_local_line("[?] Turn Wi-Fi OFF in Device/BT for Classic BT", LineKind::HINT);
        }
    }
}

std::vector<OutputLine> Pm3Session::lines_snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<OutputLine> snap = lines_;
    if (!cur_line_.empty() && !parse_prompt_line(cur_line_) && first_prompt_seen_) {
        bool all_spaces = true;
        for (char c : cur_line_) {
            if (c != ' ') {
                all_spaces = false;
                break;
            }
        }
        if (!all_spaces) {
            snap.push_back({cur_line_, classify_line(cur_line_, cur_ansi_kind_)});
        }
    }
    return snap;
}

void Pm3Session::clear_output() {
    std::lock_guard<std::mutex> lk(mu_);
    lines_.clear();
    cur_line_.clear();
    pending_cr_ = false;
    cur_ansi_kind_ = LineKind::NORMAL;
    last_cmd_status_.store(CmdStatus::NONE);
    bump_version();
}

void Pm3Session::append_local_line(const std::string& text, LineKind kind) {
    std::lock_guard<std::mutex> lk(mu_);
    lines_.push_back({text, kind});
    if (lines_.size() > 600) {
        lines_.erase(lines_.begin(), lines_.begin() + static_cast<long>(lines_.size() - 600));
    }
    bump_version();
}

std::string Pm3Session::dumps_dir() const {
    const char* home = std::getenv("HOME");
    std::string base = (home && *home) ? std::string(home) : "/home/pi";
    return base + "/pm3-dumps";
}

std::string Pm3Session::save_log() {
    std::string dir = dumps_dir();
    if (!ensure_dir_recursive(dir)) {
        return {};
    }
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm_now {};
    localtime_r(&now, &tm_now);
    char fname[64];
    std::snprintf(fname, sizeof(fname), "pm3_%04d%02d%02d_%02d%02d%02d.log",
                  tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                  tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    std::string full_path = dir + "/" + fname;

    auto snap = lines_snapshot();
    std::ofstream ofs(full_path);
    if (!ofs) return {};
    for (const auto& l : snap) {
        ofs << l.text << '\n';
    }
    return full_path;
}

void Pm3Session::seed_demo_output(const std::string& cmd, const std::vector<OutputLine>& demo) {
    std::lock_guard<std::mutex> lk(mu_);
    active_cmd_ = cmd;
    lines_.clear();
    lines_.push_back({"$ " + cmd, LineKind::CMD});
    for (const auto& d : demo) {
        lines_.push_back(d);
    }
    last_cmd_status_.store(CmdStatus::OK);
    bump_version();
}

void Pm3Session::seed_demo_ble(const std::vector<BleDeviceInfo>& bles) {
    stop_bt_worker_.store(true);
    if (ble_worker_.joinable()) {
        ble_worker_.join();
    }
    ble_worker_busy_.store(false);
    std::lock_guard<std::mutex> lk(mu_);
    ble_devices_ = bles;
    bump_version();
}

}  // namespace cpz
