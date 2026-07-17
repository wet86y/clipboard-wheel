# 超级中键

## 为 Windows 原生重构的剪贴板轮盘

![超级中键轮盘](assets/wheel-overview.png)

你是否厌倦了在多个窗口之间反复复制、粘贴和寻找历史内容？

超级中键是一款面向 Windows 的原生剪贴板轮盘工具。按住鼠标中键即可呼出轮盘，从最近复制的内容中快速选择，松开鼠标后直接粘贴到当前窗口。

2.0 系列已经完成原生 C++20 重构：启动路径更直接，减少运行时基础占用和大图片重复复制；剪贴板捕获、轮盘交互、渲染、更新与退出流程也重新收紧，带来更流畅、更稳定的操作体验。

本项目代码由 AI 编写，并经过人工功能与稳定性校验。

运行环境：Windows 10 1809 及以上或 Windows 11 x64，以及一个坚固的鼠标中键。

## 开源许可

主项目采用 [Apache License 2.0](LICENSE)。共享更新组件
`shared/DesktopUpdateKit` 作为 Git submodule 接入并采用 MIT License；完整归属说明见
[NOTICE](NOTICE) 和 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。

## 功能

| 功能 | 说明 |
|------|------|
| 中键轮盘 | 按住中键显示圆形/矩形轮盘，鼠标方向选择，松开粘贴；无历史时也可唤起空轮盘；轮盘激活期间拦截滚轮误触，避免 PPT 等应用翻页 |
| 突破轮盘 | 圆形模式下突破外缘可唤出外圈动作槽，支持快捷键和 `.lnk` 快捷方式；未配置槽位不参与选中并回落到内圈选择 |
| 剪贴板历史 | 启动后台读取 Win+V 历史补底，运行期自己监听并置顶，维护最近 8 条 |
| 多格式 | 文本 / HTML / RTF / CSV / TSV，Excel 表格粘贴保留格式 |
| 图片支持 | 可选捕获剪贴板图片，轮盘使用缩略图预览，粘贴保留原图（设置中开启） |
| 快捷复制 | 最后一个扇区固定为 Ctrl+C 复制按钮 |
| 临时锁定 | 轮盘选中历史扇区后右键切换锁定，当前运行期内不被新剪贴板顶走 |
| 托盘 | 左键切换捕获开关，右键菜单（设置/清空/退出），双色图标；设置窗口保持单例 |
| 绿色免装 | 单 exe 运行，设置写入 `%AppData%\超级中键\settings.json` |
| 单实例 | 启动时自动检测并阻止重复运行 |

基础轮盘与突破动作演示：

![基础轮盘演示](assets/basic-wheel.gif)

![突破轮盘与快捷动作演示](assets/extended-wheel.gif)

## 下载

