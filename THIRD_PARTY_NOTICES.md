# 第三方材料

- `vendor/debs/`：搜狗 NG / CPIS 的原始二进制、词库和图标；权利归各自权利人。其许可文件保存在原包中，并将包内 copyright 复制到 `vendor/notices/`。本项目未确认这些包的公开再分发授权，MIT 不覆盖这些文件。
- Fcitx5、GLib/GIO、libcurl 是构建与运行依赖，不将其源码或构建工具复制入本仓库；各自遵循上游许可证。
- 云候选二进制协议的探索参考了 [rime-wenyun](https://github.com/xing133/rime-wenyun)。本仓库使用独立的 C++ 请求、解析及异步调度实现，不包含该项目 Lua 源码。
- 本项目并非搜狗或 openKylin 官方产品，不代表官方背书。云端接口可能变化或停止服务；本地引擎仍可单独使用。
