# Resource Saver v1.2.6

基于已验证的 v1.1.0 功能核心。

本版 UI 根据 Android Terraria 1.4.5.6.4 真机日志确认的 `Terraria.Main.DrawInterface_27_Inventory` 实现。
不主动调用 SpriteBatch.Begin/End；仅当 `_beginCalled == true` 时绘制中文设置按钮和面板。

打开背包后显示“资源节省设置”。
