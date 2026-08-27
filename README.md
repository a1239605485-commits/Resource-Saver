# Resource Saver v1.0.2

作者：liuxin
包名：celso.resourcesaver
目标：Terraria 1.4.5.6.4 / Android arm64 / TEFKernel

## v1.0.2 关键修复

- 不再把 Player.selectedItem 当字段读取；改用 Player.HeldItem 属性 getter。
- ResetEffects Hook 不再依赖不存在的 selectedItem 字段，因此弹药/魔力逻辑可以真正安装。
- 魔耗分类改为 Item.mana > 0，召唤/哨兵单独 -25%，其它耗蓝物品 -15%。
- Buff +20% 不再修改 AddBuff 参数，改为 Player.Update 后直接修改 buffTime 数组。
- 鱼饵节省改为 Hook Projectile.FishingCheck()，在实际钓鱼检查前后比较鱼饵数量。
- 保持普通弹药 +20%、特殊弹药 +10%、自然回魔 +20%、近战药水冷却恢复 +10%。

## 预期 Resource Saver Hook 数量

Resource Saver 初始化阶段应成功安装 4 个 Hook：
1. Player.ResetEffects
2. Player.Update（战斗资源）
3. Player.Update（Buff 追踪）
4. Projectile.FishingCheck（鱼饵）

如果日志只出现 1~2 个 Hook，说明仍有目标方法/字段不匹配，应根据日志继续定位。
