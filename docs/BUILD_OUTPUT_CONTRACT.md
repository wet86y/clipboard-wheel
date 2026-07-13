# 构建产物维护约定

本项目只有两类可运行产物。它们都从 `Release` 配置生成，但用途、日志行为和维护位置不同；不得以目录名称中的 `Release` 或 `Debug` 推断其日志策略。

`Directory.Build.props` 将所有编译输出集中到仓库根目录 `build`，因此源码目录下不应再出现 `bin` 或 `obj`。构建前必须已初始化 `shared\DesktopUpdateKit` 子模块。

| 类型 | 唯一维护目录 | 生成方式 | 日志策略 | 用途 |
|---|---|---|---|---|
| 调试运行版 | `build\bin\Release` | `dotnet build .\src\ClipboardWheel\ClipboardWheel.csproj -c Release /p:PasteTraceNextToExecutable=true` | 生成同级 `paste-trace.log` | 本机功能验证与日志排查 |
| 正式打包版 | `artifacts\超级中键-win-x64` | `scripts\build-release.ps1` | 不生成日志 | 对外分发 |

正式打包脚本会先在 `build\updater\win-x64` 生成通用 `UpdaterStub.exe`，再将其嵌入正式单文件 EXE；该目录是构建中间产物，不属于对外分发包。

## 强制约束

1. 调试只维护 `build\bin\Release`，不要创建 `build\bin\Debug` 输出，也不要创建调试分发目录。
2. 正式包只维护 `artifacts\超级中键-win-x64`，目录中只应包含 `超级中键.exe`；不得保留 `.pdb`、`paste-trace.log` 或其他运行产物。
3. 不得创建、更新或交付 `artifacts\超级中键-debug-win-x64`（或任何 `artifacts\*-debug-*`）目录。
4. `build/` 和 `artifacts/` 均为构建产物；只能通过构建命令或发布脚本更新，不能手工编辑。
5. 完成功能修改后，先更新调试运行版并测试；只有需要对外分发时才重新运行正式打包脚本。
6. 清理时可以删除所有 `Debug` 目录和未被上述两类产物使用的临时发布目录；不得误删两个约定目录。

## 验证

调试版运行后，日志位置固定为：

```powershell
Get-Content .\build\bin\Release\paste-trace.log -Tail 200
```

正式包必须确认未生成 `paste-trace.log`。日志开关由 `PasteTraceEnabled` 构建属性控制：调试构建保持开启，`scripts\build-release.ps1` 明确传入 `false`。
