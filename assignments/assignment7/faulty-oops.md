# Faulty Kernel Oops Analysis

## Command that triggered the oops
### echo "hello_world" > /dev/faulty
Writing to `/dev/faulty` invokes the `faulty_write()` function in the
`faulty` kernel module, which deliberately dereferences a NULL pointer
to demonstrate a kernel oops.

#### The oops message
Unable to handle kernel NULL pointer dereference at virtual address 0000000000000000
Mem abort info:
ESR = 0x0000000096000045
EC = 0x25: DABT (current EL), IL = 32 bits
SET = 0, FnV = 0
EA = 0, S1PTW = 0
FSC = 0x05: level 1 translation fault
Data abort info:
ISV = 0, ISS = 0x00000045
CM = 0, WnR = 1
Internal error: Oops: 0000000096000045 [#1] SMP
Modules linked in: hello(O) faulty(O) scull(O) [last unloaded: scull(O)]
CPU: 0 PID: 153 Comm: sh Tainted: G O 6.1.44 #2
Hardware name: linux,dummy-virt (DT)
pstate: 80000005 (Nzcv daif -PAN -UAO -TCO -DIT -SSBS BTYPE=--)
pc : faulty_write+0x10/0x20 [faulty]
lr : vfs_write+0xc8/0x390
Call trace:
faulty_write+0x10/0x20 [faulty]
ksys_write+0x74/0x110
__arm64_sys_write+0x1c/0x30
invoke_syscall+0x54/0x120
el0_svc_common.constprop.0+0x44/0xf0
do_el0_svc+0x2c/0xc0
el0_svc+0x2c/0x90
el0t_64_sync_handler+0x114/0x120
el0t_64_sync+0x18c/0x190
Code: d2800001 d2800000 d503233f d50323bf (b900003f)
---[ end trace 0000000000000000 ]---

## Analysis
### What kind of fault

The first line states the fault directly: a **NULL pointer dereference**
at virtual address `0x0000000000000000`. The `Data abort info` reports
`WnR = 1`, meaning the fault occurred on a **write** (Write, not Read).
This matches `faulty_write()` attempting to store a value to a NULL
address. `FSC = 0x05` identifies it as a level 1 translation fault —
the MMU had no valid mapping for address 0.

### Where the fault occurred

The most useful line for locating the fault is: pc : faulty_write+0x10/0x20 [faulty]

The program counter (PC) was at offset `0x10` (16 bytes) into the
`faulty_write` function, which is `0x20` (32 bytes) long in total, and
the function belongs to the `faulty` module. So the faulting
instruction sits about halfway through the function.

The `Modules linked in:` line confirms `faulty(O)` is loaded — the `O`
marks it as an out-of-tree module — and `Tainted: G ... O` shows the
kernel is tainted by an out-of-tree module, which is expected here.

### The call trace

The trace shows how execution reached the fault, from the bottom up:
el0t_64_sync -> exception entry from userspace (EL0)
el0_svc -> supervisor call (syscall) handling
do_el0_svc / invoke_syscall
__arm64_sys_write -> the write() syscall
ksys_write
faulty_write [faulty] -> the driver's write handler (fault here)


This confirms the chain: the userspace `echo` performed a `write()`
system call on the open file descriptor for `/dev/faulty`, the VFS
layer dispatched it to the driver's registered write handler
`faulty_write()`, and that function dereferenced NULL.

### Locating the faulting line in the driver

`faulty_write()` in `misc-modules/faulty.c` contains an intentional
NULL dereference:

```c
ssize_t faulty_write(struct file *filp, const char __user *buf,
                     size_t count, loff_t *pos)
{
    /* make a simple fault by dereferencing a NULL pointer */
    *(int *)0 = 0;
    return 0;
}
```

The `*(int *)0 = 0;` statement writes the value 0 to memory address 0.
Because address 0 is unmapped, the MMU raises the translation fault the
oops reports.

The `Code:` line disassembles the instructions around the PC; the
faulting instruction is shown in parentheses `(b900003f)`, an AArch64
`str` (store) instruction — consistent with a write to the NULL
pointer.

### How to use this to find the bug

The combination of:
1. `pc : <function>+<offset>/<size>` — which function and how far in,
2. the `Call trace` — how the code was reached, and
3. the `WnR` bit and faulting address — read vs. write, and where,

lets a developer pinpoint the offending source line. Compiling the
module with debug info and running `addr2line`, `objdump -dS`, or `gdb`
on the module's `.ko` with the `faulty_write+0x10` value resolves the
exact line responsible for the fault.
EOF
