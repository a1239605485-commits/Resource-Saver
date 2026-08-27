# Resource Saver v1.2.5 - 安全 UI 精确探测版

- 功能核心保持 v1.1.0。
- 不安装任何 UI 绘制 Hook，不会重复 v1.2.3 的 SpriteBatch 崩溃。
- 启动时使用 `patchlib_type_get_method()` 精确查询常见 Terraria UI/绘制方法。
- TEFKernel 自己会把成功查询写入 runtime 日志，例如 `Found method 'DrawInterface' at ...`。
- 同时把查询结果写到 MOD 私有目录的 `ui_probe.txt`。
