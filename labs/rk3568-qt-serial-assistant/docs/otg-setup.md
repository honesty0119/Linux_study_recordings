# OTG实验配置说明

## 本次链路

经用户明确授权，板端临时启用ADB与ACM，ffs.adb和acm.gs6同时绑定，port_num=0对应ttyGS0。Windows识别COM4，COM3为独立CH343控制台；其他电脑须重新识别，不照搬编号。

## 临时配置与恢复

仅调整/tmp/.usb_config，持久/etc/init.d/.usb_config不变。厂商S50usbdevice与udev协作，单独stop会被自动恢复，原生restart通过USB_FORCE_CHANGED协调。先读实际BSP脚本，备份原配置，在授权范围内变更；不禁用监控、不复制本机认证或连接配置。

回退需结束虚拟串口会话，核验本机备份，恢复原临时配置后调用适配的原生restart并检查绑定/UDC。会短暂中断USB，本轮未演练回退；重启后临时文件可能消失，不提供可直接照搬的备份路径或自动配置脚本。

## 测试边界

--allow-usb-gadget只允许选择ttyGS0，不配置USB、不自动打开端口。ADB函数被保留，不等于另做过ADB传输验收。OTG结果不代替实体UART或RS-232电气验收。
