switch_thread 是内联汇编函数，其实就是为了save/recover寄存器的值。

神秘 thread create，在不断返回函数调用的过程中去调用 switch_thread，switch_entry(thread_schedule_tail())，kernel_thread() (enables interrupts and calls the thread's function，并且调用想要的函数)

sema_try_down 不会 wait，能减就减，成功返回 true，失败返回 false。

只有持有锁的 thread 可以释放它。

The Interrupt Handling Process：
中断发生，CPU 硬件查 IDT，发现对应的 stub 地址（例如时钟中断，intr20_stub），stub 会存 intr number，然后调用 intr_entry() 保存 all register 的值，然后 call intr_handler() 调用具体的注册函数。