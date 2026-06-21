#pragma once

// ButterflyBlades 模块接口
// 所有功能模块必须实现此接口

#include "types.h"
#include <string>
#include <vector>
#include <functional>

namespace bb {

// 模块基类接口（纯虚接口，DLL/动态加载兼容）
class IModule {
public:
    virtual ~IModule() = default;

    // ---- 生命周期 ----
    virtual ErrorCode init() = 0;
    virtual ErrorCode start() = 0;
    virtual ErrorCode stop() = 0;
    virtual ErrorCode shutdown() = 0;

    // ---- 元信息 ----
    virtual ModuleDescriptor get_descriptor() const = 0;
    virtual std::string get_name() const = 0;

    // ---- 事件回调注册 ----
    virtual void set_event_callback(EventCallback callback) = 0;

    // ---- 状态查询 ----
    virtual bool is_running() const = 0;
    virtual bool is_initialized() const = 0;

    // ---- 配置 ----
    virtual ErrorCode load_config(const std::string& config_path) = 0;
    virtual ErrorCode save_config(const std::string& config_path) = 0;

    // ---- 远程指令处理 ----
    virtual ErrorCode handle_remote_command(const RemoteCommand& cmd, RemoteResponse& resp) = 0;
};

// C 风格导出工厂（用于动态加载）
using CreateModuleFunc = IModule* (*)();
using DestroyModuleFunc = void (*)(IModule*);

// 模块动态加载时需导出的函数:
// extern "C" __declspec(dllexport) bb::IModule* create_module();
// extern "C" __declspec(dllexport) void destroy_module(bb::IModule* mod);

} // namespace bb
