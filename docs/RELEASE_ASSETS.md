# GitHub Release 资产约定

稳定版标签使用 `v<major>.<minor>.<patch>`，并必须与 `native/CMakeLists.txt` 中的
`SMK_VERSION` 一致。

每个正式 Release 上传：

```text
super-middle-key.exe
super-middle-key.exe.sha256
update.json
LICENSE
NOTICE
THIRD-PARTY-NOTICES.md
DesktopUpdateKit-LICENSE.txt
```

准备流程：

```powershell
.\scripts\run-self-check.ps1
.\scripts\prepare-release-assets.ps1 -Version 2.0.0 -ReleaseNotes "..."
```

资产生成到 `build\release-assets\v2.0.0`。程序资产使用 ASCII 名称，应用落地名称仍为
`超级中键.exe`。`update.json`、SHA-256 和许可证资产由 DesktopUpdateKit 共享工具生成；
SHA-256 只证明完整性，不代替代码签名。
