qemu-system-arm -M vexpress-a9 -m 1024M -snapshot -s  -kernel ../arch/arm/boot/zImage -initrd busybox-1.20.2/rootfs.img.gz -serial stdio -append "root=/dev/ram init=/sbin/init console=ttyAMA0 debug"
