# Poke3 已完成：原生桌面 + 阅读工具 + Root

完成时间：2026-07-27

## 当前状态

- 设备：Onyx BOOX Poke3，Android 10，固件 `3.5 / 12699`。
- 保留原厂墨水屏、前光、电源管理和系统设置。
- Magisk 30.7 Root 已永久写入并经完整重启验证。
- 已恢复文石原生桌面，底部“应用”是日常软件入口。
- 已安装 KOReader 2026.03，用于 EPUB、PDF、CBZ 漫画等本地阅读。
- 已安装 LocalSend 1.17.0，用于与 Mac 在同一 Wi-Fi 下无线传文件。
- 已安装“USB传书”1.1，用于检查真实 ADB USB 接口并打开系统设置或文石存储。
- Wi-Fi 默认关闭；自动熄屏为 360 秒（6 分钟）。
- ADB Shell 已获 Magisk Root，便于后续开发和维护。

## 日常使用

- 看书或漫画：进入底部“应用”，打开 KOReader。
- 管理文件：进入底部“存储”。
- 系统设置：进入底部“设置”。
- PokeHome 已从设备删除；安装包备份保留在 `outputs/PokeHome-v2.apk`。

## USB + ADB 传文件（优先）

Mac 桌面已有 `Poke3传书.app`。Poke3 的文石原生“应用”第二页已有“USB传书”。

使用方法：

1. Poke3 开机，用 USB 数据线连接 Mac。
2. Poke3 的“USB传书”显示“ADB USB 已连接就绪”。
3. Mac 双击桌面的“Poke3传书”。
4. 在窗口中选择目标目录：漫画、图书、收件箱或下载。
5. 把 EPUB、PDF、CBZ 或整个文件夹拖进窗口；文件会立即加入队列。
6. 传输期间可继续拖入，完成后到所选目标目录查看。

拖书窗口会保持打开并接收新的传输任务：

- 不需要按回车，不会弹出本机文件选择窗口。
- 一次拖入 111 本时，每本都会成为独立队列任务。
- 传输期间新拖入的文件会继续排队。
- 每批任务会记住拖入时选择的目标目录。
- Apple Books 保存成文件夹的 `.epub` 会自动封装为标准 EPUB，再传到 Poke3。
- 窗口打开且 USB 已连接时，Poke3 会保持唤醒。

ADB 传输无需开启 Wi-Fi。脚本固定连接本机这台 Poke3，其他 Android 设备不会被误选。实测推送文件后，Mac 与 Poke3 的 SHA-256 完全一致。

如果 Poke3 已插线，但脚本提示没有连接：

1. 查看 Poke3 的“USB传书”状态。
2. 若显示“调试开关已开，ADB 接口未出现”，到系统设置把 USB 调试关闭再打开。
3. 重新运行 Mac 桌面的传书脚本。脚本会自动等待 ADB 接口 8 秒，并区分“没有插线”和“只有 MTP 接口”。

## LocalSend 传文件

Mac 和 Poke3 都已安装 LocalSend 1.17.0。Poke3 端名称设为 `Poke3`，已关闭动画、自动接收和相册保存，接收目录使用系统“下载”目录。

传文件时：

1. 在 Poke3 设置里打开 Wi-Fi。
2. 确保 Mac 和 Poke3 连接同一局域网。
3. 两边分别打开 LocalSend。
4. Mac 选择“发送”，目标选择 `Poke3`；Poke3 手动确认接收。
5. 传完退出 LocalSend，并关闭 Wi-Fi。

接收文件位于“存储 → Download”。需要分类时，可移动到 `Books/Books` 或 `Books/Comics`。

## 已冻结的文石组件

以下组件使用 Android 的“停用”方式冻结，未删除，随时可恢复：

- 云同步：`com.onyx.android.ksync`
- OTA 更新：`com.onyx.android.onyxotaservice`
- 商店：`com.onyx.igetshop`、`com.onyx.appmarket`
- 邮件、传输、悬浮球、计算器、工厂测试：`com.onyx.mail`、`com.onyx.easytransfer`、`com.onyx.floatingbutton`、`com.onyx.calculator`、`com.onyx.android.production.test`

它们的用户数据与缓存已清除，回收约 12 MB。系统分区中的 APK 保留在只读空间，不能转为可用存储；被冻结后不会启动、联网或消耗后台电量。

恢复某一项：

```bash
adb shell su -c 'pm enable --user 0 包名'
```

需要联网时在系统里打开 Wi-Fi；维护后可关闭。也可用：

```bash
adb shell su -c 'svc wifi enable'
adb shell su -c 'svc wifi disable'
```

## 已验证的恢复资料

原始 boot、recovery、persist、onyxconfig、fsg、modemst1、modemst2 已从这台设备读出，在设备端与 Mac 上逐份 SHA-256 校验一致。它们保留在私有工作区 `work/poke3-root/backup/`，不要上传或分享。

设备读出的原始 boot 有效载荷与官方同版本原厂镜像完全一致：

```text
Poke3_3.5_12699_stock_boot.img
SHA-256: 7915c6eba9166aa93de76ef819eb937e78b8cfbf8e4659f87e9d415785cb5db1
```

如需取消 Root 并恢复原厂启动镜像：

```bash
adb reboot bootloader
fastboot flash boot Poke3_3.5_12699_stock_boot.img
fastboot reboot
```

只刷 `boot` 即可恢复原厂启动；不要随意刷写 `persist`、`onyxconfig`、`fsg` 或 modem 分区。

## 电池读数

本次充满后系统 fuel gauge 的 `Charge counter` 为约 **1185 mAh**，约为标称 1500 mAh 的 **79%**。这个读数会随校准变化；完成一次“充满 → 飞行模式待机 24 小时 → 记录电量”的实测后，才能判断真实续航与电芯健康。
