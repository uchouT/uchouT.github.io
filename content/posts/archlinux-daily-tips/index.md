---
title: "Arch Linux + KDE 日常使用的记录"
date: 2025-02-11T23:57:49+08:00
showLastMod: true
showbreadcrumbs: false #顶部显示当前路径
lastmod: 2025-03-31T13:48:10+08:00
tags: 
 - Arch
 - KDE
 - Linux
categories:
- Tinkering
summary: "本文记录自己在笔记本上使用 ArchLinux 和 KDE plasma 桌面进行日常学习娱乐时遇到的一些问题，以及解决方法，伴随着使用会不断更新。"
---

本文记录自己在笔记本上使用 ArchLinux 和 KDE plasma 桌面进行日常学习娱乐时遇到的一些问题，以及解决方法，伴随着使用会不断更新。

环境如下：

- Host: HP Envy x360 2-in-1 Laptop 14-fa0xxx
- OS: Arch Linux x86_64
- Kernel: Linux 6.13.1-zen3-1-zen
- DE: KDE Plasma 6.2.5
- WM: Kwin (Wayland)
- CPU: AMD Ryzen 7 8840HS w/ Radeon 780M Graphics (16) @ z
- GPU: AMD Phoenix3 [Integrated]
- Memory: 32G


## KDE 相关

目前的版本是 Plasma 6.2.5

### 键盘输入卡顿

问题由 PSR 导致，需要加上一段内核参数。

>PSR（面板自刷新）是一种节能技术，主要用于节省功耗。当屏幕内容保持不变时，显卡会停止发送数据，而是让显示面板自行维护画面。但在某些情况下，PSR 可能会导致显示问题，如：
>
> - 屏幕闪烁或短暂黑屏
> - 画面冻结
> - 延迟或其他图像异常

1. 确保使用的是 amd 开源驱动 [AMDGPU](https://wiki.archlinuxcn.org/wiki/AMDGPU)。

2. 修改 GRUB 启动参数:
```
sudoedit /etc/default/grub 
```
找到 `GRUB_CMDLINE_LINUX_DEFAULT` ，在末尾加上 `amdgpu.dcdebugmask=0x10` 参数。

3. 重新生成 GRUB 配置
```sh
sudo grub-mkconfig -o /boot/grub/grub.cfg
```
下次重启时，应该就正常了。

问题跟进：https://bbs.archlinux.org/viewtopic.php?id=301280

### 启用休眠
参考： [ArchWiki](https://wiki.archlinuxcn.org/zh-cn/%E7%94%B5%E6%BA%90%E7%AE%A1%E7%90%86/%E6%8C%82%E8%B5%B7%E4%B8%8E%E4%BC%91%E7%9C%A0)
1. 先确保已创建了交换分区，如果没有按以下命令创建：
```sh
sudo fallocate -l 32G /swapfile   # 大小 >= 内存
sudo chmod 600 /swapfile
sudo mkswap /swapfile
sudo swapon /swapfile
sudo echo '/swapfile none swap defaults 0 0' >> /etc/fstab
```
2. 配置 initramfs
```sh
sudoedit /etc/mkinitcpio.conf
```
找到 `HOOKS` 在里面添加 `resume` ，注意要添加在 `udev` 之后。
3. 重新生成配置 `sudo mkinitcpio -P
`
### 美化

安装 `plasma-framework5`，很多美化的东西要依赖这玩意。

- splashscreen: https://www.pling.com/p/2136517
- SDDM login Theme: https://www.pling.com/p/1312658
- Application Style: kvantum + [layan](https://www.pling.com/p/1325246)
- Global Theme: [Layan](https://www.pling.com/p/1325243), 注意不要直接应用全局主题，我这里直接应用会导致黑屏，重启后才恢复，而且 dock 栏会直接消失。建议是下载了全局主题后，逐项目手动应用，dock 栏布局可能得自己手搓。

Desktop Effects 里面勾选 "Blur".

窗口圆角：安装 [lightlyshaders](https://github.com/a-parhom/LightlyShaders), archlinux 用户直接通过 [aur](https://aur.archlinux.org/packages/lightlyshaders-git) 安装。

### NTFS 移动硬盘挂载失败
可以通过 `mount` 命令手动挂载，但是在桌面环境中无法通过鼠标点击弹出的菜单挂载，会显示错误信息：`wrong fs type, bad option, bad superblock on /dev/sda1, 
missing codepage or helper program, or other error`

**解决方法 :** 禁用 `ntfs3` 模块，切换为 `ntfs-3g`
1. 将 `ntfs3` 列入黑名单
```sh
echo "blacklist ntfs3" | sudo tee /etc/modprobe.d/blacklist-ntfs3.conf
```
更新 `initramfs`
```sh
sudo mkinitcpio -P
```
2. 确保安装 `ntfs-3g`
```sh
sudo pacman -S ntfs-3g
```
3. 编辑 `/etc/udisk2/mount_options.conf`，加入以下内容：
```ini
[defaults]
ntfs_defaults=uid=$UID,gid=$GID,rw,relatime
ntfs_allow=uid=$UID,gid=$GID,prealloc

```
重启后应该就正常了。

### 触控板手势优化

安装 `libinput-gestures` 和 `ydotool` 。详细可以参考 [KDE 笔记本触控板手势优化 libinput-gestures + ydotool](/posts/kde-touchpad-gesture/)

## 软件相关
### flatpak 切换为用户级安装
flatpak 中默认的 remote 源是系统级安装的，占用根分区。切换为用户级安装（只为当前用户安装）方法如下：
```sh
sudo flatpak remote-delete flathub # 删除原有的 remote
flatpak --user remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
```
### mpv 链接协议注册
用于打开 `mpv://` 的 mpv 跳转链接。

直接用别人造好的协议注册器 [Github](https://github.com/akiirui/mpv-handler)
`paru -S mpv-hand`

### rime 输入法
```sh
sudo pacman -S fcitx5-im fcitx5-rime
```
设置里 `Keyboard` -> `Virtual Keyboard` 启用，再去 `input method` 里添加 Rime，`Configure addons...` 里把 UI 切换为 KDE plasma。

使用[白霜拼音](https://github.com/gaboolic/rime-frost)词库：
```sh
rm -rf ~/.local/share/fcitx5/rime
git clone https://github.com/gaboolic/rime-frost.git ~/.local/share/fcitx5/rime
```
重新部署即可。

### Dolphin 配置右键打开 Tabby
1. 确保 Tabby 是从 GitHub 仓库下载 .pacman 文件进行安装的。
> 从 aur 获取的 Tabby 有谜之问题，没去研究过。

2. 创建 Dolphin Service Menus
```sh
sudoedit /usr/share/kio/servicemenus/TabbyHere.desktop
```
目录位置也可能在 `~/.local/share/kio/servicemenus`，视情况而定。然后粘贴以下内容：
```ini
[Desktop Entry]
Type=Service
ServiceTypes=KonqPopupMenu/Plugin
MimeType=inode/directory
Actions=open_tabby
X-KDE-Priority=TopLevel
Icon=utilities-terminal

[Desktop Action open_tabby]
Name=Open Tabby Here
Exec=tabby open %f
Icon=utilities-terminal
```
保存后重启 Dolphin，右键就能看到 `Open Tabby Here` 的选项了，如果没看到，就去 "Dolphin Configuration" -> "Context Menu" 里启用一下。
