# ButterflyBlades (蝴蝶双刀) 安全软件

跨平台安全解决方案，覆盖 Windows / Linux / macOS 操作系统。

## 项目概述

ButterflyBlades 是一套模块化架构的安全软件，具备自我保护、进程监控、病毒扫描、弹窗拦截、远程管理等功能。项目命名灵感来源于中国传统兵器"蝴蝶双刀"——防御与反击兼备。

## 架构设计

```
ButterflyBlades/
├── include/                    # 公共头文件
│   ├── common/
│   │   ├── types.h            # 核心类型定义
│   │   └── platform.h         # 平台抽象层接口
│   └── core/
│       ├── core.h             # 核心框架（ModuleManager / ServiceManager）
│       └── module_interface.h # IModule 接口定义
├── src/
│   ├── core/
│   │   ├── main.cpp           # 主入口
│   │   └── module_manager.cpp # 模块管理器实现
│   ├── modules/
│   │   ├── self_protect/      # 自我保护模块
│   │   ├── process_monitor/   # 进程监控模块
│   │   ├── thread_monitor/    # 线程卡住检测模块
│   │   ├── integrity_check/   # 完整性检测模块
│   │   ├── virus_scan/        # 病毒扫描模块
│   │   ├── popup_blocker/     # 弹窗拦截模块
│   │   └── remote_console/    # 远程控制台模块
│   └── platform/
│       ├── windows/           # Windows 平台实现
│       └── linux/             # Linux 平台实现
├── console/                    # 远程控制台客户端
├── config/                     # 默认配置
├── scripts/                    # 工具脚本
├── driver/                     # 内核驱动（规划中）
├── test/                       # 单元测试
└── CMakeLists.txt             # 构建配置
```

## 核心能力

### 1. 自我保护模块 (`self_protect`)
- 防止自身进程被终止（RtlSetProcessIsCritical）
- 反调试检测（IsDebuggerPresent / CheckRemoteDebuggerPresent）
- 关键文件完整性自检
- 可配置的受保护文件列表

### 2. 进程监控模块 (`process_monitor`)
- CPU / 内存 / GPU 使用率实时监控
- 超阈值告警与记录
- 可配置的自动终止（含宽限期）
- 进程白名单排除

### 3. 线程卡住检测 (`thread_monitor`)
- 基于 CPU 时间快照的卡住判定
- 可配置的卡住阈值
- 实时告警推送

### 4. 完整性检测 (`integrity_check`)
- 文件 SHA256 哈希监控
- 篡改/增删检测
- 定时自动检查
- 可扩展监控文件列表

### 5. 病毒扫描 (`virus_scan`)
- 双病毒库（公开库 + 自定义库）
- 签名匹配扫描
- 路径 + 哈希双通道白名单
- 后台队列异步扫描
- 远程增删签名

### 6. 弹窗拦截 (`popup_blocker`)
- 基于正则的表达规则匹配（标题 / 类名 / 进程名）
- 去重拦截机制
- 内置广告弹窗特征规则库
- 白名单窗口管理

### 7. 远程控制台 (`remote_console`)
- 基于 TCP 的自定义协议（BB_PROTO_V1）
- 支持 cmd / PowerShell / Bash / Python
- 模块热加载与卸载
- 远程指令分发
- Token 认证安全
- 心跳保持

## 编译构建

### 依赖

- CMake >= 3.20
- C++20 编译器（MSVC 2022+ / GCC 11+ / Clang 14+）
- nlohmann/json（自动 FetchContent 获取）

### Windows

```powershell
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64 -DBB_STATIC_MODULES=ON
cmake --build . --config Release
```

### Linux

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DBB_STATIC_MODULES=ON
make -j$(nproc)
```

### macOS

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DBB_STATIC_MODULES=ON
make -j$(sysctl -n hw.ncpu)
```

## 运行

```bash
# 完整启动（所有模块）
./ButterflyBlades

# 不加载自我保护模块
./ButterflyBlades --no-self-protect

# 不启动远程控制台
./ButterflyBlades --no-remote
```

## 远程控制

```bash
# 连接到远程 ButterflyBlades 实例
bb_console.exe 192.168.1.100 4444 --auth your_token --shell cmd

# 控制台命令
help              # 显示帮助
info              # 系统信息
modules           # 列出所有模块
start <module>    # 启动模块
stop <module>     # 停止模块
load <path>       # 动态加载模块
unload <module>   # 卸载模块
exec <command>    # 执行远程 Shell 命令
!<command>        # 快捷 Shell 执行
shell <type>      # 切换 Shell 类型
```

## 协议格式

客户端与远程控制台之间使用自定义协议通信：

```
BB_PROTO_V1|{"action":"ping","auth_token":"xxx"}\n
```

## 扩展开发

### 添加新模块

1. 在 `src/modules/` 下创建新目录
2. 实现 `IModule` 接口（参考 `include/core/module_interface.h`）
3. 导出 `create_module()` 和 `destroy_module()` 工厂函数
4. 在 `CMakeLists.txt` 中注册模块

```cpp
// 最小模块示例
class MyModule : public bb::IModule {
    // 实现所有纯虚函数...
};

extern "C" bb::IModule* create_module() { return new MyModule(); }
extern "C" void destroy_module(bb::IModule* m) { delete m; }
```

### 添加新平台

1. 在 `src/platform/` 下创建新目录
2. 实现所有 6 个平台抽象层（ProcessOps / WindowOps / FileOps / DriverOps / NetworkOps / SystemOps）
3. 提供工厂函数（`create_process_ops()` 等）
4. 在 `CMakeLists.txt` 中添加条件编译分支

## 安全注意事项

> **警告**：远程控制台模块默认配置了简单的 Token 认证，生产环境务必修改默认 Token。建议启用 TLS 加密通信。

## 路线图

| 阶段 | 目标 |
|------|------|
| v0.1 | Windows 基础功能 + 7 大模块 |
| v0.2 | Linux 完整实现 + macOS 适配 |
| v0.3 | Windows 内核驱动 + 驱动层检测 |
| v0.5 | Web 管理面板 + REST API |
| v0.8 | iOS / Android 轻量客户端 |
| v1.0 | HarmonyOS NEXT 支持 + 全平台发布 |

## 许可证

项目代码仅供学习与研究目的使用。
