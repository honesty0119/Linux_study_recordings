# 任务03 RK3568 Qt串口调试助手

基于厂商Qt5.15.2、C++11和QtSerialPort，在RK3568 LCD上实现触摸串口助手。**2026-11-23主要功能实测通过，保留验收项尚未全部完成。**

## 交付入口

- [调试与验收图文报告（9张实测原图）](docs/acceptance-report.md)
- [验收状态与保留项](requirements-status.md)
- [环境、Kit、构建部署说明](delivery-guide.md)
- [日志与文件等选做功能](optional-features.md)
- [交付ZIP及SHA256下载](https://github.com/honesty0119/Linux_study_recordings/releases/tag/task03-qt-serial-20261123)

## 已测结果

Ubuntu22.04.5、Qt Creator6.0.2、GCC10.3.0交叉构建成功；目标AArch64、Buildroot2018.02-rc3、Linux4.19.232。复用Wayland，仅应用内横屏。24项板端自检通过。

真实OTG完成双向文本、双向4096 B全字节二进制、定时发送与停止、文件发送、接收日志、96 KiB完整记录与64 KiB尾部缓存、拔线异常及手动重连。结果据原始文件逐字节和SHA256核验，照片不代替数据证据。

## 工程与复现

qt_demo/为qmake源码和资源，scripts/为构建/推送工具，tests/fixtures/为已知样本，docs/为图文报告，evidence/为公开整理后的证据。接收bin原字节保留；文本中的本机路径已泛化，图片保留原内容。SDK、认证文件、个人.pro.user、系统字体与镜像不包含在仓库。

Qt Creator打开qt_demo/qt_demo.pro，配置匹配SDK的qmake/C/C++/sysroot。SDK示例/opt/rk3568-sdk，可用RK3568_SDK覆盖。Ubuntu命令见commands.txt。Windows deploy.py必须指定--ssh-config，该配置由使用者定义rk3568-board并预先确认主机指纹。

源码已由Creator构建通过；build.sh仅静态检查，不能写成已实跑。Windows推送原实现实际通过；公开版改为显式SSH配置参数，未在公开路径重新联网部署。

## 验收边界

仍待真实验证：不存在端口/权限不足、实体UART参数及RS-232电气、文件取消/大容量异常、持续负载、Ubuntu一键构建。OTG不等同物理UART，TX不等于对端确认。控制台始终保护，未宣称全部任务书验收完成。
