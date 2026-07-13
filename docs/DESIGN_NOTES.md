# 设计备注

## 1. 为什么不直接依赖 Win+V 历史

Windows 剪切板历史可通过 WinRT API 读取,但它依赖用户是否启用系统剪切板历史,也可能受系统策略限制。超级中键的主流程必须由本程序自行监听剪切板变化维护历史。

因此设计为:

```text
Win+V 历史读取 = 启动补齐
本程序历史 = 主数据源
```

启动导入只做补底：导入项通过 `ImportAsBackfill` 追加到空余历史槽位，遇到重复项不移动，历史已满时不挤掉运行期复制内容。程序启动后用户新写入剪贴板的内容永远走运行期监听入口并置顶。

## 2. 为什么不用黑名单

本工具的主要场景就是 WPS / Excel / Word / 浏览器 / 文本编辑器。默认黑名单会伤害主场景。

使用应用级捕获策略:

```text
always       默认启用
modifierOnly 强依赖中键的软件使用 Ctrl + 中键触发
disabled     极少数冲突应用才禁用
```

## 3. 为什么用剪切板 + Ctrl+V 粘贴

直接操作目标文本框需要适配 Win32、WPF、Qt、Electron、浏览器、Office 等大量控件模型,不适合 V1。

设置剪切板再模拟 Ctrl+V 是最通用方案。

## 4. 为什么 ClipboardEntry 不能是 string

表格复制时可能同时存在:

```text
UnicodeText
HTML Format
Rich Text Format
CSV / TSV
Office 私有格式
```

如果只存 string,后续无法实现保留表格格式和纯文本兜底。

## 5. 格式保留的边界

V1 保存 HTML / RTF / CSV / TSV,不保存 Office 私有二进制格式。这样足以覆盖大部分 Excel / WPS → Word / WPS 文档 / 普通文本框场景。

如需更强格式复刻,后续可以增加 raw clipboard format 捕获,但那会显著增加复杂度和兼容风险。

## 6. Win+V 历史:WPF→OLE→Win+V 链上 flag 机制不可靠

### 机制本身

Windows 提供 `CanIncludeInClipboardHistory` 私有 IDataObject 格式(Microsoft Learn Clipboard Formats 文档),DWORD 值,**0 = 排除**,非 0 = 显式请求收录。WinRT 对应是 `ClipboardContentOptions.IsAllowedInHistory`。

WPF 的 `DataObject.SetData("CanIncludeInClipboardHistory", false)` 在应用层看是正确调用。

### 为什么不靠谱

实测(接手后针对 Win11 22H2-24H2)三个因素叠加让这条路靠不住:

1. **WPF→OLE 传递不被保证**:`CanIncludeInClipboardHistory` 是私有格式字符串,WPF 文档未明确说 `OleSetClipboard` 时私有格式会出现在 `EnumFormatEtc` 里。在某些 WPF/.NET 版本组合下 flag 没到 OLE,Win+V 自然看不见。
2. **Win+V 本身的异步通知**:Win+V 是 Windows 监听 `WM_CLIPBOARDUPDATE` 后台服务,通知是消息队列、异步处理的。"交替复制"场景下中间状态可能被跳过。Microsoft 工程师文章与 WindowsForum 文档均记载该机制。
3. **Delayed rendering 超时**:Excel 复制大表时用 delayed rendering(注册格式但 data handle 为 NULL,别的程序 GetClipboardData 时才生字节)。Win+V 读这些数据有 30 秒硬超时。超时则返回 NULL,该项不收录。

### V1 妥协

- 保留代码:`CanIncludeInClipboardHistory=false` 在某些环境(Windows 版本 + .NET 版本 + WPF 版本)下可能有效,作为 best-effort。代码里加了 trace 日志:`BuildDataObject entry_mode_requested=... add_to_history_setting=...`,能直接看出 setting 是否读到了。
- 设置固定:`addPasteToClipboardHistory=false`。用户既然使用超级中键，默认不再主动追求 wheel paste 进入 Win+V；污染 Win+V 的副作用可接受，但仍尽量用 flag 排除。
- **可靠兜底**:关 Windows 的 Win+V(设置 → 系统 → 剪切板 → 关闭"剪切板历史记录")。超级中键自己的扇区历史就是"需要什么再取什么",可以顶替 Win+V。
- 后续方向:如果用户明确需要 V2 重新接入 Win+V,需要三件事叠加才可靠:(a) Win32 `SetClipboardData` 直写,绕开 WPF,亲自枚举私有格式;(b) Win+V 异步通知追不上的场景走 OLE `IDataObject::EnumFormatEtc` 主动抓一次而不是等通知;(c) delayed rendering 超时场景靠我们自己持数据 `SetClipboardData` 不用延迟渲染。

