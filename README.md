# piltc

pi ltc timecode generator thingy for RPI3B+. It might work on other versions, but this is what I have.

Dependencies on libltc

Also, you need to add "isolcpus=3" to your /boot/cmdline.txt file on your rpi so we isolate one CPU away from the rest of the machine. We will run our timecode thread on this CPU core.



