# 精打细算 / Resource Saver v1.2.1

作者：liuxin

## 本版原则

- 所有实际功能逻辑以已经验证有效的 **v1.1.0 precise** 为基线。
- 魔力消耗仍使用 v1.1.0 的 `Player.manaCost` 方案，没有换成 v1.1.1 的 Item.mana 临时修改方案。
- 只新增配置层和中文游戏内设置 UI。
- 不再使用 `Terraria.IngameOptions`。Android 1.4.5.6.4 日志已确认该类型无法由 TEFKernel 找到。

## 游戏内设置入口

进入世界后打开背包，在背包界面右上区域显示：

`资源节省设置【打开】`

点击后可独立设置：

1. 总开关
2. 普通弹药节省 20%
3. 特殊弹药节省 10%
4. 魔法魔力消耗 -15%
5. 召唤/哨兵魔耗 -25%
6. 自然回魔速度 +20%
7. 近战药水冷却 +10%
8. 战斗增益时长 +20%
9. 鱼饵节省 20%

所有游戏内文字均为中文。

## 配置保存

设置会立即保存到 MOD 的 private_dir/config.ini，下次启动自动读取。

## UI Hook 兼容顺序

1. `Terraria.Main.DrawInventory()`
2. `Terraria.Main.DrawInterface_27_Inventory()`
3. `Terraria.Main.DrawInterface(GameTime)` + `Main.playerInventory`

这样避免依赖手机版中缺失的 `Terraria.IngameOptions`。

## 编译

GitHub Actions 与 v1.1.0 保持一致，最终使用：

`libResourceSaver.android.arm64.so`
