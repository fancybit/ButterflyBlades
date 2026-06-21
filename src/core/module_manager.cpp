// ButterflyBlades 模块管理器实现
// 负责模块的动态加载、卸载、生命周期管理

#include "core.h"
#include <iostream>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace bb {

ModuleManager& ModuleManager::instance() {
    static ModuleManager mgr;
    return mgr;
}

ModuleManager::~ModuleManager() {
    shutdown_all();
}

ErrorCode ModuleManager::load_module(const std::string& path) {
    std::unique_lock lock(mutex_);

#ifdef _WIN32
    void* handle = LoadLibraryA(path.c_str());
    if (!handle) {
        std::cerr << "[ModuleManager] Failed to load: " << path
                  << " (err=" << GetLastError() << ")" << std::endl;
        return ErrorCode::ERR_MODULE_LOAD_FAIL;
    }

    auto create_fn = (CreateModuleFunc)GetProcAddress((HMODULE)handle, "create_module");
    auto destroy_fn = (DestroyModuleFunc)GetProcAddress((HMODULE)handle, "destroy_module");
#else
    void* handle = dlopen(path.c_str(), RTLD_NOW);
    if (!handle) {
        std::cerr << "[ModuleManager] Failed to load: " << path
                  << " (" << dlerror() << ")" << std::endl;
        return ErrorCode::ERR_MODULE_LOAD_FAIL;
    }

    auto create_fn = (CreateModuleFunc)dlsym(handle, "create_module");
    auto destroy_fn = (DestroyModuleFunc)dlsym(handle, "destroy_module");
#endif

    if (!create_fn || !destroy_fn) {
        std::cerr << "[ModuleManager] Missing exports in: " << path << std::endl;
#ifdef _WIN32
        FreeLibrary((HMODULE)handle);
#else
        dlclose(handle);
#endif
        return ErrorCode::ERR_MODULE_LOAD_FAIL;
    }

    IModule* instance = create_fn();
    if (!instance) {
        std::cerr << "[ModuleManager] create_module returned null" << std::endl;
        return ErrorCode::ERR_MODULE_LOAD_FAIL;
    }

    auto desc = instance->get_descriptor();
    std::string name = desc.name;

    // 检查重复
    if (modules_.count(name)) {
        std::cerr << "[ModuleManager] Module already loaded: " << name << std::endl;
        destroy_fn(instance);
#ifdef _WIN32
        FreeLibrary((HMODULE)handle);
#else
        dlclose(handle);
#endif
        return ErrorCode::ERR_MODULE_LOAD_FAIL;
    }

    // 设置事件回调
    instance->set_event_callback([this](EventType type, const std::string& data) {
        fire_event(type, data);
    });

    ModuleEntry entry;
    entry.instance = std::unique_ptr<IModule, void(*)(IModule*)>(instance, destroy_fn);
    entry.handle = handle;
    entry.desc = desc;
    entry.desc.is_loaded = true;

    // 初始化模块
    ErrorCode ret = instance->init();
    if (ret != ErrorCode::SUCCESS) {
        std::cerr << "[ModuleManager] Module init failed: " << name << std::endl;
        return ret;
    }

    modules_[name] = std::move(entry);

    std::cout << "[ModuleManager] Loaded: " << name << " v" << desc.version << std::endl;
    fire_event(EventType::MODULE_LOADED, name);

    return ErrorCode::SUCCESS;
}

ErrorCode ModuleManager::unload_module(const std::string& name) {
    std::unique_lock lock(mutex_);

    auto it = modules_.find(name);
    if (it == modules_.end()) {
        return ErrorCode::ERR_MODULE_NOT_FOUND;
    }

    auto& entry = it->second;
    entry.instance->stop();
    entry.instance->shutdown();
    entry.instance.reset();

#ifdef _WIN32
    FreeLibrary((HMODULE)entry.handle);
#else
    dlclose(entry.handle);
#endif

    modules_.erase(it);

    std::cout << "[ModuleManager] Unloaded: " << name << std::endl;
    fire_event(EventType::MODULE_UNLOADED, name);

    return ErrorCode::SUCCESS;
}

IModule* ModuleManager::get_module(const std::string& name) {
    std::shared_lock lock(mutex_);
    auto it = modules_.find(name);
    if (it != modules_.end()) {
        return it->second.instance.get();
    }
    return nullptr;
}