## 7. 粘贴后是否恢复原剪切板:与 Excel/WPS 慢读存在竞态

之前默认开启 `restoreClipboardAfterPaste`,但在 Excel / WPS 处理 `CF_HTML` 表格粘贴时(解析 + 插入单元格 + 样式应用),读剪贴板时延可能超过 `restoreDelayMs`(默认 150ms)。这时我们的 restore 会把"原剪贴板内容"写回去,覆盖 Excel 即将读到的 payload,表现为"wheel 选对了,粘出的却是上一个复制"。Excel 运行越久越慢,复现概率越高。

### V1 妥协

- 固定关闭 `restoreClipboardAfterPaste`。
- 旧的 opt-in 设置已从 UI 移除。需要回退时可以从 `PasteSettings` 字段恢复，但默认路线不再做粘贴后回填。
- V2 考虑用 delayed rendering 重做(注册 `CF_HTML` 占位 + 在 `WM_RENDERFORMAT` 回调里按需生成 + Excel 读完再 restore)。或者更轻:改用 `SendInput` 的 `VK_PACKET` 直接键入纯文本(不动剪贴板),表格条目还是走剪贴板。

## 8. 轮盘布局：形状 / 等级制 / 间隔

### 8.1 圆形

扇形画满 360°（无角度间隙），间隔靠 **0.9× 基础缩放**：每个扇区挂一个 `TransformGroup`，底层 `ScaleTransform(0.9, 0.9, centroid)` 固定缩小。所有扇区均匀缩 10%，相邻边界自然分离出均匀空隙。无 Stroke、无独立分隔线。

选中扇区通过上层动画 `ScaleTransform`（1.0 → `SelectedSectorScale`，默认 1.08）叠在 base 上，等效 0.9 → 0.972。

### 8.2 矩形

Tier 制：4 区 = 2×2 网格，8 区 = 3×3 九宫格掏空中（中心格填 `CenterColor`，与圆形死区同色）。

格子间隔代码默认 4 px（`SectorGapPixels`），间隙填 borderBrush 半透明填充条可见。中心死区（`InnerDeadZoneRadius`）对两种形状都生效：圆形走角度命中前判断距离，矩形走格子命中前判断距离。

### 8.3 等级制（Tier）

圆形 Tiers: {4, 6, 8}。矩形 Tiers: {4, 8}。设置页切到圆形时默认选择 6 扇，切到矩形时默认选择 8 扇；用户仍可手动改到该形状支持的其他 tier。空槽位用 muted 色 + 低透明显示，不参与选中动画。`NormalizeSectorCount(shape, count)` 保证运行时永远不会出现非 tier 值。

### 8.4 文本自动计算

预览字符数不再由用户手动设置。圆形按扇区 geometry 做水平扫描，逐行计算可用宽度；文本从开头显示，空间不足时只截断尾部。矩形按单元格安全边距计算文本区，内容垂直居中并用真实格子 geometry 裁剪，避免贴圆角或中心死区。

图片捕获默认关闭。打开后，运行期剪贴板监听会优先读取 `PNG` / `JFIF` / `GIF` 编码流，兜底读取 WPF `BitmapSource`、DIB 和 `System.Drawing.Bitmap`，统一转成 PNG 字节保存。每个图片条目保留原始 PNG 字节用于粘贴，同时生成最长边约 360px 的预览 PNG 给轮盘解码显示，避免每次打开轮盘都解码大图。粘贴时解码原图后只写回 WPF 标准 Bitmap 载荷再发送 Ctrl+V。不要额外注册 `PNG` 自定义格式：Excel 等应用可能优先读取该格式，WPF/OLE 转交后容易出现“图片无法打开”。图片捕获和表格识别只按实际载荷判断（CSV/TSV/HTML table），不按来源进程排除，避免 Excel 内图片被误判成表格。启动 Win+V 历史导入仍会按设置读取图片，但导入在应用启动后以空闲优先级后台执行，不阻塞托盘、鼠标钩子和空轮盘可用。

