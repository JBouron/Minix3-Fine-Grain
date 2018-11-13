#!/bin/zsh

KERNEL_IMAGE="obj.i386/minix/kernel/kernel"
DEFAULT_LAYOUT="src"
if [[ $@ > 1 ]]; then
	PORT=$1
else
	PORT="1234"
fi
REMOTE_ADDR="localhost:$PORT"
rm gdb.txt
CMDFILE=$(mktemp)
echo "target remote $REMOTE_ADDR" >> $CMDFILE
echo "m_dump_ktrace" >> $CMDFILE
gdb -n -batch -s $KERNEL_IMAGE -x /home/ketza/Documents/EPFL/PDM/PDM/scripts/gdb/minix.gdb -x $CMDFILE > /dev/null
