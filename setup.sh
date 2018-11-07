#!/bin/sh
set -e

# Setup network
echo "\n\n\n" | netconf
service network restart

# Setup pkgin
MIRROR="http://10.0.2.2:8000"
pkg_add -f "$MIRROR/pkgin-0.6.4nb5.tgz"
echo "$MIRROR/" > /usr/pkg/etc/pkgin/repositories.conf
echo "y" | pkgin update

# Install some packages
PKGS="openssh vim whetstone-1.2 ubench-0.32nb1 sysbench-0.4.12nb4 randread-0.2 ramspeed-2.6.0 postmark-1.5 postgresql84-pgbench-8.4.21 pipebench-0.40 nsieve-1.2b netperf-2.4.5 netio-1.26 nbench-2.2.2 linpack-bench-940225 httperf-0.8nb1 hint.serial-98.06.12 heapsort-1.0 flops-2.0 fib-980203 dhrystone-2.1nb1 dbench-3.04nb1 bytebench-4.1.0nb5 blogbench-1.0nb1"
echo "y" | pkgin in $PKGS

# Finishing up, start services
# Start ssh server
/usr/pkg/etc/rc.d/sshd onestart
# Enable sshd at boot
sed -i 's|sshd=NO|sshd=YES|' /etc/defaults/rc.conf

echo "[+] Setup done. Goodbye !"
