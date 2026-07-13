# 超级中键落地说明

## 1. 产品边界

超级中键是一个 Windows 绿色小工具。它不是完整剪切板管理器，核心目标是：

> 用“按住鼠标中键 + 鼠标方向选择 + 松开粘贴”替代 `Win + V` 的剪切板历史选择。

V1 必须保证以下链路稳定：

1. 启动后托盘常驻。
2. 启动时尝试读取 Windows 剪切板历史。
3. 运行期监听当前剪切板变化。
4. 维护最近 N 条剪切板历史。
5. 中键按住显示轮盘。
6. 松开中键后粘贴到当前输入焦点位置。
7. Excel / WPS 单元格复制可以作为纯文本粘贴到普通文本框。
8. Excel / WPS 多单元格复制尽量保留 HTML / RTF / CSV / TSV 格式。

## 2. 技术选型

推荐技术栈：

```text
C#
.NET 8
WPF
Win32 API
WinRT Clipboard History API
single-file exe publish
```

不要改成 Python / Electron。原因：这个工具高度依赖 Windows 桌面输入、托盘、剪切板多格式、透明置顶窗口，WPF + Win32 是最直接的路线。

## 3. 系统 API 依赖

### 3.1 剪切板监听

使用 `AddClipboardFormatListener(hwnd)` 注册隐藏窗口。系统剪切板变化时，窗口收到 `WM_CLIPBOARDUPDATE`。

实现位置：

```text
Services/ClipboardMonitorService.cs
Native/NativeMethods.cs
```

### 3.2 Windows 剪切板历史读取

启动后以空闲优先级后台调用：

```csharp
Windows.ApplicationModel.DataTransfer.Clipboard.GetHistoryItemsAsync()
```

这只是增强能力。它可能失败，例如：

- 用户未开启 Windows 剪切板历史。
- 系统策略限制。
- API 返回非 Success 状态。
- 某些条目不含文本或 HTML。

要求：失败时不得阻塞主程序；成功导入也不得阻塞托盘、鼠标钩子或空轮盘首次唤起。导入项只能通过 `ImportAsBackfill` 补到底部空位，遇到重复项不移动，历史已满时不顶掉运行期新复制内容。

实现位置：

```text
Services/WindowsClipboardHistoryService.cs
```

### 3.3 鼠标中键捕获

使用 `SetWindowsHookEx(WH_MOUSE_LL, ...)` 安装低级鼠标钩子。

实现位置：

```text
Services/MouseHookService.cs
Native/NativeMethods.cs
```

核心事件：

```text
WM_MBUTTONDOWN
WM_MOUSEMOVE
WM_MOUSEWHEEL / WM_MOUSEHWHEEL
WM_MBUTTONUP
```

中键按下时显示轮盘，并拦截中键原始事件。轮盘激活期间 `WM_MOUSEMOVE` 只用于更新扇区高亮后继续放行，保持底层应用鼠标移动响应；`WM_MOUSEWHEEL` / `WM_MOUSEHWHEEL` 直接拦截，避免按下滚轮时的偶发滚动传给 PowerPoint 等应用造成翻页或焦点丢失。

轮盘关闭必须做一次输入状态清理：清掉本次选区、待处理鼠标移动、中键按下时间和锁定切换标记。关闭后保留一个很短的冷却窗口，避免远程桌面或网络波动补发旧中键事件后立刻再次误触发。轮盘异常停留超过保护时间时自动取消，但不要模拟鼠标抬起或点击，避免误伤底层应用。

突破轮盘的外圈空槽只作为占位显示。空槽不参与命中测试，不显示选中动画，也不会覆盖内圈选择；只有配置了有效快捷键或 `.lnk` 快捷方式的槽位才允许被选中和执行。

### 3.4 自动粘贴

不要尝试直接写入目标控件。应采用通用方案：

```text
把选中条目写入系统剪切板
↓
SendInput 模拟 Ctrl + V
↓
延迟等待目标应用读取
```

实现位置：

```text
Services/PasteService.cs
Native/NativeMethods.cs
```

注意：`SendInput` 受 Windows UIPI 约束。如果目标窗口以管理员权限运行，而超级中键不是管理员权限，可能无法注入。设置页提供“管理员模式运行”开关，点击“保存”后才会重启切换权限。

## 4. 数据结构

每条剪切板历史不能只存 string。必须用多格式容器：

```csharp
ClipboardEntry
├─ DisplayText
├─ PlainText
├─ HtmlText
├─ RtfText
├─ CsvText
├─ TsvText
├─ SourceProcessName
├─ LooksLikeSpreadsheet
├─ LooksLikeSingleCell
└─ CreatedAt
```

原因：Excel / WPS 表格复制时，剪切板里可能同时包含纯文本、HTML、RTF、CSV、Office 私有格式。V1 不保存 Office 私有二进制格式，但应保存 HTML / RTF / CSV / TSV 和纯文本。

## 5. 表格处理策略

### 5.1 捕获时

从 `IDataObject` 中尽量读取：

```text
UnicodeText
Text
HTML Format
Rich Text Format
CSV
```

然后判断：

```text
PlainText 含 tab 或多行 → 可能是表格
HTML 含 <table → 可能是表格
CSV → 可能是表格
PlainText 不含 tab 且不含换行 → 单单元格
```

### 5.2 粘贴时

默认智能粘贴（固定，不再暴露 UI）：

```text
单单元格 → 优先纯文本
多单元格且有 HTML / RTF → 优先保留格式
其他 → 纯文本
```

Ctrl / Shift 修饰键不再改变粘贴模式（旧逻辑已注释保留）。

## 6. 轮盘 UI

