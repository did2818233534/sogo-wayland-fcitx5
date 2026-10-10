# sogo-wayland-fcitx5

让搜狗 NG 的拼音引擎通过原生 Fcitx5 候选栏在 Linux / Wayland 环境中工作。

本项目提供 **开源的 Fcitx5 适配层**，通过会话 D-Bus 调用搜狗 NG / CPIS 引擎，不使用搜狗旧候选窗。搜狗引擎本身仍是闭源软件。仓库同时归档了本次使用的原始安装包，便于固定版本部署，见 [第三方说明](THIRD_PARTY_NOTICES.md)。这不是搜狗官方项目。

## 支持范围

- 已开发和运行的环境：Ubuntu 24.04、x86_64、GNOME Wayland、Fcitx5 5.1.7。
- 适配 CPIS / 搜狗 NG 10.0.9.0.6；具体包版本和校验值见 [vendor](vendor/README.md)。
- 当前面向单用户桌面，安装到 `/opt/sogou-ng`。不支持多用户同时共享引擎或与其他 CPIS 安装并存。
- Fcitx D-Bus 自动输入检查通过；**没有完成 Chrome 原生 Wayland 键盘端到端人工验证**。具体应用能否输入也取决于其输入法协议和桌面环境配置。

## 功能与按键

| 操作 | 行为 |
| --- | --- |
| 字母键 | 拼音输入 |
| 空格 / 1–5 | 提交高亮候选 / 指定候选 |
| 右 / 下 | 下一个候选；越过末项后到下一页首项 |
| 左 / 上 | 上一个候选；越过首项后到上一页末项 |
| `,` / `.` 或 PageUp / PageDown | 上一页 / 下一页 |
| Escape | 取消当前输入 |
| Enter | 提交原始拼音 |
| 标点 | 默认 ASCII 英文标点；无预编辑时逗号句号正常输入 |

默认一页 5 个候选。适配层不使用 Shift 切中英文，但 Fcitx5 的全局 Shift 快捷键仍需自行关闭，见安装步骤。

云候选通过 HTTPS 异步补充，标记 `☁`。默认开启，发送当前正在转换的拼音，不发送周围文本、已提交文本或真实设备 ID。停止输入 60 ms 后请求，复用连接并取消过期请求；最多缓存 256 个查询，仅在内存中保存。网络失败时使用本地候选；快速按空格不会等待云端。详见 [云候选说明](docs/cloud.md)。

## 构建

在 Ubuntu 24.04 上安装构建和桌面运行依赖：

```bash
sudo apt update
sudo apt install build-essential pkg-config python3 \
  libfcitx5core-dev libfcitx5config-dev libfcitx5utils-dev \
  libglib2.0-dev libcurl4-openssl-dev \
  fcitx5 fcitx5-config-qt fcitx5-frontend-gtk3 fcitx5-frontend-qt5 \
  libgtk-3-0t64 libxtst6
make -j"$(nproc)"
make check
```

输出为 `build/libsogou-native.so`。`make clean` 只移除构建目录。

## 安装

先确保当前桌面已使用 Fcitx5。初次从 IBus 切换时，可运行 `im-config -n fcitx5`，然后注销并重新登录。不要同时启动多个输入法框架。

在本项目目录运行：

```bash
# 只校验归档，无系统改动
python3 scripts/deploy.py install --check
# 固定版本解包到 /opt/sogou-ng，不执行 deb 包的维护脚本
sudo python3 scripts/deploy.py install --user "$(id -un)"
# 以下命令以当前桌面用户运行，不能 sudo
python3 scripts/user-setup.py install
```

系统安装会在 `/etc/cpis`、`/usr/share/cpis`、`/var/lib/cpis` 和 `/usr/lib/x86_64-linux-gnu/` 建立到 `/opt/sogou-ng/root` 的链接。遇到已有安装或路径冲突就停止，不覆盖原文件。用户注册脚本同样拒绝覆盖已有配置；不会修改你的 Fcitx5 profile 或全局按键。

接下来：

