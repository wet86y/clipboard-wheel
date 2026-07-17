# 超级中键原生实现

此目录包含超级中键 2.x 的正式 C++20 实现。历史 .NET 8/WPF 基线保存在 Git 标签
`archive/net8-final`，不与当前源码混合维护。

## 本地构建

```powershell
.\native\scripts\build-native.ps1 -Configuration Release
.\native\scripts\run-native-self-check.ps1 -Configuration Release
```

脚本通过 Visual Studio Installer 定位 MSVC 和 CMake，不要求当前 PowerShell 已加载开发者环境。全部生成文件位于根目录 `build\native`。

## 运行与兼容边界

- 主程序使用 C++20、Win32、Direct2D/DirectWrite、WIC、OLE 和 C++/WinRT，不依赖 .NET 运行时。
- 原生构建使用现有 `SettingsVersion = 3`、应用数据目录、互斥体和自启动标识。
- `DesktopUpdateKit/native` 由根 CMake 直接引入，原生 Stub 以 RCDATA 嵌入单 EXE；`--verify-release`
  会离线验证资源、PE/x64 结构和区段边界。
- Release 与 `RelWithDebInfo` 的“关于”页均开放检查、下载、暂停、后台继续、节点切换和安装；
  Release 固定使用正式仓库，诊断配置可切换到隔离测试仓库。
- 默认版本集中由 CMake 的 `SMK_VERSION` 管理，当前正式原生目标为 `2.0.2`。诊断联网测试可用
  `--update-test-repository owner/repo` 隔离缓存和更新通道，Release 会拒绝该参数。
- 根目录 `scripts` 是正式构建、自检、资产准备和发布入口；`native/scripts` 只负责内部 CMake 构建与诊断。
