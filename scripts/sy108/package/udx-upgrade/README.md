# SY108 OpenWrt 平台升级支持（`udx-upgrade`）

`udx-upgrade` 提供 SY108 平台的 OpenWrt `sysupgrade` 校验、刷写钩子与启动状态确认服务：

- **镜像校验（`image-check`）**：结合 `fwtool` 元数据与 `components/boot-format/` 解析器，校验 `sysupgrade.bin` 中的 `boot` Capsule 头部、CRC32 及目标 UBI 卷容量。
- **平台升级流程**：在切换至 `procd` RAM 升级环境前有序停止 CP/NV 服务并卸载 USB Gadget，延长 PMIC 升级保活窗口，使用 `ubiupdatevol` 写入 `boot` 与 `rootfs` 卷，调用 `nand_restore_config` 保存配置归档，并将 `misc` 分区启动状态置为 `pending`。
- **启动确认服务**：主系统启动且核心管理功能就绪后，将 `misc` 分区的试启动状态（`trial`）确认为正式就绪（`ready`）。
- **Recovery 兼容**：独立 Recovery 复用同一套 `sysupgrade` 流程；勾选保留配置时，通过只读挂载已安装的主系统提取配置备份。

