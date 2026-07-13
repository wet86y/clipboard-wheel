# 参与贡献

欢迎通过 Issue 和 Pull Request 参与项目。

提交贡献即表示你同意按照本仓库的 Apache License 2.0 授权该贡献。请只提交
你有权授权的代码和资源；引入第三方内容时必须在 Pull Request 中说明来源和
许可证，并同步更新 `THIRD-PARTY-NOTICES.md`。

克隆时请初始化子模块：

```powershell
git clone --recurse-submodules <repository-url>
```

提交前运行：

```powershell
.\scripts\run-self-check.ps1
```

修改 `DesktopUpdateKit` 时应在其独立仓库提交并推送，再在本仓库更新
`shared/DesktopUpdateKit` 指向的提交。不要提交 `build` 或 `artifacts` 中的生成物。
