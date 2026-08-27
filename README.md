# 精打细算 / Resource Saver v1.1.1

作者：liuxin  
目标：Terraria Android 1.4.5.6.4 / arm64 / TEFKernel

本版从已经在真机验证生效的 v1.0.3 stable 分支继续开发，加载结构、CMake 和核心 Hook 路线保持不变。

## 功能

1. 普通弹药：额外 20% 节省
   - Gel / Arrow / Bullet / Dart / Flare / Snowball
2. 特殊弹药：额外 10% 节省
   - Coin / Fallen Star / Rocket / Stynger Bolt / Candy Corn / Jack 'O Lantern / Stake / Nail 等
   - Sand / Solution 工具型弹药不额外节省
3. 魔法类魔力消耗：-15%
4. 召唤 / 哨兵魔力消耗：-25%
5. 自然魔力恢复累计速度：+20%
6. 手持近战武器时，Potion Sickness 恢复速度：+10%
7. 战斗类长时间 Buff：持续时间 +20%
8. 鱼饵：在原版决定真正消耗 1 个鱼饵后，再独立进行 20% 返还判定

## 稳定回退

HeldItem 分类如果在某一帧读取失败，不会中断整个 MOD：

- 弹药回退到 v1.0.3 已验证的 20% 节省；
- 魔力不再依赖 HeldItem：Player.Update 期间会临时降低物品栏内所有耗蓝物品的 Item.mana，结束后立即恢复；
- 仅“特殊弹药10% / 召唤25% / 近战专属药水冷却”暂时跳过该帧。

这样可以避免再次出现“一个 getter 失败导致所有功能都失效”。

## 诊断日志

卸载 MOD 时会输出：

- ammo20_frames / ammo10_frames
- mana15_frames / summon25_frames
- melee_potion_extra_ticks
- held_fallback_frames
- Buff extended 数量
- 检测到的鱼饵消耗次数 / MOD 实际返还数量

这些数值主要用于定位兼容性问题；ammo frames 是功能应用帧数，不伪装成“实际省下的弹药数量”。


## v1.1.1 魔力修复

1.4.5 将实际魔力支付流程拆分到了新的内部方法。v1.1.1 不再依赖 Player.manaCost 是否被这些路径读取，而是在 Player.Update 前临时降低 inventory 中所有 mana>0 物品的 Item.mana，Update 后恢复原值。这样不会永久改物品，也避免每帧重复乘算。