实现位置：

```text
UI/WheelOverlay.xaml
UI/WheelOverlay.xaml.cs
```

窗口要求：

```text
无边框
透明
置顶
不抢焦点
不显示任务栏
```

关键参数：

```text
扇区数量：圆形 {4, 6, 8}，矩形 {4, 8}。设置页切到圆形默认 6，切到矩形默认 8。
轮盘半径
中心死区半径
透明度
普通扇区颜色
高亮扇区颜色
文字颜色
最大预览字符数
```

选择计算：

```text
dx = mouseX - centerX
dy = mouseY - centerY
距离 < deadZone → 无选择
角度 atan2(dy, dx)
角度映射到扇区 index
```

扇区锁定：

```text
轮盘激活期间，右键当前选中的真实历史扇区 → 切换运行期锁定。
锁定项使用蓝灰色标记（普通 `#4E6E9E`，选中 `#5F83BA`），只保存在 ClipboardEntry.IsLocked 内存状态，不写 settings.json。
右键锁定/解锁后，本次松开中键只关闭轮盘，不执行粘贴。
Quick Copy 是最后一个 UI 占位扇区，不进入历史服务，也不能被右键锁定。
```

历史槽位兼容：

```text
新增剪贴板内容时，锁定项保留当前索引，新内容填入第一个未锁定历史槽位。
打开轮盘前，按当前形状、扇区数量和 Quick Copy 开关计算可见历史槽位。
切换到更少扇区时，超出可见范围的锁定项尽量压回最后的可见历史槽位。
锁定项多于可见历史槽位时，剩余锁定项保留在历史中，切回更多扇区后可再次显示。
Quick Copy 开启时，最后一个复制扇区永远不参与锁定项归一化。
```

## 7. 设置窗口

实现位置：

```text
UI/SettingsWindow.xaml
UI/SettingsWindow.xaml.cs
```

V1 先做基础字段，不要求做完整主题编辑器。

必须支持：

- 扇区数量。
- 半径。
- 死区半径。
- 透明度。
- 快捷复制开关。
- 图片捕获开关（默认关闭）。

以下策略固定在代码中，不在设置窗口暴露：

- 启动时读取一次 Win+V 历史。
- 历史数量固定 8 条。
- 粘贴模式固定 Smart。
- 不恢复原剪切板。
- wheel 粘贴尽量不进入 Win+V，但只作为 best-effort。
- 应用黑白名单停用，中键捕获开关由托盘图标/菜单控制。

配置文件位置：

```text
%AppData%\超级中键\settings.json
```

首次使用中文名版本时，如果新目录没有设置文件，会尝试从旧目录 `%AppData%\ClipboardWheel\settings.json` 复制一份作为迁移来源。迁移失败不应阻止程序启动，失败时回退默认配置。

## 8. 托盘菜单

实现位置：

```text
Services/TrayService.cs
```

程序启动时通过 `Local\SuperMiddleKey.SingleInstance` 命名 Mutex 做单实例保护。第二个进程会在初始化托盘、剪贴板监听和鼠标钩子前退出。

菜单建议：

```text
启用中键轮盘
暂停 10 分钟
打开设置
清空历史
退出
```

注意：左键单击托盘图标只用于切换中键捕获总开关；设置窗口只从右键菜单进入。设置窗口必须保持单例，重复点击“设置”只激活已有窗口，不能创建多个。暂停只暂停鼠标捕获，不停止剪切板监听。

## 9. 应用捕获规则

应用级中键捕获规则已停用，所有应用的捕获启用/禁用由托盘图标和菜单控制。旧 per-app 规则（always / modifierOnly / disabled）在 `ProcessRules` 中以注释形式保留，方便回退。

## 10. 发布方式

项目只维护 Release 源码路径。日常验证使用 `dotnet build .\src\ClipboardWheel\ClipboardWheel.csproj -c Release` 的调试运行版；分发使用 `scripts\build-release.ps1` 生成的打包发布版。编译输出由根目录 `Directory.Build.props` 统一放入 `build`，不要在源码目录下维护 `bin` 或 `obj`。

绿色 exe，推荐用项目自带脚本：

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build-release.ps1
```

产物：`artifacts\超级中键-win-x64\超级中键.exe`

也可手动发布：

```powershell
dotnet publish .\src\ClipboardWheel\ClipboardWheel.csproj -c Release -r win-x64 --self-contained true /p:PublishSingleFile=true /p:PasteTraceEnabled=false /p:IncludeNativeLibrariesForSelfExtract=true -o artifacts\超级中键-win-x64
```

## 11. 本地 agent 注意事项

1. 不要先做安装包。
2. 不要把 ClipboardEntry 简化成 string。
3. 不要删除 Win+V 读取模块，失败也要保留可选增强。
4. 日志由 `PasteTraceEnabled` 构建属性控制：普通 `dotnet build .\src\ClipboardWheel\ClipboardWheel.csproj -c Release` 默认写日志，发布脚本传入 `/p:PasteTraceEnabled=false` 后不写日志。
5. 先保证纯文本链路，再处理格式保留。
6. 如果 WinRT 历史读取编译报错，检查目标框架为 `net8.0-windows10.0.19041.0`。
7. 如果 `SendInput` 无法粘贴到管理员窗口，用管理员模式重启验证。
8. 如果轮盘抢焦点导致无法粘贴，检查 `WS_EX_NOACTIVATE` 是否生效。
9. 启动时通过 `Local\SuperMiddleKey.SingleInstance` Mutex 防止多实例。
10. 图片捕获默认关闭，图片识别和表格识别只按实际剪贴板载荷判断，不按来源进程排除。
