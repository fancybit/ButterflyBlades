/**
 * ButterflyBlades 远程控制台客户端
 * 连接到远程主机上运行的 ButterflyBlades 实例，执行模块管理和 Shell 命令
 *
 * 用法:
 *   bb_console.exe <host> <port> [--auth Token] [--shell cmd|ps|bash]
 *
 * 示例:
 *   bb_console.exe 192.168.1.100 4444 --auth my_secret_token --shell cmd
 */

#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <thread>
#include <atomic>
#include <chrono>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define SOCKET_ERROR_CODE SOCKET_ERROR
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <cstring>
#define SOCKET int
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR_CODE (-1)
#define closesocket close
#endif

#include <nlohmann/json.hpp>

class ConsoleClient {
private:
    SOCKET sock_ = INVALID_SOCKET;
    std::string host_;
    uint16_t port_;
    std::string auth_token_;
    std::string shell_type_;
    std::string prompt_;
    uint32_t cmd_id_ = 0;

    std::string build_msg(const nlohmann::json& data) {
        auto payload = data;
        payload["auth_token"] = auth_token_;
        return "BB_PROTO_V1|" + payload.dump() + "\n";
    }

    nlohmann::json send_and_recv(const nlohmann::json& data) {
        std::string msg = build_msg(data);

        // 发送
        int sent = ::send(sock_, msg.c_str(), (int)msg.length(), 0);
        if (sent <= 0) {
            throw std::runtime_error("Send failed");
        }

        // 接收
        char buf[65536];
        int recvd = ::recv(sock_, buf, sizeof(buf) - 1, 0);
        if (recvd <= 0) {
            throw std::runtime_error("Recv failed or connection closed");
        }
        buf[recvd] = 0;

        // 可能会收到多条消息（\n 分割），取第一条完整 JSON
        std::string raw(buf);
        size_t sep = raw.find("BB_PROTO_V1|");
        if (sep == std::string::npos) {
            throw std::runtime_error("Invalid protocol response");
        }

        sep += strlen("BB_PROTO_V1|");
        size_t end = raw.find('\n', sep);
        std::string json_str = (end != std::string::npos)
            ? raw.substr(sep, end - sep)
            : raw.substr(sep);

        return nlohmann::json::parse(json_str);
    }

public:
    ConsoleClient(const std::string& host, uint16_t port,
                  const std::string& token, const std::string& shell)
        : host_(host), port_(port), auth_token_(token), shell_type_(shell) {}

    ~ConsoleClient() { disconnect(); }

    bool connect() {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

        sock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_ == INVALID_SOCKET) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        inet_pton(AF_INET, host_.c_str(), &addr.sin_addr);

        if (::connect(sock_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR_CODE) {
            std::cerr << "Connection failed: " << host_ << ":" << port_ << std::endl;
            return false;
        }

        // 测试 ping
        try {
            nlohmann::json ping;
            ping["action"] = "ping";
            auto resp = send_and_recv(ping);
            if (resp["action"] == "pong") {
                prompt_ = "BB@" + resp.value("hostname", "unknown") + "> ";
                std::cout << "Connected to " << resp.value("version", "?")
                          << " on " << resp.value("hostname", "?") << std::endl;
                return true;
            }
        } catch (const std::exception& e) {
            std::cerr << "Auth failed: " << e.what() << std::endl;
        }
        return false;
    }

