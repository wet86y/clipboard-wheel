# GitHub Release 资产约定

公开仓库 `wet86y/clipboard-wheel` 保存 Apache-2.0 授权的项目源码、文档和 Release 资产；
MIT 授权的 `DesktopUpdateKit` 源码由独立仓库维护，并通过 submodule 固定提交。

## Release 命名

版本标签使用：

```text
v1.0.0
```

版本号必须与 `Directory.Build.props` 中的 `Version` 保持一致。

## 资产

每个稳定版 Release 至少上传：

```text
super-middle-key.exe
super-middle-key.exe.sha256
update.json
LICENSE
NOTICE
THIRD-PARTY-NOTICES.md
DesktopUpdateKit-LICENSE.txt
```

可使用以下脚本准备资产：

```powershell
.\scripts\build-release.ps1
.\scripts\prepare-release-assets.ps1 -Version 1.0.0
```

资产会生成到：

```text
build\release-assets\v1.0.0
```

`build\release-assets` 是本地发布准备目录，不应提交到公开仓库。

## 共享发布工具

发布脚本的实际实现位于 submodule：

```text
shared\DesktopUpdateKit\tools
```

当前项目的 `scripts` 目录只保留项目入口和参数转发。项目配置位于根目录 `release.config.json`，其他项目不得复用本项目的仓库名、程序名或资产名。

## 当前安全边界

当前版本使用 SHA-256 校验完整性，但尚未使用 Windows 代码签名证书。发布说明中不得把 SHA-256 描述为发布者身份认证。
