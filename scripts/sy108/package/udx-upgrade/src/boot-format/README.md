# SY108 启动镜像与事务格式（`boot-format`）

本目录定义 SY108 启动 Capsule 镜像格式与 `misc` 分区启动事务状态结构，供用户态升级校验工具与主机打包脚本使用：

- `misc-state.c` / `misc-state.h`：解析与生成 1 MiB `misc` 分区启动环境块（CRC32 校验与 `udx_boot_state` 的 `ready` / `pending` / `recovery` / `trial` 状态转换）。
- `capsule.c` / `capsule.h`：校验 DHTB/Android/SPRD 兼容外层头、`XGZ1` 引导元数据、目标卷容量限制及各段（gzip 流、Linux `Image`、ramdisk、DTB）长度与 CRC32。
- `tools/image/capsule.py`：构建 4096 字节头部前缀（偏移 `0x80` 为 `XGZ1` 元数据，偏移 `0x1000` 为 gzip 压缩内核载荷）。

对应的 U-Boot 端 Capsule 解析、RTC 启动阶段协同与镜像引导实现在 `sy108-uboot-spl` 仓库的 `src/u-boot/common/loader/sy108/` 中维护。
