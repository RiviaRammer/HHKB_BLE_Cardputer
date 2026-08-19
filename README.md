# HHKB Professional 2 × Cardputer BLE 桥接器

把 USB 版 HHKB Professional 2 接到 ESP32-S3开发板：ESP32-S3 作为 USB Host 读取键盘，再作为标准 BLE HID 键盘连接电脑、平板或手机。

固件按原始 HID usage 转发，不先把按键翻译成字符，因此 Ctrl/Alt/Shift/GUI 组合键、HHKB 的 Fn 层和 DIP 开关映射都能保留。蓝牙设备名为 `HHKB_BLE`，当前实现标准 6KRO 键盘，不转发 Consumer Control 页。

## 已验证状态

- ESP32-S3 能枚举 HHKB Professional 2 的 `0853:0100` Boot Keyboard 接口。
- 8 字节 USB 键盘报告可通过 Web 状态页实时观察。
- Cardputer 自身的 USB 供电不足以可靠启动 HHKB 的内置 Hub 和键盘；使用隔离的稳定外部 5V 后枚举成功。
- 固件底层保留多级 USB Hub 支持，但应用层只桥接键盘接口。

## Web 状态页

复制示例配置并填写自己的 Wi-Fi 名称和密码：

```bash
cp main/config_example.h main/config.h
```

```text
主机名:   hhkb-debug
状态页:   http://<Cardputer-IP>/
状态 API: http://<Cardputer-IP>/api/status
```

让手机或电脑连接同一个 Wi-Fi，然后在路由器的 DHCP/客户端列表中查找主机名 `hhkb-debug`。Cardputer 启动日志也会输出状态页地址；烧录后可先从串口监视器记下 IP，再把 USB-C 接口切换给 HHKB。

页面可以实时观察：

- HHKB 是否已经连接。
- 当前按键和累计输入报告。
- 最近 40 条输入及原始 8 字节报告。
- `HHKB_BLE` 是否已连接并完成配对。


## 先确认供电：数据能直连，5V 必须隔离

Cardputer 的 ESP32-S3 原生支持 USB OTG，但其 USB-C 口主要按受电/充电口设计。切换为 USB Host 不等于接口会自动提供可靠的 5V VBUS。HHKB Professional 2 内部还有一个 USB Hub，供电不能省略。

最方便的制作方法是使用合规的独立供电 USB 2.0 Hub。

## 编译和刷写

工程是原生 ESP-IDF，已使用 ESP-IDF 5.5.5 验证构建。首次构建需要联网，ESP-IDF Component Manager 会根据 `main/idf_component.yml` 自动下载官方 `espressif/usb_host_hid` 1.2.0 组件。

在 VSCode 中：

1. 用 ESP-IDF 扩展打开本目录。
2. 选择 `ESP-IDF: Select Current ESP-IDF Version`，使用 5.5.5。
3. 将 `main/config_example.h` 复制为 `main/config.h` 并填写 Wi-Fi。
4. 选择目标 `esp32s3`，串口选择 Cardputer。
5. 执行 `ESP-IDF: Build your project`，再执行 `Flash your project`。

已经激活 ESP-IDF 5.5.5 的终端也可以运行：

```bash
idf.py set-target esp32s3
cp main/config_example.h main/config.h
# 编辑 main/config.h 后继续
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

把 `/dev/ttyACM0` 换成实际串口。Cardputer 只有一个 USB-C 数据口，因此刷写与连接 HHKB 分两步：

1. 先不接 HHKB，用电脑给 Cardputer 刷写。
2. 刷写完成后拔掉电脑线，保持侧面电源开关为 ON。
3. 接上制作好的隔离供电直连线（或测试 Hub），必要时按复位键。
4. 从启动日志或路由器中取得 `hhkb-debug` 的 IP，在同一 Wi-Fi 中打开该地址，先确认按键报告正常进入开发板。
5. 在目标设备蓝牙设置中配对 `HHKB_BLE`。

## 配对与排障

- Windows/macOS/手机若缓存过旧的 HID 描述符，先在系统里“忽略/删除 `HHKB Cardputer` 和 `HHKB_BLE`”，重启 Cardputer 后重新配对 `HHKB_BLE`。
- 固件使用 BLE Secure Connections Just Works 绑定；配对记录保存在 ESP32 NVS，重启后会自动回连。
- 蓝牙断线期间的按键不会补打；重连时只同步当下仍按住的键。
- Caps Lock、Num Lock、Scroll Lock 状态会由 BLE 主机反向发回 USB 键盘。HHKB 通常没有对应指示灯，但链路是完整的。
- 看不到蓝牙名称：确认系统日志出现 `BLE HID started`，并删除目标设备中的旧配对。
- 没有 `USB keyboard online`：优先检查 VBUS 是否真有 5V、D+/D- 是否接反，以及是否启用了数据线。
- HHKB 自带的两个 Hub 下行口不在本项目支持范围内；不要再接鼠标、U 盘等设备。

## 已知边界

- 仅桥接一个 USB 键盘到一个 BLE 主机。
- BLE 键盘报告为标准 6KRO；按下超过 6 个非修饰键时，多出的键无法同时上报。
- 已通过 ESP-IDF 5.5.5 完整编译，并在 Cardputer、HHKB Professional 2 和隔离外部供电条件下验证 USB 枚举及按键输入。

## 参考资料

- [M5Stack Cardputer 官方文档](https://docs.m5stack.com/en/core/Cardputer?hidden=true)
- [Cardputer 官方原理图](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/481/Sch_M5Cardputer.pdf)
- [HHKB Professional 2 官方规格（含内置 Hub 供电规格）](https://happyhackingkb.com/jp/products/discontinued/hhkb_pro2/)
