# 精打细算 / Resource Saver v1.0.1

作者：liuxin  
目标：Terraria Android 1.4.5.6.4 / TEFKernel / arm64

## v1.0.1 修复

v1.0.0 在手机版原版 Terraria 上只成功安装了 1 个 Hook。原因是首版误用了几个 tModLoader 扩展/包装方法，并且把 Player.AddBuff 参数数量写成了 4。

v1.0.1 改为尽量只依赖原版稳定成员：

- Player.ResetEffects()
- Player.Update(int)
- Player.AddBuff(int, int, bool)
- Player/Item 原版字段

同时修正 TEFKernel Prefix 返回值：`false` 才是继续执行原方法，`true` 会跳过原方法。

## 功能

1. 普通战斗弹药：使用 Terraria 原版 `ammoCost80` 机制，额外 20% 节省。
2. 火箭/高级弹药：使用原版 `huntressAmmoCost90` 机制，额外 10% 节省。
3. 魔法武器：魔力消耗 -15%。
4. 自然魔力恢复：恢复累计速度 +20%。
5. 召唤/哨兵：魔力消耗 -25%。
6. 手持有伤害的近战武器：治疗药水冷却恢复速度 +10%。
7. 常用战斗 Buff：长时间应用持续时间 +20%。
8. 鱼饵：检测到本次真实消耗 1 个鱼饵后，20% 概率返还。

## 构建

上传整个项目到 GitHub 后，Actions 使用 Android NDK r28c 编译 arm64：

`libResourceSaver.android.arm64.so`

并自动组装：

```
ResourceSaver/
├── Manifest.json
├── Info.json
├── mymod.json
└── Resources/
    └── lib/
        └── libResourceSaver.android.arm64.so
```

## v1.0.1 日志检查

这版正常情况下，Resource Saver 初始化阶段应连续安装多个 Hook，而不再像 v1.0.0 只有一个 Hook。重点检查：

```
Resource Saver v1.0.1 initializing
Combat hooks v1.0.1: ResetEffects=... Update=...
Combat buff duration hook v1.0.1: ready
Bait saver hook v1.0.1: ready
Resource Saver v1.0.1 initialized
```

其中 Hook ID 不应为 -1。