普通用户可从 [GitHub Releases](https://github.com/wet86y/clipboard-wheel/releases/latest)
下载最新稳定版。程序为绿色免安装单 EXE；当前尚未使用 Windows 代码签名证书，请仅从
本仓库 Release 获取，并保留随发布资产提供的许可证与第三方声明。

### 2.0.2：隔离 Windows Shell 阻塞

2.0.2 修复了部分本地文件夹可能让 Windows Shell 调用不返回，进而堵住整个突破动作队列的问题。

- **文件夹动作进程隔离**：文件夹快捷方式由独立 STA 辅助进程执行，恢复默认 Shell 动词与 `NOASYNC/DDEWAIT` 语义；异常目录不能再直接卡住主程序动作线程。
- **明确的超时边界**：辅助进程连续 3 秒未返回时生成挂起诊断，主程序最多等待 5 秒便回收辅助进程并继续处理后续动作。
- **更可靠的权限重启**：退出旧进程时提前释放单实例互斥体，避免管理员模式重启的新进程因后台清理而接管超时。
- **纯净的正式资产**：正式 Release 仍为单 EXE，不包含诊断日志、PDB 或故障注入。

### 2.0.1：原生重构后的完整稳定版

2.0.1 不只是一次常规补丁，而是完整原生重构落地后的一轮系统性修复与收口：从启动、捕获、轮盘渲染和粘贴，到设置迁移、权限切换、后台退出与在线更新，关键链路均按生产质量重新梳理。

- **更轻快的原生运行**：使用 C++20 与 Windows 原生 API，缩短启动链路，降低常驻运行时基线；图片载荷采用共享不可变数据，减少大对象复制。
- **更流畅的轮盘体验**：所有图片入口预先生成最长边不超过 360px 的轮盘预览，原图仅在粘贴时使用，避免大尺寸照片拖慢轮盘显示。
- **更干净的剪贴板事务**：内部写入不会被再次捕获；粘贴目标失效、写入失败或按键发送失败时恢复用户原剪贴板，避免失败操作留下半成品内容。
- **更可靠的输入安全**：重新收紧中键、左右键与权限边界状态。进入管理员窗口或 UAC 安全桌面、按下 Esc、投递失败，或单次轮盘达到 15 秒时，立即取消本次交互并释放鼠标；之后仍可正常再次唤起。
- **完整的旧设置升级**：兼容读取 1.x 设置和旧目录，成功后统一迁移为 Version 3；只保证旧版向 2.x 升级，不支持反向降级。
- **更稳的生命周期与更新**：加固部分初始化失败、后台线程停止、重复退出、D2D 设备重建和更新回滚；更新包由宿主与更新 Stub 双重执行 SHA-256 校验。
- **纯净的正式资产**：Release 继续保持单 EXE、无日志、无故障注入和无诊断运行开销；完整诊断能力只保留在本地 RelWithDebInfo 构建中。

## 当前架构

主程序按 `app → ui/platform/updater → core` 的方向组织：

| 模块 | 职责 |
|------|------|
| `native/app` | 进程入口、AppHost 总编排、启动与退出顺序 |
| `native/ui` | 轮盘和设置窗口、Direct2D/DirectWrite/WIC 渲染、DPI 与设备重建 |
| `native/platform/windows` | 剪贴板/OLE、输入钩子、窗口与权限边界、设置持久化 |
| `native/core` | 历史、布局、选择和其他可独立测试的业务逻辑 |
| `native/updater` | 主程序与更新组件之间的宿主适配 |
| `shared/DesktopUpdateKit` | 独立维护的原生下载、校验、替换与回滚组件 |

共享更新组件以静态库接入主程序，更新 Stub 作为资源嵌入正式 EXE；发布目录仍只需要一个 `超级中键.exe`。

## 构建

超级中键 2.0 使用 C++20、Win32、Direct2D、DirectWrite、WIC、OLE 与 C++/WinRT，当前正式资产面向
Windows 10 1809 及以上和 Windows 11 x64，不依赖第三方运行时。源码位于 `native`，共享更新组件
位于 `shared\DesktopUpdateKit`。首次克隆必须初始化子模块：

```powershell
git clone --recurse-submodules https://github.com/wet86y/clipboard-wheel.git
```

已有工作区可执行 `git submodule update --init --recursive`。构建脚本通过 Visual Studio
Installer 自动定位 MSVC、CMake 和 Windows SDK，普通 PowerShell 不需要预先加载开发者环境。

| 类型 | 命令 | 产物位置 | 用途 |
|------|------|----------|------|
| 诊断运行版 | `.\scripts\run-dev.ps1` | `build\bin\RelWithDebInfo\超级中键.exe` 与 PDB | 保持优化和真实动画时序，启用结构化诊断日志 |
| 正式打包版 | `.\scripts\build-release.ps1` | `artifacts\超级中键-win-x64\超级中键.exe` | 单 EXE、无第三方运行时，分发用 |

`build\native` 只保存 CMake/MSVC 中间文件和测试程序，`build\bin` 只保存便于人工运行的
应用产物，`artifacts\超级中键-win-x64` 只允许存在正式 `超级中键.exe`。不得创建第二套
调试发布目录，也不得手工修改生成物。

## 项目文档

- `docs/ACCEPTANCE_TESTS.md`：完整人工验收清单。
- `docs/BUILD_OUTPUT_CONTRACT.md`：中间文件、诊断产物与正式包的唯一目录约束。
- `docs/EXTENDED_WHEEL_DESIGN.md`：突破轮盘布局和动作语义。
- `docs/LAUNCH_COMPATIBILITY.md`：浏览器、文档和 WPS 动作兼容规则。
- `docs/RELEASE_CHECKLIST.md`：原生版本发布门禁。
- `docs/SHARED_UPDATE_ARCHITECTURE.md`：原生程序如何接入 DesktopUpdateKit。
- `docs/UPDATE_DESIGN.md`：下载、替换、健康确认与回滚设计。
- `docs/用户使用说明.md`：普通用户使用说明。

## 托盘操作

- **左键单击**：切换中键捕获 开/关。图标即时变化（蓝色圆 = 开启，灰色圆 = 关闭）
- **右键单击**：弹出菜单（启用/禁用轮盘、设置、清空历史、退出）
- **设置入口**：只通过右键菜单的“设置”打开；重复点击只会激活已有设置窗口，不会创建多个。

## 设置要点

| 设置 | 说明 |
|------|------|
| 快捷复制 | 最后一个扇区固定为复制按钮（深绿色），选中即 Ctrl+C |
| 扇区锁定 | 轮盘打开时选中历史扇区并右键，蓝灰色表示锁定；再次右键解除。Quick Copy 扇区不可锁定；切换形状/扇区数后会按当前可见历史槽位归一化 |
| 图片捕获 | 默认关闭；打开后实时捕获剪贴板图片，轮盘中显示缩略图 |
| 形状/扇区 | 圆形 4/6/8（切换默认 6），矩形 4/8（切换默认 8） |
| 轮盘大小/死区/透明度 | 单页设置窗口直接调整，保存后即时写入 settings.json |

粘贴模式固定为 Smart：单格文本走纯文本，表格 HTML 走格式化；含 `<table>` 的合并单元格自动识别。文本/富文本剪贴板捕获固定为全格式，图片捕获由设置开关单独控制，历史固定 8 条；中键轮盘是否启用由托盘图标/菜单控制。Ctrl/Shift 修饰键不再改变粘贴模式。

## 诊断

运行 `.\scripts\run-dev.ps1` 会构建并启动 `RelWithDebInfo`。诊断日志位于：

```text
%LOCALAPPDATA%\超级中键\logs\native-diagnostic-*.log
```

日志为 UTF-8 JSON Lines，不记录剪贴板正文、图片内容、完整路径、URL、窗口标题或用户输入。
提交前运行 `.\scripts\run-self-check.ps1`，它会构建 Release 与 RelWithDebInfo、执行全部 CTest、
检查 PDB 和资源，并重新生成唯一正式包。
