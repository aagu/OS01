# 项目结构

本系统参考osdev Wiki中[Meaty Skeleton](https://wiki.osdev.org/Meaty_Skeleton)的项目结构来组织文件。

* `boot` 系统启动相关代码
* `kernel` 系统内核相关代码
* `libc` 系统库函数

`libc`内代码依据不同的编译指令，会编译为`libk`和`libc`两个库（后者暂未实现），`libk`和`libc`共享部分代码，前者由内核使用，后者供应用程序使用。