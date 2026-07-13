# 程序内更新设计

## 当前范围

第一阶段使用公开 GitHub Releases 作为分发源，不依赖代码签名证书：

- 主程序从 `wet86y/clipboard-wheel` 的最新正式 Release 读取版本和更新说明。
- 更新检查、更新说明、下载、暂停/继续和取消全部位于“关于”页，不弹出更新确认窗口。
- GitHub Release 更新资产使用 ASCII 名称 `super-middle-key.exe` 和 `super-middle-key.exe.sha256`，避免中文文件名在不同上传客户端和 Windows 区域设置下被规范化。
- The application checks its own filename at startup. If a user launches the ASCII-named EXE directly, the shared NativeAOT UpdaterStub renames it to `超级中键.exe`, restarts it, and verifies the health marker; a protected directory leaves the current name unchanged so startup is not blocked.
- 主程序下载并校验 SHA-256 后，释放内嵌的 NativeAOT `UpdaterStub.exe`；Stub 不再携带第二份 .NET 运行库。
- Stub 等待主程序正常退出，备份旧 EXE、替换新版并重新启动。
- 新版启动后写入一次性健康标记；超时或启动失败时恢复旧 EXE。

## 资源边界

更新过程只允许修改：

- 当前程序所在目录中的 EXE 和临时备份。
- `%TEMP%\SuperMiddleKey-update\<transaction-id>`。

更新过程不得修改 `%AppData%\超级中键\settings.json`、旧配置迁移目录、剪贴板历史或自启动注册表项。

## 本地共享维护

更新客户端、UpdaterStub 和发布工具统一维护在独立的 `DesktopUpdateKit` 仓库，并通过
`shared\DesktopUpdateKit` Git submodule 固定到经过验证的提交。本项目通过
`ClipboardWheel.csproj` 的链接编译项和 `release.config.json` 接入共享工具；项目脚本
只保留薄包装入口，不复制共享实现。

本地构建、清单生成和普通测试不会访问 GitHub。只有显式运行
`scripts\publish-release.ps1` 时才上传 Release 资产；源码仓库推送与 Release 资产上传
是两个独立动作。

## 运行限制

主程序使用 `asInvoker` 运行。如果当前 EXE 所在目录不可写，更新会失败并提示用户将程序移动到用户可写目录；第一阶段不自动请求管理员权限。

The Basic Settings page provides an optional `runas` restart for compatibility with elevated target applications. It does not change the manifest or default startup; the elevated instance waits for the original single-instance mutex to be released before taking over.

正式发布版才会嵌入 UpdaterStub。普通 `dotnet build` 调试版如果没有传入 `UpdaterStubPath`，点击检查更新时会提示该构建未包含 Stub。

正式发布版启用 .NET 单文件压缩，以降低 GitHub 下载体积；首次启动会有少量解压开销。更新完成后的健康检查超时仍由 Stub 自动回滚。

下载阶段没有全局 100 秒 HTTP 超时；用户可在“关于”页暂停、继续或取消。暂停会保留当前 HTTPS 连接，取消会终止请求并清理本次临时下载目录。大于 16 MiB 且确认支持 HTTP Range 的完整 EXE，会由共享 `DesktopUpdateKit` 使用最多 4 个连接分块下载；不支持 Range 或分块响应不正确时自动退回单连接。应用页只显示共享组件提供的进度，不包含下载算法。

## 下载源与增量更新

当前下载源是 GitHub Releases，低速网络的速度主要取决于用户到 GitHub 发布资产的链路。客户端不会自动使用代理或不受信任镜像。

完整 EXE 下载支持由共享组件管理的 GitHub 下载节点。`update.json` 可提供统一的 `downloadNodes` 列表，客户端按“上次成功节点、节点 priority、GitHub 官方”顺序用 64 KiB Range 请求探测。节点只改变 EXE 的传输路径；清单、固定 Tag 地址、大小以及 SHA-256 校验逻辑不由第三方节点决定。所有节点失败时自动回退 GitHub 官方直链。

下载任务不再依附设置窗口。关闭设置页时默认自动暂停；“转入后台下载”会让任务继续运行，完成后在设置页显示“立即安装”，不会未经确认退出主程序。速度显示采用两秒滑动窗口，并显示节点 ID 与单路/多路连接状态。

“关于”页提供“使用加速节点”开关，默认开启。关闭后，当前或后续下载均使用 GitHub 官方直连；开关开启且下载进行中时可使用“切换加速节点”按钮，强制跳到下一个加速节点，不取消整个更新任务。

下一阶段可在固定、受控的镜像域名上提供完整 EXE 备用下载源，并在客户端配置允许的主机白名单。增量更新需要额外维护“基准版本 SHA-256、补丁 SHA-256、目标 SHA-256、补丁格式版本和完整 EXE 回退地址”。由于压缩单文件 EXE 的二进制差异可能较大，必须先以多个真实版本测量补丁比例后再决定是否启用。

## 后续增强

- Windows Authenticode 代码签名。
- GitHub Actions 自动构建和发布。
- 增量更新和断点续传。
- 更细粒度的更新日志和失败诊断。
