#!/bin/zsh

KERNEL_IMAGE="obj.i386/minix/kernel/kernel"
DEFAULT_LAYOUT="src"
PORT="1234"
if [[ $@ > 1 ]]; then
        HOST=$(docker inspect $1 | grep "IPAddress" | tail -n1 | awk -F\" '{print $4}')
else
        HOST="localhost"
fi
REMOTE_ADDR="$HOST:$PORT"
gdb -s $KERNEL_IMAGE -ex "target remote $REMOTE_ADDR" -ex "layout $DEFAULT_LAYOUT" -ex "info thread"

