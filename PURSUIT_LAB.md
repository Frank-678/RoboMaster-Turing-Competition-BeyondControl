# 同时行动追逃实验工具

本工具是离线研究原型，正式机器人没有导入它，比赛参数没有改动。用于验证“当前射击、封出口、侧移、防御、经济路线”在一个明确的短局面里如何取舍。

## 运行

```bash
python3 tests/pursuit_lab_test.py
python3 tools/eval_pursuit_lab.py --out build/pursuit-probes
python3 tools/pursuit_lab.py build/pursuit-probes/diagonal-edge-cap300.scene.json --seed 20261029
```

运行本身只用 Python 标准库。可选的独立线性规划审计需要 NumPy/SciPy：

```bash
python3 tests/pursuit_certificate_check.py --out build/pursuit-lp.json
```

已有场景 runner 编译后，可对照物理结算；替换为本机实际 runner 路径：

```bash
python3 tests/pursuit_differential.py --engine build/scenarios/ACTUAL_BUILD_ID/runner --count 240 --out build/pursuit-differential.json
```

这对照的是仓内自建 arena，**不是原版 ROS 裁判差分**。不需要真实模型调用。

## 明确支持的局面

输入 `schema=1`，必须显式提供 `complete_local_model=true`、地图尺寸、全部可走格、恰好两辆初始存活普通车的位置/HP、`baseline_action` 和 `score_damage_cap`。可提供自己 R/D、剩余资源、已知经济目标、估值权重及搜索预算。

不提供敌方得分字段。任务计分、增益、出生保护、事件、第三辆车和局部视野不完整均未实现，不能删掉这些事实来冒充受支持局面。当前适用的是封闭离线微型模型，**不能直接把真实感知标成 complete_local_model 来上场**。未来接入需要与公开感知一致的适配器，并对未观测的敌车/能力/地形保留不确定性或明确回退。

最长三次动作。初始两车均存活，死亡后不会在这段窗口内重新参战；不处理已有复活倒计时。攻击先于移动、双方攻击同时结算、每次死亡扣60D并最低归零、击杀加60D、争格/不能交换位置、障碍不挡普通攻击、存活者采集均显式实现。物理子集已与现有 arena 随机逐例对照。

活车的我方候选包括 DEFEND、四向 ATTACK 和空格 MOVE；我方省略提交在本普通模型中被 DEFEND 弱支配，不作为默认候选。对手另外保留不提交情景。先前 `defending` 状态不会变成下一 Tick 的防御承诺。

## 搜索和证书

每个节点构造完整的“我方动作 × 对手动作”矩阵；下层矩阵的值回传上层，而非先知道对手本 Tick 动作再作答。矩阵用独立实现的 fictitious play 求候选混合策略，并直接计算：

\[
L=\min_b\sum_a x_a Q^-_{ab},\qquad U=\max_a\sum_b Q^+_{ab}y_b.
\]

`Q-`、`Q+` 为下层值的上下界。对给定有限模型的最优值有 `L ≤ V ≤ U`；它是数值证书，**不是对整场比赛、隐藏信息或晋级概率的保证**。迭代没收敛时仍返回实算 gap，不把“迭代1024次”等同精确均衡。另用独立 HiGHS LP 对80个矩阵及20棵两 Tick 树核验上下界。

从深度1开始迭代加深。更深层未完成时，整层丢弃，保留最后一层完整结果；连深度1都无法完成则返回明确给定的基线，不臆造动作。节点和时间预算共享，时间在动作组合/矩阵求解间检查，受操作系统调度影响，属于软预算。

`policy` 是搜索候选的分布；`certificate` 给候选值和基线值的独立界。只有候选下界超过基线上界加 `override_margin` 才报告 `candidate_exceeds_baseline_in_model`，否则保留基线。`sampled_action` 遵守该选择；单次固定 seed 样本不具有混合策略的期望收益保证。上线时若采用混合策略，不能取最大概率动作替代采样、也不能公开固定可预测种子后仍声称不可利用。

## 估值和计分版本

叶子使用自己的增量资源分/伤害分、自己的 HP 变化、死亡机会成本和沿已知可走图到经济目标的进展。默认权重只是未调参示例：HP 0.03、死亡3.0、目标进展1.0、切换余量0.05。HP/目标项不是官方加分；尚未实现任务资格和名次的价值。

资源公式固定为30分/120R。伤害分母必须由输入明确：仓内 `practice/rules.txt` 自称初版、文本为300，而已有策略/arena采用500。工具两种都测，**没有替正式比赛裁定版本**；使用者先锁定赛事最终规则及本场裁判。

## 本轮验证范围

23项单元测试通过；240个普通结算随机样本与已构建 arena 一致；20个设计场景都完成两 Tick；独立 LP 的80个矩阵和20棵树检查通过。没有运行新的四人完整比赛，也没有声称提高晋级率。实际原始产物位于分析仓 log 分支本轮 `pursuit-lab-2026-10-29/` 目录，分析和审查在对应分析文档。

只选择分析工具可直接采纳；把它接入正式策略是另一次行为变更，需保留关闭开关并做成对实战验证。它不授权打开已有 `tactical_search`，两者算法不同。

## 审查反例复跑

```bash
python3 tools/review_handoff_probe.py /path/to/deepseek-checkout --out build/review-probes.json
```

该工具针对 eacb963 的 M1/M4 对手池、Ready 等行数截断和构建路径含空格三个问题。使用临时目录并 mock 构建，不运行 ROS 或 colcon，不改被审查源文件。后续修正版本应保留源码哈希并重核结果；现有工具的目的在于提供可复现证据，不是长期兼容所有未来接口。
