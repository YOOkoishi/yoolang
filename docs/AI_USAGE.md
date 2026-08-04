# AI 使用声明

本文件记录本仓库中人工智能辅助开发的实际范围，供参赛队复核和提交时披露。依据为
2026 年全国大学生计算机系统能力大赛编译系统设计赛的 AI 使用披露要求；规则版本或
组委会通知如有更新，以提交当日的官方版本为准。

## 使用工具

- 工具名称：OpenAI Codex。
- 使用日期：2026-08-04。
- 使用方式：在参赛队给定的本地仓库中分析、生成补丁、运行构建与测试，并整理验证
  记录；未把测试用例名称、输入或答案接入编译器运行时优化决策。

## AI 生成或修改范围

`58d15c1..cd047e6` 的通用性能优化任务、其已提交正确性修复、基于 `5f97e68` 的 OIR
动态 GEP 别名健全性修复，以及基于 `f7e5ac0` 的 bit-digit 非负快路/原循环回退修复由
Codex 辅助完成，范围为：

- 编译器生产代码：`include/oir/OIRScalarOpt.h`、`src/oir/OIRAnalysis.cpp`、
  `src/pass/mir/MIRLocalCSEPass.cpp`，以及 `src/pass/oir/` 下本任务涉及的 affine
  recurrence、bit-digit idiom、GVN、guarded-call CSE、inline、local simplify、loop
  transforms、optimization pipeline、scalar-opt utilities 和 OIR-to-MIR lowering 文件。
  本次动态 GEP 修复具体修改 `src/oir/OIRAnalysis.cpp`，使未知动态字节位置保守返回
  `MayAlias`；本次 bit-digit 修复具体修改 `src/pass/oir/OIRBitDigitIdiomPass.cpp`，仅在
  两个输入均非负时执行直接位运算，否则保留原循环。
- 测试：本任务在 `test/easy/`、`test/functional/`、`test/ir/` 中新增或修改的回归、
  差分、IR 结构和端到端用例，包括 guarded-call 指针别名正确性用例，以及
  `test/easy/oir_dynamic_gep_alias.{sy,in,out}`、`test/ir/oir_dynamic_gep_alias.sy` 和
  `scripts/oir_infra_tests.py` 中的直接别名分析回归。
- 文档与验证：本任务记录、AI 使用声明及由测试/性能脚本生成的本地验证报告。

“辅助完成”包括生成新代码、修改既有代码、提出修复方案和执行验证。Git 历史不能证明
的人工逐行修改不在此声明中推定为已发生。

## 人工输入、修改与复核状态

- 已有人工输入：参赛队提出“修复违反比赛规则的优化，并在合法前提下保留收益”的
  目标与约束，并决定是否采纳工作区补丁。
- 可由仓库证据确认的人工逐行代码修改：无记录。为避免虚假披露，当前按“未记录”
  处理。
- 人工复核：待参赛队成员完成。AI 执行的构建、测试和子代理审查不计作人工复核。

提交比赛前，参赛队应逐项检查生成代码、补充实际发生的人工修改，并由队员填写：

- [ ] 已核对上述工具名称和生成/修改范围。
- [ ] 已审阅生产代码及其通用性、语义正确性和比赛规则符合性。
- [ ] 已据实补充人工修改内容；如确无逐行修改，明确记录为“无”。
- 复核人：`待填写`
- 复核日期：`待填写`
- 人工修改说明：`待填写（无修改时填写“无”）`

## 技术合规边界

本任务中的优化只依据类型、IR 结构、SSA/use-def、支配关系、循环结构、别名/内存效果
证明和通用成本模型。优化代码不得读取源文件路径，不得按测试名、用户函数名、输入、
输出或已知答案选择变换；证明不完整时必须保守地保留原计算。

官方入口：<https://compiler.xtnl.org.cn/>；2026 官方资料仓库：
<https://gitlab.eduxiji.net/csc1/nscscc/compiler2026>。
