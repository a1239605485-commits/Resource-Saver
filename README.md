# Resource Saver v1.2.4 — 安全诊断版

此版本继续使用 v1.1.0 的功能核心和独立配置开关逻辑，但**不安装任何游戏内绘制 Hook**。

原因：v1.2.3 已确认 `SpriteBatch.End()` UI Hook 会在 Android 1.4.5.6.4 启动阶段触发 SIGABRT。

v1.2.4 的任务：

1. 恢复可以正常进入游戏的稳定状态。
2. 保留 config.ini 以及所有功能开关逻辑。
3. 启动时枚举 Terraria.Main / Terraria.Utils / SpriteBatch 中真实存在的 Draw、Inventory、Interface、Menu、UI 等方法。
4. 日志中的 `UI-METHOD` 行用于制作下一版真正匹配 Android 的中文按钮界面。

测试后请导出 TEFManager 日志。重点信息以 `UI-METHOD` 开头。
