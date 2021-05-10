#!/bin/bash

# simple programme that uses the bargraph on the osrfx2_0 board to count in binary format

for i in {1..255}
do
	echo $i
	echo $i > /sys/class/usbmisc/osrfx2_0/device/bargraph
	sleep .2
done
