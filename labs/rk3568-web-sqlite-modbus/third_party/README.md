# 第三方依赖与基线

## 已用依赖

| 依赖 | 已验证基线 | 来源与许可 |
| --- | --- | --- |
| libmodbus | 3.2.0 | [官方仓库](https://github.com/stephane/libmodbus)，LGPL-2.1-or-later；清单见libmodbus.json，附COPYING.LESSER |
| SQLite | 板端3.21.0；头文件/库须匹配ABI | [官方说明](https://sqlite.org/copyright.html)，public domain |
| nginx | 板端已有1.12.2，独立实例验证 | [官方源码许可](https://nginx.org/LICENSE)，BSD类许可 |
| Python | 板端3.8.6；Ubuntu/Windows测试工具 | [官方文档](https://docs.python.org/3/)，使用标准库 |

这些是实测基线，不是“最新版本”或持续安全维护承诺。原Boa/Lighttpd/GoAhead为需求候选；实现复用板端既有nginx，未替换系统80端口服务。源码包、SDK、系统镜像、第三方运行库和二进制不随本目录发布。

## 构建前提

主机与目标分别准备libmodbus前缀目录，头文件和库架构必须匹配。工具链为匹配板端的aarch64-buildroot-linux-gnu；SQLite开发头及库来自匹配环境。libmodbus源码下载SHA256已核对官方Release资产，不等于独立签名验证；本次不自动下载或安装依赖。
