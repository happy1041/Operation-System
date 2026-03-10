只连接一次 （debugpintos）
看完 getting started
把 git 管理搞好
再看一下 loading
有些东西就是你不自己写一下该不会，只会永远失去边写代码边调试的能力。

CS : 代码段寄存器；IP : 指令指针寄存器。在8086机中，任意时刻，CPU将CS:IP指向的内容当作指令来执行。

BIOS Bootloader Kernel

loader create basic page table 之前，是直接拿真实物理地址（例如基地址 0x20000）来访问的。建立之后（0xc0020000->0x00020000）就可以通过虚拟地址来访问了。但是当前 cpu 仍然拿着 0x20000，把它当作虚拟地址来翻译，会 crash。所以再创建一个恒等映射。（maps the 64 MB at the base of virtual memory (starting at virtual address 0) directly to the identical physical addresses.）

+------------------+  <- 0xFFFFFFFF (4GB)
|      32-bit      |
|  memory mapped   |
|     devices      |
|                  |
/\/\/\/\/\/\/\/\/\/\
/\/\/\/\/\/\/\/\/\/\
|                  |
|      Unused      |
|                  |
+------------------+  <- depends on amount of RAM
|                  |
|                  |
| Extended Memory  |
|                  |
|                  |
+------------------+  <- 0x00100000 (1MB)
|     BIOS ROM     |
+------------------+  <- 0x000F0000 (960KB)
|  16-bit devices, |
|  expansion ROMs  |
+------------------+  <- 0x000C0000 (768KB)
|   VGA Display    |
+------------------+  <- 0x000A0000 (640KB)
|  pintos kernel   |
+------------------+  <- 0x00020000 (128KB)
|  page tables     |
|  for startup     |
+------------------+  <- 0x00010000 (64KB)
|  page directory  |
|  for startup     |
+------------------+  <- 0x0000f000 (60KB)
|  initial kernel  |
|   thread struct  |
+------------------+  <- 0x0000e000 (56KB)
|        /         |
+------------------+  <- 0x00007e00 (31.5KB)
|   pintos loader  |
+------------------+  <- 0x00007c00 (31KB)
|        /         |
+------------------+  <- 0x00000600 (1536B)
|     BIOS data    |
+------------------+  <- 0x00000400 (1024B)
|     CPU-owned    |
+------------------+  <- 0x00000000