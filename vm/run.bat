@echo off
qemu-system-x86_64.exe -kernel "%~dp0kernel.img" -initrd "%~dp0initramfs.img" -append "console=tty0 quiet" -m 256M -k es -drive file="%~dp0disk.img",format=raw,if=virtio
pause
