# 原生构建产物约定

项目只维护一套 C++20 源码和两个用户可见输出。首次构建前必须初始化
`shared\DesktopUpdateKit` 子模块。

| 类型 | 唯一目录 | 生成方式 | 内容 |
|---|---|---|---|
| CMake 中间文件 | `build\native` | `native\scripts\build-native.ps1` | 库、测试程序、生成资源和内部 EXE |
| 诊断运行版 | `build\bin\RelWithDebInfo` | `scripts\run-dev.ps1` | `超级中键.exe` 与匹配 PDB |
| 正式发布包 | `artifacts\超级中键-win-x64` | `scripts\build-release.ps1` | 仅 `超级中键.exe` |

`build\bin\Release` 是正式脚本生成的本地 Release 镜像，可用于自动化验证，不作为公开包。

## 强制约束

1. `build` 与 `artifacts` 均为可删除、可重建的生成目录，不得提交或手工维护。
2. 正式发布目录只能包含一个 `超级中键.exe`，不得附带 PDB、日志、测试程序或 Stub。
3. 不创建 `artifacts\*-debug-*`、第二套测试包或源码目录内的 `bin/obj`。
4. RelWithDebInfo 保持优化和真实动画时序；Release 编译掉高频诊断代码。
5. `scripts\run-self-check.ps1` 是仓库统一门禁，必须同时验证两种配置和正式包。

诊断日志写入 `%LOCALAPPDATA%\超级中键\logs`，不写在 EXE 同级目录。
