#!/bin/zsh

KERNEL_IMAGE="obj.i386/minix/kernel/kernel"
DEFAULT_LAYOUT="src"
if [[ $@ > 1 ]]; then
	PORT=$1
else
	PORT="1234"
fi
REMOTE_ADDR="localhost:$PORT"
gdb -s $KERNEL_IMAGE -ex "target remote $REMOTE_ADDR" -ex "layout $DEFAULT_LAYOUT" -ex "info thread"