1. 注销并重新登录，使 Fcitx5 重新加载插件。
2. 打开 `fcitx5-configtool`，取消“仅显示当前语言”，添加 **Sogou Native / 搜狗拼音（Wayland 适配）**。
3. 在“全局选项”中将“额外的输入法切换键”设为禁用，去掉 Shift 切换；按需保留 Ctrl+Space 切换中英文。
4. 在文本编辑器中输入 `nihao`，用方向键、空格和翻页键检查候选。联网时可试 `nohao`，云端曾返回“你好”，但服务结果并不保证恒定。

源码仓库与安装目录相互独立。移动或删除源码目录不会破坏已经完成的安装。

### 已经有本次开发版安装

安装脚本会拒绝覆盖旧的 `/opt/sogou-ng`，包括原先手动安装的开发版本。**不要为重装直接删除该目录**：先备份个人词库、检查旧版本的链接清单，并用原版本的卸载方式移除。这里的新卸载脚本只接受由本项目脚本生成的 `deployment.json`。

## 卸载

先在 `fcitx5-configtool` 删除本输入法并切换到其他输入法，退出 Fcitx5，再运行：

```bash
fcitx5-remote -e
python3 scripts/user-setup.py uninstall
sudo python3 scripts/deploy.py uninstall
```

脚本只删除仍指向本安装的链接。个人词库保留在 `/opt/sogou-ng/userdata`，请按需备份；确认不再需要后可自行删除剩余目录。最后注销重新登录恢复其他输入法。全局按键没有被安装脚本改写，无需还原。

## 夜间模式

使用 Fcitx5 自带深色主题，无需重启输入法：

```bash
sudo apt install python3-dbus
/usr/bin/python3 scripts/appearance.py dark    # 始终深色
/usr/bin/python3 scripts/appearance.py auto    # 跟随系统明暗模式
/usr/bin/python3 scripts/appearance.py light   # 始终浅色
/usr/bin/python3 scripts/appearance.py restore # 恢复首次修改前的主题设置
```

首次操作会备份原主题设置。该设置影响共用 Fcitx5 Classic UI 的输入法，不只搜狗；由桌面 Kimpanel 或应用自行绘制的候选窗可能不受控制。自动模式依赖桌面提供明暗模式通知。

## 排查问题

```bash
systemctl --user status sogou-ng-engine.service
journalctl --user -u sogou-ng-engine.service -b --no-pager
fcitx5-remote -n
ldd /opt/sogou-ng/libsogou-native.so
```

- 没有候选：确认服务正常、没有另一个 CPIS 实例占用端口、当前选中 `sogou-native`。
- Chrome 不接受输入：先确认普通文本编辑器可以输入，再检查 Chrome 实际使用的 Wayland/XWayland 后端和 Fcitx5 前端；本项目不会修改浏览器启动参数。
- 没有云候选：可能是网络、接口变化、过快提交，或使用方向键后抑制了迟到的结果；本地输入仍可用。
- 图标未刷新：注销登录，或刷新当前主题图标缓存。图标由随附引擎包提供。

## 开发与验证

`src/` 是适配层，`data/` 是注册配置，`scripts/` 是部署工具，`vendor/` 是固定第三方包。

[测试说明](docs/testing.md) 记录自动验证覆盖和局限。CI 仅构建适配层、运行部署工具测试并校验归档，不启动闭源引擎，不连接云服务。

当前仍是实验版本：本地引擎 D-Bus 调用有 700 ms 超时；闭源引擎卡顿可能短暂影响输入。尚未实现官方账号登录、词库同步、皮肤、全平台兼容或完整的搜狗官方云功能。

## 许可证

原创适配层、脚本和文档采用 [MIT](LICENSE)。`vendor/` 中的第三方包、词库、图标及版权材料不受 MIT 覆盖；这是一份含闭源依赖归档的开源适配层仓库，不是完全开源的搜狗输入法。归档包中的部分 copyright 文件含模板内容，不能据此认定整个引擎获得开源授权，详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
