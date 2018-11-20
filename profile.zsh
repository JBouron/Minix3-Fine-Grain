#!/bin/zsh

KERNEL_IMAGE="obj.i386/minix/kernel/kernel"
DEFAULT_LAYOUT="src"
if [[ $@ > 1 ]]; then
	SLEEP=$1
else
	SLEEP="20"
fi
PORT="1234"
REMOTE_ADDR="localhost:$PORT"
rm gdb.txt
CMDFILE=$(mktemp)
echo "target remote $REMOTE_ADDR" >> $CMDFILE
echo "shell rm gdb.txt" >> $CMDFILE
echo "set var ktzprofile_enabled=1" >> $CMDFILE
echo "continue" >> $CMDFILE
gdb -n -batch -s $KERNEL_IMAGE -x /home/ketza/Documents/EPFL/PDM/PDM/scripts/gdb/minix.gdb -x $CMDFILE > /dev/null &
PID=$!
sleep $SLEEP
kill -SIGTERM $PID

CMDFILE=$(mktemp)
echo "target remote $REMOTE_ADDR" >> $CMDFILE
echo "m_dump_ktzprofile_data" >> $CMDFILE
gdb -n -batch -s $KERNEL_IMAGE -x /home/ketza/Documents/EPFL/PDM/PDM/scripts/gdb/minix.gdb -x $CMDFILE > /dev/null &
