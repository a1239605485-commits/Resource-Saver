# 精打细算 / Resource Saver v1.0.0

作者：liuxin  
目标：Terraria Android 1.4.5.6.4 / TEFKernel / arm64

## v1.0.0 功能

1. 普通战斗弹药：在原版节省判定之后，额外 20% 概率不消耗。
2. 火箭/高级弹药：额外 10% 概率不消耗。
3. 魔法武器：最终魔力消耗降低 15%，最低仍为 1。
4. 自然魔力恢复：恢复累计速度提高 20%。
5. 召唤/哨兵武器：最终魔力消耗降低 25%，最低仍为 1。
6. 手持有伤害的近战武器时：治疗药水冷却恢复速度提高 10%。
7. 常见战斗 Buff 的长时间应用持续时间提高 20%。
8. 鱼饵：在原版鱼饵消费之后，额外 20% 概率返还本次消耗的 1 个鱼饵。

## 构建

把整个项目上传到 GitHub，Actions 会使用 Android NDK r28c 编译 arm64：

`libResourceSaver.android.arm64.so`

并自动组装 TEFManager artifact：

```
ResourceSaver/
├── Manifest.json
├── Info.json
├── mymod.json
└── Resources/
    └── lib/
        └── libResourceSaver.android.arm64.so
```

## 首次测试重点

进入游戏后先检查 TEFManager 日志应看到：

```
Resource Saver v1.0.0 initializing
Combat hooks: ammo=... manaCost=... manaRegen=... meleePotion=...
Combat buff duration hook: ready
Bait saver hook: ready
Resource Saver v1.0.0 initialized
```

任何 hook 出现 `-1` / `failed`，把日志发回来即可定位对应功能，不需要重新猜 MOD 包结构。
