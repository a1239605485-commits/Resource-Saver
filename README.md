# Terraria Resource Saver v1.2.7

安全中文设置 UI 修正版。

- 核心功能继续使用已验证的 v1.1.0 逻辑。
- 配置总开关 + 8 个独立开关继续保存到 `private_dir/config.ini`。
- 不再调用 `Utils.DrawBorderString`、`SpriteBatch.DrawString` 或任何需要 `Vector2/Color` 结构体参数的反射绘图函数。
- 中文设置界面改用 Terraria 原版 `Main.MouseText(...)` 绘制。
- UI 仅安装 Postfix，不跳过任何原版 UI 方法。
- 优先挂 `DrawInterface_33_MouseText`，找不到时回退到 `DrawInterface_27_Inventory`。
- 只有原版 SpriteBatch 已经 Begin 时才显示设置 UI。

打开背包后应看到 `【资源节省设置】 点击打开`。
