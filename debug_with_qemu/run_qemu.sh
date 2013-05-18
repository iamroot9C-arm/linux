qemu-system-arm -M vexpress-a9 -m 1024M -snapshot -s  -kernel -smp 4 ../arch/arm/boot/zImage -initrd busybox-1.20.2/rootfs.img -serial stdio -append "root=/dev/ram rdinit=/sbin/init console=ttyAMA0 debug"

#qemu-system-arm -M vexpress-a9 -m 1024M -snapshot -s  -kernel ../arch/arm/boot/zImage -initrd busybox-1.20.2/rootfs.img -serial stdio -append "root=/dev/ram rdinit=/sbin/init console=ttyAMA0 debug"

#qemu-system-arm -M vexpress-a9 -m 1024M -snapshot -s -S -kernel ../arch/arm/boot/zImage -initrd busybox-1.20.2/rootfs.img.gz -serial stdio -append "root=/dev/ram rdinit=/sbin/init console=ttyAMA0 debug"
