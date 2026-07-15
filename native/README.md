# 超级中键原生迁移工程

此目录是与现有 .NET 8/WPF 主程序并行的 C++ 重构工作区。迁移完成前，原生工程不得替换现有发布入口、CI 或 Release 资产。

## 本地构建

```powershell
.\native\scripts\build-native.ps1 -Configuration Release
.\native\scripts\run-native-self-check.ps1 -Configuration Release
```

脚本通过 Visual Studio Installer 定位 MSVC 和 CMake，不要求当前 PowerShell 已加载开发者环境。全部生成文件位于根目录 `build\native`。

## 当前迁移边界

- 已建立 C++20/Win32/Direct2D/C++/WinRT 工程、核心行为测试和原生更新协调器。
- 原生构建使用现有 `SettingsVersion = 3`、应用数据目录、互斥体和自启动标识。
- `DesktopUpdateKit/native` 由根 CMake 直接引入，原生 Stub 以 RCDATA 嵌入单 EXE；`--verify-release`
  会离线验证资源、PE/x64 结构和区段边界。
- Release 与 `RelWithDebInfo` 的“关于”页均开放检查、下载、暂停、后台继续、节点切换和安装；
  Release 固定使用正式仓库，诊断配置可切换到隔离测试仓库。
- 默认版本集中由 CMake 的 `SMK_VERSION` 管理，当前正式原生目标为 `2.0.0`。诊断联网测试可用
  `--update-test-repository owner/repo` 隔离缓存和更新通道，Release 会拒绝该参数。
- 正式发布仍由现有 .NET 项目和仓库根目录脚本负责。
