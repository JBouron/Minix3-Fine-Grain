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
echo "y" | pkgin in openssh
echo "y" | pkgin in vim

# Finishing up, start services
# Start ssh server
/usr/pkg/etc/rc.d/sshd onestart

echo "[+] Setup done. Goodbye !"
