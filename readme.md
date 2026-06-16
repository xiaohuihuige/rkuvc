### 操作流程
1. `mv /etc/init.d/S50usbdevice* /etc/`; `reboot`
2. 配置uvc功能：运行uvc_config.sh或者usb_config.sh
3. 运行: uvc_app
4. 打开AMCAP即可预览




## 修改bsp支持extcon_usb

内核配置打开 
CONFIG_EXTCON_USB_GPIO=y

