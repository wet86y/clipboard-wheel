# 原生程序内更新设计

主程序读取 `wet86y/clipboard-wheel` 的最新正式 Release，在“关于”页展示更新日志、下载状态、
节点、连接数和安装操作。Release 固定正式仓库；只有 RelWithDebInfo 接受隔离测试仓库参数。

## 下载与校验

- DesktopUpdateKit 使用 WinHTTP 流式写文件，支持最多四路 Range 下载和单路回退。
- 清单大小、独立 SHA 文件和最终 EXE SHA-256 必须全部一致。
- 节点必须为 HTTPS；错误 Content-Range、HTML 响应、路径穿越和 HTTPS 降级均被拒绝。
- 暂停、继续、后台下载、取消和节点切换由进程级会话维护，不依附设置窗口生命周期。

## 替换与回滚

1. 主程序释放内嵌原生 Stub 和事务 JSON。
2. Stub 等待旧进程退出，暂存新版、备份旧 EXE 并原子替换。
3. 新版完成托盘、设置、剪贴板和输入安全初始化后写入 `--update-health` 标记。
4. 启动失败或健康超时会终止失败进程、恢复备份并重新启动原版本。
5. 成功后清理备份、负载和事务目录；无法即时删除的 Stub 使用退出后清理。

更新不得修改 `%APPDATA%\超级中键\settings.json` 的格式，也不得改变互斥体、自启动值或
默认 `asInvoker` 行为。不可写目录会报告失败，不自动提升权限。

## 发布协议

资产名保持 `super-middle-key.exe`、`super-middle-key.exe.sha256` 和 `update.json`。
正式升级门禁必须覆盖托管 `v1.1.0 →` 原生 `v2.0.2`、`v2.0.1 → v2.0.2` 和失败回滚。
