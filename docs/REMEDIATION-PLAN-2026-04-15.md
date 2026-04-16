# mod-ac-aegis 整改计划

日期：2026-04-15

## 目标

对照 [modules/mod-ac-aegis/docs/REVIEW-2026-04-15.md](modules/mod-ac-aegis/docs/REVIEW-2026-04-15.md)
核验问题是否仍然存在，并在不引入新误报、不削弱既有检出能力的前提下，优先整改真正成立的高风险项。

## 复核结论

### 已完成整改

1. event writer 不再直接并发读取可变配置对象，异步线程改为读取独立原子快照字段。
2. punish state 改为仅在持久态真正变化时才写 offense 表，observe 不再无条件 upsert。
3. 已补充 `ban_until` 回收索引的增量 SQL。
4. 离线上下文现在会继续执行风险衰减，再决定是否回收。
5. Debuff/Jail/Kick/Ban 风险阈值已接入处罚闸门。
6. login、logout、map change 已统一做全量检测状态复位。
7. Ban 天数已统一到 AutoAction.Ban 配置，旧的 Offense.Stage3BanDays/Stage4BanDays 已从模块读取逻辑中移除。
8. 未接线的 OnTransportTransition 入口已删除，避免继续形成假扩展点。
9. BuildMovementContext 与 geometry 查询补上了 IsInWorld 级别的防御。
10. delete 与 purge 现已在执行删除前等待 async event writer 完成删除边界之前的 in-flight batch，避免旧 event 在删除后被回写。

### 仍然成立，需后续处理

1. 当前 barrier 解决的是 writer 线程的旧 batch 回写问题，不会阻止在线玩家在删除之后产生新的合法 event。
2. Pull 与 knockback 的完整 server-issued expectation 仍缺稳定 hook 支撑。
3. geometry 真实 hit 点语义仍未重构，当前仍是保守近似。

### 已被当前代码覆盖或已部分失效

1. 合法飞行速度基线问题已修复，当前 [modules/mod-ac-aegis/src/AcAegisMgr.cpp](modules/mod-ac-aegis/src/AcAegisMgr.cpp) 会在服务端授权飞行状态下回退到 MOVE_FLIGHT 基线。
2. transport 或 taxi 边界不再仅依赖脚本显式 hook，当前常规移动入口已在首个边界变化包上执行状态同步与样本链复位。
3. geometry 的空 map 判空当前已经存在，复核报告中的空指针描述与现状不完全一致。
4. BuildMovementContext 已有空指针防御，但仍应补上 IsInWorld 级别的生命周期保护。

### 不按复核原建议直接硬修

1. Pull 期望窗口不能简单放进 OnPlayerSpellCast。这个 hook 是玩家施法侧，不是受控位移目标侧，直接给 Pull 建 mandatory expectation 容易把合法的外部拉拽反过来打成 IgnoredPull 误报。
2. knockback grace 的彻底闭环需要服务端已下发位移事件的可靠 hook。当前只能做保守收紧，不能伪造一个并不存在的 server-issued expectation。

## 当前操作语义

1. clean 或 clear：只清理单个玩家的 offense、处罚状态和在线检测上下文，不删除 `ac_aegis_event` 历史。
2. delete：删除单个玩家的 offense 与 event 数据，并移除在线上下文。对仍在线的玩家来说，删除之后触发的新证据仍会重新生成新记录。
3. purge：全局清空 offense 与 event 表并清空内存上下文。对仍在线的玩家来说，purge 之后触发的新证据仍会重新生成新记录。
4. clean 之所以看起来稳定，是因为它本来不删除 event 表，因此不存在“旧 event 被异步 worker 回写回来”的现象。

## 执行顺序

### Phase 1

1. 修复 event writer 配置并发读取。已完成。
2. 让 punish state 只在持久态真正变化时落库。已完成。
3. 补充离线 ban 回收索引迁移脚本。已完成。

### Phase 2

1. 将风险阈值接入 DetermineAction。已完成。
2. 统一 Ban 天数配置来源，并移除旧键探测。已完成。

### Phase 3

1. 修复 login、logout、map change 生命周期残留。已完成。
2. 为离线上下文补充风险衰减和自然回收。已完成。
3. 补充更激进的 player 或 map 生命周期防御。已完成第一步，后续仍需实机回归。

## 验证方式

1. 先跑编辑器静态诊断，确保新增修改没有直接编译错误。
2. 不主动触发全量 build，保持与仓库约定一致。
3. 保留 Pull 和完整 knockback expectation 为后续单独回归项，避免为了补漏报直接引入新的 dungeon 或 boss 技能误报。