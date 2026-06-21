#pragma once

// ButterflyBlades 核心框架接口

#include "types.h"
#include "module_interface.h"
#include "platform.h"
#include <memory>
#include <vector>
#include <unordered_map>
#include <shared_mutex>

namespace bb {

// 模块管理器 - 负责动态加载/卸载/管理所有模块
class ModuleManager {
public:
    static ModuleManager& instance();

    // 从 DLL/SO 加载模块
    ErrorCode load_module(const std::string& path);

    // 卸载模块
    ErrorCode unload_module(const std::string& name);

    // 获取模块
    IModule* get_module(const std::string& name);

    // 列出所有模块
    std::vector<ModuleDescriptor> list_modules();

    // 启动所有已加载模块
    ErrorCode start_all();

    // 停止所有模块
    ErrorCode stop_all();

    // 关闭所有模块并卸载
    ErrorCode shutdown_all();

    // 广播远程指令到指定模块
    RemoteResponse dispatch_command(const RemoteCommand& cmd);

    // 设置全局事件回调
    void set_global_event_callback(EventCallback callback);

    // 触发事件（由模块调用）
    void fire_event(EventType type, const std::string& data);

private:
    ModuleManager() = default;
    ~ModuleManager();

    struct ModuleEntry {
        std::unique_ptr<IModule, void(*)(IModule*)> instance{nullptr, nullptr};
        void* handle = nullptr;   // DLL/SO handle
        ModuleDescriptor desc{};
    };

    std::shared_mutex mutex_;
    std::unordered_map<std::string, ModuleEntry> modules_;
    EventCallback global_event_callback_;
};

// 服务管理器 - 核心服务生命周期管理
class ServiceManager {
public:
    static ServiceManager& instance();

    // 初始化核心服务
    ErrorCode init_core();

    // 启动服务
    ErrorCode start();

    // 停止服务
    ErrorCode stop();

    // 是否运行中
    bool is_running();

    // 获取平台操作对象
    platform::ProcessOps* process_ops() { return process_ops_.get(); }
    platform::WindowOps* window_ops() { return window_ops_.get(); }
    platform::FileOps* file_ops() { return file_ops_.get(); }
    platform::DriverOps* driver_ops() { return driver_ops_.get(); }
    platform::NetworkOps* network_ops() { return network_ops_.get(); }
    platform::SystemOps* system_ops() { return system_ops_.get(); }

private:
    ServiceManager() = default;
    ~ServiceManager();

    std::unique_ptr<platform::ProcessOps> process_ops_;
    std::unique_ptr<platform::WindowOps> window_ops_;
    std::unique_ptr<platform::FileOps> file_ops_;
    std::unique_ptr<platform::DriverOps> driver_ops_;
    std::unique_ptr<platform::NetworkOps> network_ops_;
    std::unique_ptr<platform::SystemOps> system_ops_;
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
};

} // namespace bb
