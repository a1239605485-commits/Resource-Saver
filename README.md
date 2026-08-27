# Resource Saver v1.2.3 中文设置 UI 修正版

- 功能核心：保持 v1.1.0 precise 逻辑，只增加配置开关判断。
- UI：不再 Hook IngameOptions / DrawInventory / DrawInterface。
- 新入口：Hook `Terraria.Main.Draw(GameTime)`，在其 Postfix 中自行 `SpriteBatch.Begin/End` 绘制中文按钮和设置面板。
- 正常情况下打开背包后右上角显示 `【资源节省设置】 点击打开`。
- 如果 `playerInventory` 字段在某构建不可读，自动退化为一直显示入口。
- 配置保存在 MOD private_dir/config.ini。
