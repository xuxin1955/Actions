# HALO 通用参数

本目录存放 SY108 / UDX710 AP 与 CP 基带进行 HALO 握手所需的平台通用参数，构建时随 `udx-cpnv` 包安装至 `/etc/udx-cpnv-warm/`（权限 `0600`）：

| 文件 | 长度 | 格式说明 |
|---|---:|---|
| `halo-private.bin` | 256 字节 | 128 字节大端 RSA 模数 `n`，后接 128 字节大端私有指数 `d` |
| `halo-verification.bin` | 136 字节 | 128 字节对端 RSA 模数，后接 8 字节 `featureList` 参数 |
