# 应用与文档启动兼容层

## 目标

外圈“应用”动作只控制由该动作启动的窗口，绝不根据同名进程扫描或接管用户已有会话。

## 启动会话

每个动作使用一个绑定键：普通快捷方式以快捷方式路径为键；浏览器额外包含规范化后的目标网址。

一次启动遵循以下流程：

1. 对关联应用的可见顶层窗口做快照。
2. 启动目标。
3. 仅登记快照中不存在的新窗口句柄。
4. 二次触发只最小化、恢复或关闭已登记句柄。
5. 若启动页句柄失效，按原始快照重新绑定替换后的新窗口。
6. 已登记窗口若被应用隐藏到托盘，二次触发优先恢复并置前，而不是再次发送关闭。
7. 找不到安全的新顶层窗口时，保留 6 秒等待期；到期后允许按原路径重新启动一次，并以 1.2 秒冷却防止连续触发创建多个窗口。

这套流程覆盖浏览器、PDF 阅读器以及存在启动页或进程交接的应用，同时避免影响手动打开的同名窗口。

文件夹是例外：Explorer 是常驻进程，文件夹绑定只按实际 Explorer 窗口路径判断。窗口不存在时会直接重新打开目标文件夹，不受旧 `explorer.exe` 进程影响。

## 兼容配置表

`MouseHookService` 中的 `CompatibilityProfiles` 仅描述已知应用的额外规则：

| 应用 | 独立启动 | 主窗口筛选 |
| --- | --- | --- |
| Excel | `/x` | `XLMAIN` |
| Word | 程序本体使用 `/w` | `OpusApp` |
| PowerPoint | 无强制实例参数 | `PPTFrameClass` |
| WPS Writer / 表格 / 演示 / PDF | 使用默认启动方式 | 通用新增窗口观察 |

未知应用不需要加入表即可获得通用窗口会话支持；只有启动页、实例转交或窗口类特殊时才增加专用规则。

## 文档与 WPS

文档通过 Windows 文件关联启动，并使用 `FindExecutable` 获取关联程序用于窗口观察。WPS 默认支持同一窗口多标签；当文档被复用到已有 WPS 窗口时，Windows 顶层窗口 API 无法安全区分其中单个标签。因此兼容层会正常打开文件，但不会最小化或关闭整个已有 WPS 会话。

若 WPS 配置为新开顶层窗口，或本次启动创建了新顶层窗口，兼容层会照常登记并精确控制该窗口。

## Shared WPS host safety

When a WPS document is handed to an existing tabbed host, the session is
activation-only: a second trigger may restore and foreground the recorded host,
but it never binds later splash or helper windows as the document window.
Tab selection is a one-time best-effort UI Automation enhancement. If WPS does
not expose a matching tab, the attempt is disabled for that session so it cannot
delay later triggers. This WPS-specific behavior is deliberately not applied to
other applications.
