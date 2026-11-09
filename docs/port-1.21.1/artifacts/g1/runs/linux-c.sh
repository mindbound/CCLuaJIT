#!/bin/bash
# Phase C (and D), Linux: MachineSpecs on this distro. $1 = alma | ubuntu, $2 = label suffix (release | debug).
G=<scratch>/g1
case $1 in
  alma) J=/usr/lib/jvm/java-21-openjdk; echo "host: $(cat /etc/redhat-release)" ;;
  ubuntu) J=/usr/lib/jvm/java-21-openjdk-amd64; echo "host: $(. /etc/os-release; echo $PRETTY_NAME)" ;;
esac
bash $G/specs-g1.sh $J $1-${2:-release}
