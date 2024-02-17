# 编译过程

本系统需要使用交叉编译工具，[为什么需要交叉编译](https://wiki.osdev.org/Why_do_I_need_a_Cross_Compiler)？简单来讲，就是去除系统自带编译工具的各种“优化”选项，因为我们在编译内核，针对应用程序编译所做的优化，可能适得其反。

## 如何获取交叉编译工具

具体构建流程参考[这里](https://wiki.osdev.org/GCC_Cross-Compiler)，需要注意的是，本系统为64位，因此需要编译x86_64-elf工具链，而非i686-elf。此外，强烈建议参照[这个](https://wiki.osdev.org/Libgcc_without_red_zone)说明编译没有red zone的libgcc，否则在中断处理时可能遇到大麻烦。

## Build dependencies

* mkfs.vfat (from dosfstools)
* mmd (from mtools)