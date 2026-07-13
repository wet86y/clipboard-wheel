# 发布检查清单

这个清单用于打包发布版前的最后确认。项目只维护 Release 源码路径，发布版统一由 `scripts\build-release.ps1` 生成。

## 代码状态

- [ ] 当前分支正确。
- [ ] 已从当前仓库根目录运行脚本，未依赖本机绝对路径。
- [ ] `git status` 只包含本轮预期改动。
- [ ] 代码 diff 已审阅，没有混入无关格式化或产物文件。
- [ ] 相关文档已同步。
- [ ] 关于页遵守共享 `ABOUT_PAGE_GUIDELINES.md`；更新区域由项目 UI 承载，下载协议仍由共享组件提供。

## 共享工具子模块

- [ ] `git submodule status --recursive` 未出现 `-`、`+` 或 `U` 状态。
- [ ] `shared\DesktopUpdateKit` 指向已经推送到其公开远程仓库的提交。
- [ ] 确认 `release.config.json` 的仓库、资产名和产物目录属于当前项目。
- [ ] 确认正式构建使用共享 `UpdaterStub`，没有恢复项目内重复副本。
- [ ] 构建末尾出现 `Release executable verification passed: --verify-release`；该检查必须直接运行最终单文件 EXE，并确认内嵌替换组件具有完整的 PE/COFF 头及未越界的节区数据。
- [ ] 只有明确发布时才运行 `scripts\publish-release.ps1`。

## 编译与打包

- [ ] 执行 `dotnet build .\src\ClipboardWheel\ClipboardWheel.csproj -c Release`。
- [ ] 无错误；如有 warning，确认是已知且可接受。
- [ ] 需要分发时执行 `scripts\build-release.ps1`。
- [ ] 确认发布 exe 位于 `artifacts\超级中键-win-x64\超级中键.exe`。
- [ ] 确认正式包未生成 `paste-trace.log`；日志仅存在于诊断包。
- [ ] 确认正式发布目录只包含 `超级中键.exe`，不附带 `.pdb` 调试符号文件。
- [ ] 确认正式包启动/退出后，除 `%AppData%\超级中键\settings.json` 外不写入运行状态或日志文件。
- [ ] 确认旧发布目录 `artifacts\ClipboardWheel-win-x64` 未被误用。

## 核心手测

- [ ] 首次启动、无历史时，中键可唤起空轮盘。
- [ ] 复制文本后，中键轮盘可选中并粘贴。
- [ ] 圆形 4/6/8 和矩形 4/8 都能打开，不崩溃。
- [ ] 轮盘移动高亮响应正常，松开中键后行为正确。
- [ ] 扇区锁定和解除锁定正常；Quick Copy 扇区不可锁定。
- [ ] 图片捕获开启时，图片预览大小和裁切稳定。
- [ ] PowerPoint 中中键呼出轮盘时，轻微滚动滚轮不会翻页或丢焦点。
- [ ] 如本轮涉及突破轮盘，按 `docs/EXTENDED_WHEEL_DESIGN.md` 和 `docs/ACCEPTANCE_TESTS.md` 的突破轮盘章节完成手测。
- [ ] 如本轮涉及应用模式，按 `docs/LAUNCH_COMPATIBILITY.md` 完成浏览器、文档和 WPS 降级路径手测。

## 提交与交付

- [ ] 提交前再次运行 `git diff --stat`。
- [ ] commit message 描述用户可感知的行为变化。
- [ ] 提交后确认 `git status` 干净。
- [ ] 若已打包，向使用者说明发布 exe 的路径、时间和大小。

## 更新下载器发布契约

- [ ] `release.config.json` 包含有效的 `downloadNodes`，且 `github-direct` 为启用的 `{url}` 官方回退。
- [ ] 如填写 `release.config.json.about`，已包含开发者和历史链接，且链接使用 HTTPS；该信息不会写入 `update.json`。
- [ ] `update.json` 已由共享 `prepare-release-assets` 脚本生成，节点列表、版本、Tag、EXE 大小和 SHA-256 与本地资产一致。
- [ ] 项目与 `DesktopUpdateKit` 两个 Git 工作区均已提交且干净，再执行 `publish-release.ps1`。
- [ ] Release 资产包含 Apache-2.0、NOTICE、第三方声明和 DesktopUpdateKit MIT 文本。
- [ ] 确认发布命令只上传 Release 资产；源码仓库通过独立、明确的 Git push 发布。
