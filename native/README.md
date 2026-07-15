# 超级中键原生迁移工程

此目录是与现有 .NET 8/WPF 主程序并行的 C++ 重构工作区。迁移完成前，原生工程不得替换现有发布入口、CI 或 Release 资产。

## 本地构建

```powershell
.\native\scripts\build-native.ps1 -Configuration Release
.\native\scripts\run-native-self-check.ps1 -Configuration Release
```

脚本通过 Visual Studio Installer 定位 MSVC 和 CMake，不要求当前 PowerShell 已加载开发者环境。全部生成文件位于根目录 `build\native`。

## 当前迁移边界

- 已建立 C++20/Win32/Direct2D/C++/WinRT 工程和核心行为测试。
- 原生构建使用现有 `SettingsVersion = 3`、应用数据目录、互斥体和自启动标识。
- 更新下载器仍处于禁用状态；原生构建不允许通过 `--verify-release`，不能作为正式 Release。
- 正式发布仍由现有 .NET 项目和仓库根目录脚本负责。
