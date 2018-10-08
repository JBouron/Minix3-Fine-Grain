#!/bin/zsh

KERNEL_IMAGE="obj.i386/minix/kernel/kernel"
DEFAULT_LAYOUT="src"
REMOTE_ADDR="localhost:1234"
gdb -s $KERNEL_IMAGE -ex "target remote $REMOTE_ADDR" -ex "layout $DEFAULT_LAYOUT"
