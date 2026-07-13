# Git submodule 共享更新架构

本项目通过 Git submodule 接入独立维护的 `DesktopUpdateKit`：

```text
shared\DesktopUpdateKit
```

主仓库记录经过验证的下载器提交 SHA，不跟随下载器远程分支自动漂移。首次克隆使用：

```powershell
git clone --recurse-submodules https://github.com/wet86y/clipboard-wheel.git
```

普通 clone 后补充初始化：

```powershell
git submodule update --init --recursive
```

## 当前项目接入

- `src\ClipboardWheel\ClipboardWheel.csproj` 链接 submodule 中的 `UpdateClient`、`UpdateLauncher`、更新模型和下载控制。分块、并行、Range 回退、校验和后续断点续传只在 `DesktopUpdateKit` 实现。
- `shared\DesktopUpdateKit\src\UpdaterStub` 使用 C# NativeAOT 构建并嵌入正式单 EXE。
- `release.config.json` 保存宿主项目的仓库、资产名、项目文件和产物目录。
- `scripts\build-release.ps1`、`prepare-release-assets.ps1` 和 `publish-release.ps1` 是宿主入口，实际实现来自 submodule。
- submodule 缺失时，项目和脚本会提示运行初始化命令，不回退到任何本机兄弟目录。

## 关于页面规范

共享组件不接管完整的“关于”页面。宿主项目负责页面布局、应用简介、开发者信息、GitHub 历史和自定义内容；`DesktopUpdateKit` 只负责更新状态、下载控制、校验和安装。

更新区域按“当前版本/检查更新 → 更新说明 → 下载控制 → 立即安装”展示。失败、暂停、取消和校验结果在页面内显示；下载完成后必须由用户明确点击安装。

完整规范见 `shared\DesktopUpdateKit\docs\ABOUT_PAGE_GUIDELINES.md`。

## 下载器开发流程

submodule 初始化后通常处于 detached HEAD。修改下载器前先进入独立分支：

```powershell
Set-Location .\shared\DesktopUpdateKit
git switch main
git pull --ff-only
```

完成修改后先在下载器仓库提交并推送，再回到主仓库更新 gitlink：

```powershell
git add .\shared\DesktopUpdateKit
git commit -m "build: update DesktopUpdateKit submodule"
```

顺序必须是“推送下载器提交 → 提交主仓库 gitlink”，避免主仓库引用远程不存在的提交。不要对已被主项目引用的下载器历史做强制推送。

## 本地发布流程

```powershell
.\scripts\run-self-check.ps1
.\scripts\build-release.ps1
.\scripts\prepare-release-assets.ps1 -Version 1.0.0
.\scripts\publish-release.ps1 -Version 1.0.0
```

最后一个命令才访问 GitHub Release；源码 `git push` 是独立动作。发布脚本要求主仓库和 submodule 都没有未提交修改。

## 维护与许可证边界

- 主项目采用 Apache License 2.0。
- `DesktopUpdateKit` 采用 MIT License，完整文本位于 submodule 的 `LICENSE`。
- 正式 Release 同时携带主项目 `LICENSE`、`NOTICE`、`THIRD-PARTY-NOTICES.md` 和下载器 MIT 文本。
- 共享更新逻辑只在 `shared\DesktopUpdateKit` 对应的独立仓库维护，不在宿主项目复制实现。
- 项目版本号由根目录 `Directory.Build.props` 管理。
- `update.json` 只描述版本、发布资产、校验值和 `downloadNodes`。
- 镜像节点只能改变传输路径，不能决定版本、资产大小或 SHA-256。
- 下载器协议修改后必须重新执行升级、健康检查和回滚验收。
- 正式发布前，`git submodule status --recursive` 不得出现未初始化、提交漂移或冲突状态。
