---
title: "KDE 笔记本触控板手势优化 libinput-gestures + ydotool"
date: 2025-02-24T13:47:09
categories:
- Tinkering
tags: 
 - KDE
 - Linux
 - Arch
showbreadcrumbs: false #顶部显示当前路径
---
目前(6.3.1) KDE Plasma 桌面自带的触控板手势只有 4 指上滑下滑，这肯定是不够用的。可以通过 [libinput-gestures](https://github.com/bulletmark/libinput-gestures) 来实现自定义笔记本触控板手势。


## libinput-gestures
### 安装
arch 用户直接通过 AUR 安装 [libinput-gestures from AUR](https://aur.archlinux.org/packages/libinput-gestures/)。
```sh
paru -S libinput-gestures
```
其他发行版用户手动安装：
```sh
git clone https://github.com/bulletmark/libinput-gestures.git
cd libinput-gestures
sudo ./libinput-gestures-setup install
```
> Debian and Ubuntu users may also need to install libinput-tools if that package exists in your release:
>```sh
>sudo apt-get install libinput-tools
>```

### 启动
1. 加入 `input` 用户组：
```sh
sudo gpasswd -a $USER input
newgrp input # 立即生效
```
2. 添加自启动，并现在启动：
```sh
libinput-gestures-setup autostart start
```

## ydotool
[libinput-gestures 文档](https://github.com/bulletmark/libinput-gestures)建议使用的键盘模拟器是 `xdotool` ，但是它只能用在 xorg 或者 xwayland 的 app 上，而最新的 KDE 采用 wayland，因此我们用 [ydotool](https://github.com/ReimuNotMoe/ydotool) 做替代。

### 安装并启动
```sh
# Arch:
sudo pacman -S ydotool
# Debian based systems:
sudo apt install ydotool
```
添加开机自启动并现在启动：
```sh
systemctl --user enable --now ydotool.service
```

## 配置
创建自定义配置：
```sh
cp /etc/libinput-gestures.conf ~/.config/ && kate ~/.config/libinput-gestures.conf
```
### libinput-gestures 配置解释
这里仅作快速上手解释，更详细请参考配置文件中的注释。
配置格式：
```
gesture <action> [finger_count] <command>
```
libinput-gestures 支持的 `action` 有：
-  swipe up
-  swipe down
-    swipe left
-    swipe right
-    swipe left_up
-    swipe left_down
-    swipe right_up
-    swipe right_down
-    pinch in
-    pinch out
-    pinch clockwise
-    pinch anticlockwise
-    hold on 
-    hold on N (按压 N 秒)

`finger_count` 即同时运动的手指数，例如，三指上滑写为：`gesture swipe up 3`
`command` 即执行的命令，可以是任何命令，包括运行脚本，因此可操作性很强。下面就是通过执行 `ydotool` 命令来实现的快捷键触发。

### ydotool
ydotool 支持键盘和鼠标模拟，详细参考：https://github.com/ReimuNotMoe/ydotool#usage

值得注意的是，ydotool 的 key 指令要用 keycodes，可以在这个文件中找到 keycodes：`/usr/include/linux/input-event-codes.h` 。`keycodes:1` 表示按住对应的键，`keycodes:0` 表示释放对应的键。

例如，要用 ydotool 模拟 ctrl + F12，需要依次按住 ctrl，按住 F12，再释放两个按键。ctrl(左) 的 keycodes 为 29，F12 的 keycodes 为 88，则命令写作：`ydotool key 29:1 88:1 88:0 29:0`

### 参考配置
于是，我们便可以自定义触控板的手势，来执行一些快捷键了。下面给出我自己用的简单配置：
```conf
gesture swipe down 3 ydotool key 29:1 88:1 88:0 29:0     # 三指下滑回到桌面
gesture swipe up 3 ydotool key 29:1 68:1 68:0 29:0       # 三指上滑查看窗口
gesture swipe right 4  ydotool key 56:1 15:1 15:0 56:0   # 任务窗口切换
gesture swipe left 4   ydotool key 56:1 15:1 15:0 56:0
gesture pinch out ydotool key 29:1 13:1 13:0 29:0        # 捏合放大（Ctrl + =）
gesture pinch in ydotool key 29:1 12:1 12:0 29:0         # 捏合缩小（Ctrl + -）
```
配置完成后，需要重启 libinput-gestures 以应用：
```sh
libinput-gestures-setup stop desktop autostart start
```