## 9. "场景 A"跳过 SetDataObject

wheel paste 之前在 UI 线程对比剪贴板当前 `UnicodeText` 与 wheel 条目的 `PlainText`:
- 一致:跳过 `SetDataObject`(剪贴板本来就是对的)。**Win+V 也不增条目**(没写就没记录)。
- 不一致:调 `SetDataObject` 走重试逻辑。

这是 V1 主要场景的优化:"复制 X → wheel 选 X 再粘"是最高频动作,本来就不会改 Win+V。

## 10. WM_MOUSEMOVE 不再被钩子吃掉（2026-07-07）

之前的实现中 `MouseHookService` 对每条轮盘激活期间的 WM_MOUSEMOVE 返回 `Suppress()`（即 `1`），导致底层应用完全收不到鼠标移动事件。虽然系统光标依然移动，但浏览器/Excel 等应用表现为"鼠标卡死"。

现行方案：钩子不再返回 `Suppress()`，改为让 WM_MOUSEMOVE 正常通过钩子链。轮盘扇区的高亮更新仍由钩子线程 `BeginInvoke` 驱动。同时轮盘覆盖窗口加了 `WS_EX_TRANSPARENT` 样式，确保鼠标事件能穿透透明覆盖层到达底层应用。

例外：轮盘激活期间继续拦截 `WM_MOUSEWHEEL` / `WM_MOUSEHWHEEL`。中键按下时部分鼠标会偶发滚轮抖动，PowerPoint 等应用对滚轮翻页很敏感；滚轮事件不参与轮盘选区，放行只会增加误翻页和丢焦点风险。

远程桌面或网络波动可能让鼠标按下/抬起事件乱序到达。当前保护策略是：打开/关闭轮盘都异步调度，不在鼠标钩子里等待 UI；每次轮盘隐藏后统一清理内部输入状态；关闭后短暂冷却，防止旧中键事件立即重入；轮盘异常停留超过保护时间时自动取消。不要在鼠标移动时依赖系统“中键是否仍按下”的瞬时状态取消轮盘，因为远程桌面下该状态可能短暂不可信。

## 11. Quick Copy — 最后一个扇区变为复制按钮

设置 `wheel.quickCopy=true` 后，最后一个扇区（顺时针排列，对应左上角）固定占用为 Ctrl+C 按钮。扇区展示为深绿色背景、加粗蓝色文字，选中后发送 `SendInput(Ctrl+C)` 复制底层应用当前选中的内容。

这个扇区不消耗剪切板历史条目——前面的空位显示为 muted 占位符，实际条目数量减一。

## 12. 运行期扇区锁定

轮盘激活时，右键当前选中的真实历史扇区会切换运行期锁定状态。锁定状态只保存在内存中的 `ClipboardEntry.IsLocked`，不写入 `settings.json`，程序退出后失效。

历史服务在新增剪贴板条目时按槽位重排：已锁定条目保留当前索引，新条目填入第一个未锁定槽位。例如第一个扇区锁定后，新复制内容进入第二个历史扇区。Quick Copy 是 UI 占位项，不进入历史服务，也不能被右键锁定。

当形状、扇区数量或 Quick Copy 开关变化时，打开轮盘前会按当前可见历史槽位数归一化锁定项：仍在可见范围内的锁定项保留原槽位，超出范围的锁定项尽量压回最后的可见历史槽位；如果锁定项多于当前可见历史槽位，剩余锁定项继续保留在历史中，等切回更多扇区后再显示。Quick Copy 开启时，最后一个复制扇区不参与归一化，永远不会被历史锁定项占用。

锁定/解锁是独立操作：右键成功切换后，本次松开中键只关闭轮盘，不执行粘贴。

## 13. PasteTrace 构建门控

`PasteTrace` 使用 `PASTE_TRACE_ENABLED` 编译符号控制调用点：
- 普通 `dotnet build -c Release` 默认启用，方便本地调试并写入 `paste-trace.log`。
- `scripts/build-release.ps1` 明确传入 `/p:PasteTraceEnabled=false`，发布的 single-file exe 不包含日志调用点。

这样不依赖 single-file 运行时的程序集路径特征，也不会产生 IL3000 警告；同一套 Release 源码仍能稳定产出调试版和发布版。
