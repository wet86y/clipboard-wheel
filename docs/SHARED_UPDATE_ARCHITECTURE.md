# DesktopUpdateKit 子模块架构

`shared\DesktopUpdateKit` 是独立 Git 仓库。主仓库只记录经过验证的提交 SHA，不复制更新实现，
也不自动跟随远程分支。

## 原生接入

- 根 CMake 通过 `add_subdirectory` 引入 `DesktopUpdateKit/native` 静态库。
- 原生 Stub 作为 RCDATA 嵌入 `超级中键.exe`；`--verify-release` 离线检查其 PE/x64 结构。
- `NativeUpdateCoordinator` 持有检查、下载、暂停、继续、取消、节点切换和安装会话。
- `release.config.json` 保存宿主仓库、资产、下载节点和发布目录；资产准备和上传仍复用子模块工具。
- 更新 UI 由宿主设置窗口维护，共享组件不拥有页面布局。

## 修改顺序

1. 在 DesktopUpdateKit 独立仓库创建分支、测试、提交并合入其 `main`。
2. 回到本仓库，将 gitlink 固定到已推送的合并提交。
3. 运行根仓库 Release、RelWithDebInfo、CTest、替换和回滚门禁。

不得让主仓库引用只存在于本机或已被强制改写的子模块提交。

## 发布边界

- 主项目采用 Apache-2.0，DesktopUpdateKit 采用 MIT。
- 正式资产同时携带双方许可证和第三方声明。
- `update.json` 只描述版本、固定 Tag、资产、大小、SHA-256 与 HTTPS 节点。
- 第三方节点只能改变传输路径，不能决定版本、资产或校验值；官方直连始终是强制兜底。