    void disconnect() {
        if (sock_ != INVALID_SOCKET) {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
#ifdef _WIN32
        WSACleanup();
#endif
    }

    void run() {
        if (!connect()) {
            std::cerr << "Failed to establish connection." << std::endl;
            return;
        }

        std::cout << "\nButterflyBlades Remote Console" << std::endl;
        std::cout << "Type 'help' for commands, 'exit' to quit.\n" << std::endl;

        std::string line;
        while (true) {
            std::cout << prompt_;
            if (!std::getline(std::cin, line)) break;

            if (line.empty()) continue;
            if (line == "exit" || line == "quit") break;

            process_command(line);
        }
    }

private:
    void process_command(const std::string& input) {
        std::istringstream iss(input);
        std::string cmd;
        iss >> cmd;

        try {
            if (cmd == "help") {
                show_help();
            }
            else if (cmd == "ping") {
                nlohmann::json j;
                j["action"] = "ping";
                auto r = send_and_recv(j);
                std::cout << "[PONG] " << r.dump(2) << std::endl;
            }
            else if (cmd == "info") {
                nlohmann::json j;
                j["action"] = "get_system_info";
                auto r = send_and_recv(j);
                std::cout << "[SYSTEM INFO]" << std::endl;
                std::cout << "  Hostname:    " << r.value("hostname", "?") << std::endl;
                std::cout << "  OS:          " << r.value("os", "?") << std::endl;
                std::cout << "  User:        " << r.value("user", "?") << std::endl;
                std::cout << "  Memory:      " << r.value("total_memory_mb", 0) << " MB" << std::endl;
                std::cout << "  BB Version:  " << r.value("bb_version", "?") << std::endl;
                std::cout << "  Modules:     " << r.value("loaded_modules", 0) << " loaded" << std::endl;
            }
            else if (cmd == "modules" || cmd == "lsmod") {
                nlohmann::json j;
                j["action"] = "list_modules";
                auto r = send_and_recv(j);
                std::cout << "[MODULES] Total: " << r["total"] << std::endl;
                for (auto& m : r["modules"]) {
                    std::cout << "  " << m["name"] << " v" << m["version"]
                              << (m["enabled"] ? " [ENABLED]" : " [DISABLED]")
                              << (m["is_loaded"] ? " [LOADED]" : "") << std::endl;
                    std::cout << "    " << m["description"] << std::endl;
                }
            }
            else if (cmd == "start") {
                std::string mod;
                iss >> mod;
                nlohmann::json j;
                j["action"] = "start_module";
                j["module"] = mod;
                auto r = send_and_recv(j);
                std::cout << "[START] Module '" << mod << "': " << r["status"] << std::endl;
            }
            else if (cmd == "stop") {
                std::string mod;
                iss >> mod;
                nlohmann::json j;
                j["action"] = "stop_module";
                j["module"] = mod;
                auto r = send_and_recv(j);
                std::cout << "[STOP] Module '" << mod << "': " << r["status"] << std::endl;
            }
            else if (cmd == "load") {
                std::string path;
                iss >> path;
                nlohmann::json j;
                j["action"] = "load_module";
                j["path"] = path;
                auto r = send_and_recv(j);
                std::cout << "[LOAD] " << r["status"] << std::endl;
            }
            else if (cmd == "unload") {
                std::string mod;
                iss >> mod;
                nlohmann::json j;
                j["action"] = "unload_module";
                j["module"] = mod;
                auto r = send_and_recv(j);
                std::cout << "[UNLOAD] Module '" << mod << "': " << r["status"] << std::endl;
            }
            else if (cmd == "exec" || cmd == "!") {
                // 获取输入行中 exec 后面的所有内容作为命令
                std::string exec_cmd;
                if (cmd == "exec") {
                    std::getline(iss, exec_cmd);
                    if (!exec_cmd.empty() && exec_cmd[0] == ' ') exec_cmd = exec_cmd.substr(1);
                } else {
                    exec_cmd = input.substr(1); // ! 后面的所有内容
                }
                if (!exec_cmd.empty()) {
                    nlohmann::json j;
                    j["action"] = "exec_command";
                    j["command"] = exec_cmd;
                    j["shell"] = shell_type_;
                    auto r = send_and_recv(j);
                    std::cout << r["output"] << std::endl;
                }
            }
            else if (cmd == "dispatch") {
                std::string mod, act, payload;
                iss >> mod >> act;
                std::getline(iss, payload);
                if (!payload.empty() && payload[0] == ' ') payload = payload.substr(1);

                nlohmann::json j;
                j["action"] = "dispatch";
                j["module"] = mod;
                j["cmd_action"] = act;
                j["payload"] = payload;
                auto r = send_and_recv(j);
                std::cout << "[DISPATCH] Code: " << r["code"] << std::endl;
                std::cout << "  Data: " << r.value("data", "") << std::endl;
            }
            else if (cmd == "shell") {
                std::string sh;
                iss >> sh;
                if (!sh.empty()) {
                    shell_type_ = sh;
                    std::cout << "[SHELL] Switched to: " << sh << std::endl;
                } else {
                    std::cout << "[SHELL] Current: " << shell_type_ << std::endl;
                }
            }
            else {
                std::cerr << "Unknown command: " << cmd << ". Type 'help' for available commands." << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] " << e.what() << std::endl;
        }
    }

    void show_help() {
        std::cout << R"(
ButterflyBlades Console Commands:
=================================
  help                              Show this help
  ping                              Test connection
  info                              Show system information
  modules / lsmod                   List loaded modules
  start <module>                    Start a module
  stop <module>                     Stop a module
  load <path>                       Load a module DLL/SO
  unload <module>                   Unload a module
  exec <command>                    Execute shell command remotely
  !<command>                        Shortcut for exec
  dispatch <module> <action> [json] Dispatch command to module
  shell [cmd|ps|bash|py]           Get/Set active shell type
  exit / quit                       Disconnect and exit
)" << std::endl;
    }
};

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cout << "ButterflyBlades Remote Console Client" << std::endl;
        std::cout << "Usage: bb_console <host> <port> [--auth Token] [--shell cmd|ps|bash]\n" << std::endl;
        std::cout << "Example:" << std::endl;
        std::cout << "  bb_console 192.168.1.100 4444 --auth my_token --shell cmd" << std::endl;
        return 1;
    }

    std::string host = argv[1];
    uint16_t port = (uint16_t)std::stoi(argv[2]);
    std::string token;
    std::string shell = "cmd";

    for (int i = 3; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--auth" && i + 1 < argc) {
            token = argv[++i];
        } else if (arg == "--shell" && i + 1 < argc) {
            shell = argv[++i];
        }
    }

    ConsoleClient client(host, port, token, shell);
    client.run();
    return 0;
}
