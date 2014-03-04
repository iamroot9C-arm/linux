#!/bin/bash
#qemu-system-arm -M vexpress-a9 -m 1024M -snapshot -s -S -smp 4,cores=4 -kernel arch/arm/boot/zImage -initrd ./rootfs.img -serial stdio -append "root=/dev/ram rdinit=/sbin/init console=ttyAMA0 debug"
qemu-system-arm -M vexpress-a9 -m 1024M -snapshot -s -S -smp 4,maxcpus=64,sockets=16,cores=4,threads=1 -kernel arch/arm/boot/zImage -initrd ./rootfs.img -serial stdio -append "root=/dev/ram rdinit=/sbin/init console=ttyAMA0 debug"

