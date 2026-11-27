#ifndef KERNEL_SYSCALL_H
#define KERNEL_SYSCALL_H

#define OS_SYSNAME  "USR/UX 2"
#define OS_NODENAME "localhost"
#define OS_RELEASE  "1 0.65"
#define OS_VERSION    "#1 " __DATE__ " " __TIME__
#define OS_MACHINE  "i386"
#define OS_DOMAINNAME "(none)"
#define OS_OPERATING_SYSTEM "FloppyUX"

void syscall_init(void);

#endif