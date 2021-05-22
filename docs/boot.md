# 引导程序

本系统使用UEFI引导，不涉及16位汇编代码。引导程序为内核启动做准备工作，内容包括：
* 加载内核到内存中
* 探测内存布局
* 设置VBE模式
* 跳转到内核

## 准备工作

### [posix-uefi](https://gitlab.com/bztsrc/posix-uefi.git)

本系统使用posix-uefi来编写uefi引导程序，posix-uefi提供posix风格的编码方式来编写uefi程序，且不需要使用交叉编译。posix-uefi可以自动生成PE格式的可执行文件，无需手工干预。

为方便使用，将thirdpart/posix-uefi/uefi文件夹链接到boot/uefi/uefi:

```shell
cd boot/uefi/
ln -s ../../thirdpart/posix-uefi/uefi
```

### OVMF

bochs目前并不支持uefi启动，因此本系统使用qemu来进行模拟。qemu需要使用OVMF.fd来模拟uefi模式，OVMF.fd的二进制发布可以[在此](https://retrage.github.io/edk2-nightly/)下载。
