#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cpz {

enum class LineKind {
    NORMAL,
    SUCCESS,  // [+] or green ANSI
    WARN,     // [*]/[-] or yellow ANSI
    ERROR,    // [!] or red ANSI
    INFO,     // [=] or cyan ANSI
    HINT,     // [?]
    DEV,      // [#] device message
    CMD,      // echoed command
};

enum class CmdStatus {
    NONE,
    OK,
    WARN,
    ERR,
};

struct OutputLine {
    std::string text;
    LineKind kind = LineKind::NORMAL;
};

struct SerialPortInfo {
    std::string path;        // e.g. /dev/ttyACM0
    std::string label;       // e.g. ttyACM0 (Proxmark3)
    bool is_proxmark = false;
};

struct BleDeviceInfo {
    std::string mac;         // e.g. AA:BB:CC:DD:EE:FF
    std::string name;        // advertised/cached name
    bool is_proxmark = false;
    bool is_classic_bt = false;  // true -> bt:<MAC> (RDV4 BlueShark), false -> ble:<MAC> (PM5 BWM)
    bool is_paired = false;
    bool is_trusted = false;
};

class Pm3Session {
public:
    Pm3Session();
    ~Pm3Session();

    Pm3Session(const Pm3Session&) = delete;
    Pm3Session& operator=(const Pm3Session&) = delete;

    // Rescan /dev/ttyACM* and /dev/ttyUSB*, keeping selected_port valid or preserving ble:/bt:/tcp:.
    std::vector<SerialPortInfo> scan_ports();

    // Bluetooth (PM5 BLE + RDV4 BlueShark Classic BT) discovery & pairing via bluetoothctl
    std::vector<BleDeviceInfo> list_known_ble_devices();
    void refresh_ble_devices_async();
    bool start_ble_scan();
    bool is_ble_scanning() const { return ble_scanning_.load(); }
    bool start_bt_pair(const std::string& mac, const std::string& pin = "1234");
    bool is_bt_pairing() const { return bt_pairing_.load(); }
    bool last_bt_pair_ok() const { return bt_pair_ok_.load(); }
    std::string bt_pair_target() const;
    std::string bt_pair_status_msg() const;
    std::vector<BleDeviceInfo> ble_devices_snapshot() const;

    void set_selected_port(const std::string& port);  // "" = OFFLINE mode
    std::string selected_port() const;
    bool is_offline() const;
    static bool is_non_serial_port(const std::string& port);

    // Query/toggle Wi-Fi radio via nmcli (for Classic BT BlueShark coexistence on CM0 CYW4343A2)
    static bool is_wifi_enabled();
    static bool set_wifi_enabled(bool enable);
    void refresh_wifi_state_async();
    bool start_wifi_toggle(bool enable, bool clear_paused_flag = false);
    bool is_wifi_busy() const { return wifi_busy_.load(); }
    bool is_wifi_on_cached() const { return wifi_on_cached_.load(); }
    std::string wifi_status_msg() const;

    // Run a proxmark3 command asynchronously in a persistent interactive forkpty() session.
    bool run_command(const std::string& cmd);
    bool send_enter();
    bool request_stop();
    void abort();
    void close_session(bool restore_wifi = true);
    bool is_running() const { return running_.load(); }
    bool is_session_alive() const { return session_alive_.load(); }
    bool is_connected() const { return session_connected_.load(); }
    int last_exit_code() const { return last_exit_code_.load(); }
    CmdStatus last_cmd_status() const { return last_cmd_status_.load(); }
    uint64_t version() const { return version_.load(std::memory_order_acquire); }
    std::string active_command() const;

    // Snapshot lines for UI rendering.
    std::vector<OutputLine> lines_snapshot() const;
    void clear_output();
    void append_local_line(const std::string& text, LineKind kind = LineKind::INFO);

    // Persistent CLI command history (~/.config/czpm3/history.txt).
    std::vector<std::string> load_cli_history() const;
    void save_cli_history(const std::vector<std::string>& hist) const;

    // Save current output log to ~/pm3-dumps/ and return saved path (or empty on error).
    std::string save_log();
    std::string dumps_dir() const;

    // Seed sample state for headless screenshot preview.
    void seed_demo_output(const std::string& cmd, const std::vector<OutputLine>& demo);
    void seed_demo_ble(const std::vector<BleDeviceInfo>& bles);

private:
    enum class EscState { NONE, ESC, CSI, OSC };

    bool run_command_internal(const std::string& cmd, bool log_cmd);
    void bump_version() { version_.fetch_add(1, std::memory_order_release); }
    std::string find_pm3_binary() const;
    std::string config_dir() const;
    std::string config_file_path() const;
    std::string history_file_path() const;
    std::string wifi_paused_flag_path() const;
    void mark_wifi_paused_flag(bool paused) const;
    void load_port_config();
    void save_port_config(const std::string& val) const;
    void reader_loop(int master_fd, int pid);
    void process_raw_chunk(const char* data, size_t len);
    void finalize_current_line();
    void check_interactive_prompt();
    static bool parse_prompt_line(const std::string& s, bool* out_connected = nullptr);
    static LineKind parse_sgr_kind(const std::string& params, LineKind current);
    static LineKind classify_line(const std::string& clean, LineKind ansi_hint);
    static bool looks_like_proxmark(const std::string& s);
    static bool looks_like_blueshark(const std::string& s);

    mutable std::mutex mu_;
    std::string selected_port_;
    bool force_offline_ = false;
    std::string session_port_;
    std::string active_cmd_;
    std::string pending_cmd_;
    std::string last_sent_cmd_;
    bool first_prompt_seen_ = false;
    bool pending_cr_ = false;
    bool cmd_had_err_ = false;
    bool cmd_had_warn_ = false;

    std::vector<OutputLine> lines_;
    std::string cur_line_;
    LineKind cur_ansi_kind_ = LineKind::NORMAL;
    EscState esc_state_ = EscState::NONE;
    std::string csi_buf_;

    std::vector<BleDeviceInfo> ble_devices_;
    std::atomic<bool> ble_scanning_{false};
    std::atomic<bool> bt_pairing_{false};
    std::atomic<bool> bt_pair_ok_{false};
    std::atomic<bool> ble_worker_busy_{false};
    std::atomic<bool> stop_bt_worker_{false};
    std::string bt_pair_target_;
    std::string bt_pair_status_msg_;
    std::thread ble_worker_;

    std::atomic<bool> session_alive_{false};
    std::atomic<bool> session_connected_{false};
    std::atomic<bool> prompt_ready_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> bt_wifi_hint_needed_{false};
    std::atomic<bool> wifi_paused_for_bt_{false};
    std::atomic<bool> wifi_busy_{false};
    std::atomic<bool> wifi_on_cached_{true};
    std::string wifi_status_msg_;
    std::thread wifi_worker_;
    std::atomic<int> last_exit_code_{0};
    std::atomic<CmdStatus> last_cmd_status_{CmdStatus::NONE};
    std::atomic<uint64_t> version_{1};
    std::atomic<int> child_pid_{-1};
    int master_fd_ = -1;
    std::thread worker_;
};

}  // namespace cpz