std::vector<ModuleDescriptor> ModuleManager::list_modules() {
    std::shared_lock lock(mutex_);
    std::vector<ModuleDescriptor> result;
    for (auto& [name, entry] : modules_) {
        result.push_back(entry.desc);
    }
    // 按优先级排序
    std::sort(result.begin(), result.end(), [](const ModuleDescriptor& a, const ModuleDescriptor& b) {
        return a.priority < b.priority;
    });
    return result;
}

ErrorCode ModuleManager::start_all() {
    std::shared_lock lock(mutex_);
    for (auto& [name, entry] : modules_) {
        ErrorCode ret = entry.instance->start();
        if (ret != ErrorCode::SUCCESS) {
            std::cerr << "[ModuleManager] Start failed: " << name << std::endl;
            return ret;
        }
        entry.desc.enabled = true;
    }
    return ErrorCode::SUCCESS;
}

ErrorCode ModuleManager::stop_all() {
    std::shared_lock lock(mutex_);
    for (auto& [name, entry] : modules_) {
        entry.instance->stop();
        entry.desc.enabled = false;
    }
    return ErrorCode::SUCCESS;
}

ErrorCode ModuleManager::shutdown_all() {
    std::unique_lock lock(mutex_);
    for (auto& [name, entry] : modules_) {
        entry.instance->stop();
        entry.instance->shutdown();
        entry.instance.reset();
#ifdef _WIN32
        FreeLibrary((HMODULE)entry.handle);
#else
        dlclose(entry.handle);
#endif
    }
    modules_.clear();
    return ErrorCode::SUCCESS;
}

RemoteResponse ModuleManager::dispatch_command(const RemoteCommand& cmd) {
    RemoteResponse resp;
    resp.command_id = cmd.command_id;
    resp.code = ErrorCode::ERR_MODULE_NOT_FOUND;

    if (cmd.module == "*") {
        // 广播到所有模块
        std::shared_lock lock(mutex_);
        for (auto& [name, entry] : modules_) {
            RemoteResponse r;
            entry.instance->handle_remote_command(cmd, r);
            if (r.code == ErrorCode::SUCCESS) {
                resp = r;
            }
        }
    } else {
        auto* mod = get_module(cmd.module);
        if (mod) {
            resp.code = mod->handle_remote_command(cmd, resp);
        }
    }

    return resp;
}

void ModuleManager::set_global_event_callback(EventCallback callback) {
    std::unique_lock lock(mutex_);
    global_event_callback_ = std::move(callback);

    // 传播到已加载模块
    for (auto& [name, entry] : modules_) {
        entry.instance->set_event_callback(global_event_callback_);
    }
}

void ModuleManager::fire_event(EventType type, const std::string& data) {
    EventCallback cb;
    {
        std::shared_lock lock(mutex_);
        cb = global_event_callback_;
    }
    if (cb) {
        cb(type, data);
    }
}

// ========== ServiceManager ==========

ServiceManager& ServiceManager::instance() {
    static ServiceManager svc;
    return svc;
}

ServiceManager::~ServiceManager() {
    stop();
}

ErrorCode ServiceManager::init_core() {
    if (initialized_.load()) {
        return ErrorCode::ERR_ALREADY_INITIALIZED;
    }

    std::cout << "[ServiceManager] Initializing platform ops..." << std::endl;

    // 创建平台操作对象
    process_ops_ = platform::create_process_ops();
    window_ops_ = platform::create_window_ops();
    file_ops_ = platform::create_file_ops();
    driver_ops_ = platform::create_driver_ops();
    network_ops_ = platform::create_network_ops();
    system_ops_ = platform::create_system_ops();

    initialized_.store(true);
    std::cout << "[ServiceManager] Core initialized." << std::endl;

    return ErrorCode::SUCCESS;
}

ErrorCode ServiceManager::start() {
    if (!initialized_.load()) {
        return ErrorCode::ERR_NOT_INITIALIZED;
    }
    running_.store(true);
    std::cout << "[ServiceManager] Service started." << std::endl;
    return ErrorCode::SUCCESS;
}

ErrorCode ServiceManager::stop() {
    running_.store(false);
    std::cout << "[ServiceManager] Service stopped." << std::endl;
    return ErrorCode::SUCCESS;
}

bool ServiceManager::is_running() {
    return running_.load();
}

} // namespace bb
