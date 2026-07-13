# 分支项目说明

> 历史说明：原 `project/clipboard-wheel-branch-prep` 分支的工作已经合并到 `master`，当前文件保留为项目维护纪律文档，不代表当前仍需保留该分支。

当前分支：`project/clipboard-wheel-branch-prep`

基线提交：`7c7d1f6 fix: suppress wheel scroll while wheel is active`

## 目标

这个分支用于承载后续受控改动：先写清楚目标、边界、验收和发布纪律，再进入具体功能修订。分支从当前可用发布版切出，不复制源码目录，不维护第二套 Debug 项目。

当前分支主目标是“突破轮盘”功能拓展：在圆形轮盘外按上、下、左、右四个方向展开动作扇区，用于绑定快捷键和 Windows 快捷方式。详细设计见 `docs/EXTENDED_WHEEL_DESIGN.md`。

## 维护口径

- 唯一维护源码：`src/ClipboardWheel`
- 调试运行版：`dotnet build .\src\ClipboardWheel\ClipboardWheel.csproj -c Release`
- 打包发布版：`scripts\build-release.ps1`
- 唯一发布现场：`artifacts\超级中键-win-x64\超级中键.exe`
- 不维护单独 Debug 版本，不手工编辑 `build/` 或 `artifacts/` 中的编译产物

## 工作纪律

1. 每轮改动先确认当前分支和 `git status`。
2. 代码改动保持小步，优先修当前问题，不做顺手重构。
3. 影响中键钩子、剪贴板监听、粘贴链路、轮盘几何裁切时，必须同步更新相关文档和验收项。
4. 发布前必须跑 Release 编译；涉及分发时再跑打包脚本。
5. 打包产物只用于交付验证，不纳入 git。
6. 提交前检查 diff，确保每个改动都能对应当前任务。

## 高风险区域

- `MouseHookService`：中键、右键、滚轮、鼠标移动拦截都会影响底层应用行为。
- `ClipboardHistoryService`：去重、锁定槽位、图片捕获和 Win+V 回填顺序会影响历史稳定性。
- `PasteService`：剪贴板写入和 `Ctrl+V` 时序会影响 Excel、WPS、PowerPoint。
- `WheelOverlay.xaml.cs`：轮盘几何、图片预算、遮罩、突破轮盘外圈几何和 DPI 定位容易产生视觉越界或不同扇区不一致。
- `SettingsWindow`：新增轮盘设置页时要保持设置结构清晰；开关控件统一使用红绿拨轮/滑动开关，不使用复选框。

## 验收基线

每次功能性改动至少覆盖：

- 空历史也能唤起轮盘。
- 圆形 4/6/8 与矩形 4/8 能正常打开。
- Quick Copy 扇区不可锁定。
- 锁定槽位在扇区数量和形状切换后不占用复制扇区。
- 图片预览不超出裁切轮廓。
- PowerPoint 中按住中键并轻微滚动不会翻页或丢焦点。
- 圆形 4/6/8 能唤出突破轮盘，矩形模式不会唤出。
- 空扩展动作槽松开无动作关闭。
- 快捷键和快捷方式动作槽分别能执行对应动作。

完整手测清单见 `docs/ACCEPTANCE_TESTS.md`。

## 分支合入条件

- Release 编译通过。
- 必要时打包发布版已重新生成。
- 文档与代码行为一致。
- `git status` 干净。
- 提交信息能说明实际行为变化。
