# 原生版本发布检查清单

## 仓库与结构

- [ ] 当前提交位于 `main`，工作区干净，仅包含预期 submodule SHA。
- [ ] `git submodule status --recursive` 没有 `-`、`+` 或 `U`。
- [ ] 根目录只维护 `native`、`shared`、`scripts`、`docs`、`assets` 等当前结构；没有恢复托管源码或历史构建目录。
- [ ] `SMK_VERSION`、标签和准备资产版本完全一致。

## 自动化门禁

- [ ] 运行 `.\scripts\run-self-check.ps1`。
- [ ] Release 与 RelWithDebInfo 的全部 CTest 通过。
- [ ] 两种配置的 `--verify-release` 返回 0，诊断配置生成 PDB。
- [ ] `artifacts\超级中键-win-x64` 只包含 `超级中键.exe`。
- [ ] `prepare-release-assets.ps1` 生成的大小、SHA-256、Tag、资产名和节点列表一致。

## 人工验收

- [ ] 按 `docs\ACCEPTANCE_TESTS.md` 验证文本、图片、轮盘、设置、托盘和突破动作。
- [ ] 验证 Windows 10/11 x64、普通权限、管理员重启和多 DPI/多显示器。
- [ ] 验证 `.NET v1.1.0 → v2.0.0`、`v2.0.0 →` 下一版本，以及启动失败和健康超时回滚。
- [ ] 确认正式更新入口使用 `wet86y/clipboard-wheel`，Release 拒绝测试仓库参数。

## 发布

- [ ] Release 资产包含项目许可证、NOTICE、第三方声明和 DesktopUpdateKit MIT 文本。
- [ ] 只有明确发布时运行 `.\scripts\publish-release.ps1 -Version <version>`。
- [ ] GitHub Release 和源码推送分别核验；不得用本地旧资产冒充当前提交产物。
