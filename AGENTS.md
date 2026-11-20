# Repository Agent Guide

本文件约束在本仓库中工作的自动化 Agent。具体架构、构建和发布细节以仓库内对应文档为准，不在此重复维护。

## Project scope

- 当前产品实现位于 `native/`，目标平台为 Windows x64，使用 C++20。
- `archive/net8-final` 仅作为兼容性审计基线；除非任务明确要求维护该基线，否则保持只读。
- 开始工作前先确认任务属于主仓库还是 `shared/DesktopUpdateKit` 子模块。

## Repository boundaries

- `shared/DesktopUpdateKit` 是独立 Git 仓库和 submodule。
- 修改 updater 时，应在子模块仓库中完成分支、测试、提交和推送，再回到主仓库更新 gitlink。
- 主仓库只能引用已经存在于子模块远端、不会被改写的提交。
- 子模块合并、主仓库 gitlink 更新、主仓库提交与推送是不同操作，不互相隐含授权。

## Before changing code

- 先检查 `git status --short --branch` 和 `git submodule status`，确认真实仓库边界及已有修改。
- 保留用户已有的未提交修改，不重置、不覆盖无关内容。
- 根据任务先阅读对应文档：
  - 构建与输出：`docs/BUILD_OUTPUT_CONTRACT.md`
  - updater 架构：`docs/SHARED_UPDATE_ARCHITECTURE.md`
  - 发布流程：`docs/RELEASE_CHECKLIST.md`
  - 人工验收：`docs/ACCEPTANCE_TESTS.md`
- 审计、解释和问题清单任务默认只读；除非用户明确要求，否则不要顺带修复。

## Validation

- 主仓库完整门禁：`./scripts/run-self-check.ps1`。
- DesktopUpdateKit 独立门禁：`./shared/DesktopUpdateKit/native/scripts/build-native.ps1 -Configuration Release`。
- 根据变更范围选择最小充分验证；涉及正式构建、打包、更新或跨模块行为时运行完整门禁。
- 短超时不等于构建失败；先确认进程和实际输出，再决定是否修改构建逻辑。
- 若 EXE 文件被运行中的进程锁定，先确认锁定状态，关闭后重跑同一验证，不要据此改动构建规则。
- 最终报告必须区分已执行并通过、已执行但失败、以及未执行的验证。

## Generated files and documentation

- 根目录 `README.md` 是 GitHub 项目主页。只有用户明确允许时才可主动修改，常规文档更新、发布和版本号调整均不包含此权限。
- 同步远端时通过 Git 合并保留远端 `README.md` 的内容。若存在需要改写主页内容的冲突，先请用户确认处理方式。
- 不直接编辑或提交 `build/`、`artifacts/` 中的生成物，除非任务明确要求处理发布资产。
- 改变构建输出或路径约定时，同步更新 `docs/BUILD_OUTPUT_CONTRACT.md`。
- 改变 updater 集成或仓库边界时，同步更新 `docs/SHARED_UPDATE_ARCHITECTURE.md`。
- 优先使用仓库已有脚本，不用临时命令复制其构建或发布逻辑。

## Release safety

- 普通开发任务不授权创建 Tag、GitHub Release、上传资产、修改公开更新通道或推送分支。
- 发布相关工作必须核对版本号、`release.config.json`、`update.json`、发布说明和最终 EXE 元数据的一致性。
- 正式发布前执行 `docs/RELEASE_CHECKLIST.md`，并验证真实公开更新入口；本地文件不能代替线上结果。
- 用户只要求发布资产时，不把任务扩展为源码发布、许可证清理或其他公开化工作。

## Scope discipline

- 用户限定为“只构建/测试项目本身”时，不进行宿主集成、gitlink 更新或发布操作。
- 不因修复代码而自动获得提交、推送、合并或发布权限。
- 兼容性路径应以测试和可追溯证据为依据；没有新证据时，不删除已验证的兼容行为。
