#!/bin/sh

git clone https://github.com/x42/libltc
git clone https://github.com/WiringPi/WiringPi.git

(cd libltc ; ./autogen.sh; ./configure; make; sudo make install)
(cd WiringPi; ./build)
sudo ldconfig
