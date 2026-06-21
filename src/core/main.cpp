// ButterflyBlades 安全软件 - 主入口
// Windows PC 版本

#include "../include/core/core.h"
#include <iostream>
#include <csignal>
#include <string>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace bb;

// 全局退出标志
std::atomic<bool> g_exit_flag{false};

// 信号处理
void signal_handler(int sig) {
    std::cout << "\n[ButterflyBlades] Received signal " << sig << ", shutting down..." << std::endl;
    g_exit_flag.store(true);
}

#ifdef _WIN32
BOOL WINAPI console_handler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT ||
        ctrlType == CTRL_CLOSE_EVENT) {
        std::cout << "\n[ButterflyBlades] Console event " << ctrlType << ", shutting down..." << std::endl;
        g_exit_flag.store(true);
        return TRUE;
    }
    return FALSE;
}
#endif

void print_banner() {
    std::cout << R"(
 ____        _   _             __ _       ____  _           _           
| __ ) _   _| |_| |_ ___ _ __ / _| |_   _| __ )| | __ _  __| | ___  ___ 
|  _ \| | | | __| __/ _ \ '__| |_| | | | |  _ \| |/ _` |/ _` |/ _ \/ __|
| |_) | |_| | |_| ||  __/ |  |  _| | |_| | |_) | | (_| | (_| |  __/\__ \
|____/ \__,_|\__|\__\___|_|  |_| |_|\__, |____/|_|\__,_|\__,_|\___||___/
                                    |___/                               

    ButterflyBlades Security Suite v)" << BB_VERSION_STR << R"(
    Cross-platform security solution
    ========================================
    )" << std::endl;
}

bool create_default_dirs() {
    std::string config_dir = DEFAULT_CONFIG_DIR;
    std::string modules_dir = config_dir + "\\modules";
    std::string logs_dir = config_dir + "\\logs";
    std::string db_dir = config_dir + "\\db";

    try {
        std::filesystem::create_directories(config_dir);
        std::filesystem::create_directories(modules_dir);
        std::filesystem::create_directories(logs_dir);
        std::filesystem::create_directories(db_dir);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to create directories: " << e.what() << std::endl;
        return false;
    }
}

int main(int argc, char* argv[]) {
    print_banner();

    // 设置信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
#endif

    // 创建默认目录
    if (!create_default_dirs()) {
        std::cerr << "[FATAL] Cannot create required directories." << std::endl;
        return 1;
    }

    // 初始化核心服务
    std::cout << "[INIT] Initializing core services..." << std::endl;
    auto& svc = ServiceManager::instance();
    ErrorCode ret = svc.init_core();
    if (ret != ErrorCode::SUCCESS) {
        std::cerr << "[FATAL] Core initialization failed: " << (int)ret << std::endl;
        return 2;
    }

    // 注册全局事件回调
    auto& mgr = ModuleManager::instance();
    mgr.set_global_event_callback([](EventType type, const std::string& data) {
        std::string type_name;
        switch (type) {
            case EventType::PROCESS_HIGH_CPU: type_name = "PROCESS_HIGH_CPU"; break;
            case EventType::PROCESS_HIGH_MEMORY: type_name = "PROCESS_HIGH_MEMORY"; break;
            case EventType::PROCESS_HIGH_GPU: type_name = "PROCESS_HIGH_GPU"; break;
            case EventType::PROCESS_KILLED: type_name = "PROCESS_KILLED"; break;
            case EventType::THREAD_STUCK: type_name = "THREAD_STUCK"; break;
            case EventType::INTEGRITY_VIOLATION: type_name = "INTEGRITY_VIOLATION"; break;
            case EventType::VIRUS_DETECTED: type_name = "VIRUS_DETECTED"; break;
            case EventType::POPUP_DETECTED: type_name = "POPUP_DETECTED"; break;
            case EventType::POPUP_BLOCKED: type_name = "POPUP_BLOCKED"; break;
            case EventType::SELF_PROTECT_TRIGGERED: type_name = "SELF_PROTECT_TRIGGERED"; break;
            case EventType::MODULE_LOADED: type_name = "MODULE_LOADED"; break;
            case EventType::MODULE_UNLOADED: type_name = "MODULE_UNLOADED"; break;
            default: type_name = "UNKNOWN"; break;
        }
        std::cout << "[EVENT] " << type_name << ": " << data << std::endl;
    });

    // 解析命令行参数，确定要加载的模块
    std::vector<std::string> modules_to_load = {
        "self_protect",
        "integrity_check",
        "process_monitor",
        "thread_monitor",
        "virus_scan",
        "popup_blocker",
        "remote_console"
    };

    // 可以通过命令行参数控制
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--no-self-protect") {
            modules_to_load.erase(
                std::remove(modules_to_load.begin(), modules_to_load.end(), "self_protect"),
                modules_to_load.end());
        } else if (arg == "--no-remote") {
            modules_to_load.erase(
                std::remove(modules_to_load.begin(), modules_to_load.end(), "remote_console"),
                modules_to_load.end());
        } else if (arg == "--port" && i + 1 < argc) {
            // 端口设置（此处简化）
            i++;
        }
    }

    // 启动服务
    ret = svc.start();
    if (ret != ErrorCode::SUCCESS) {
        std::cerr << "[FATAL] Service start failed: " << (int)ret << std::endl;
        return 3;
    }

    // 加载并启动所有模块
    for (auto& mod_name : modules_to_load) {
        std::string module_path;
#ifdef _WIN32
        module_path = std::string(DEFAULT_CONFIG_DIR) + "\\modules\\bb_" + mod_name + ".dll";
#else
        module_path = std::string(DEFAULT_CONFIG_DIR) + "/modules/bb_" + mod_name + ".so";
#endif

        // 如果是内置模块（当前编译在一起），直接创建实例
        // 这里演示模块加载流程，实际使用通过 DLL 加载
        std::cout << "[LOAD] Attempting to load module: " << mod_name << std::endl;

        // 在实际部署中，各模块编译为独立 DLL/SO
        // ErrorCode loadRet = mgr.load_module(module_path);
        // if (loadRet != ErrorCode::SUCCESS) {
        //     std::cerr << "[WARN] Skipped module: " << mod_name << std::endl;
        // }
    }

    std::cout << "[START] All modules loaded. ButterflyBlades is running." << std::endl;
    std::cout << "[INFO] Press Ctrl+C to exit." << std::endl;

    // 主循环
    while (!g_exit_flag.load() && svc.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // 优雅关闭
    std::cout << "\n[SHUTDOWN] Stopping all modules..." << std::endl;
    mgr.stop_all();
    mgr.shutdown_all();

    std::cout << "[SHUTDOWN] Stopping core services..." << std::endl;
    svc.stop();

    std::cout << "[SHUTDOWN] ButterflyBlades terminated." << std::endl;
    return 0;
}
