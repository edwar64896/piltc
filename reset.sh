#!/bin/sh
echo "Resetting NTP Time"
echo ""
echo ""
service ntp stop
ntpd -u ntp -g -q
service ntp start
echo ""
echo Starting NTP LTC Generator
echo ""
./timer

