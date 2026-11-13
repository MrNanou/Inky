#!/bin/bash
set -e
cd ~/Code/Inky/esp/inky
source ~/Code/esp-idf/export.sh

# Wait for the board to wake up and expose its serial port.
echo "Waiting for /dev/ttyACM0 ..."
while [ ! -e /dev/ttyACM0 ]; do
    sleep 0.1
done

# Flash as soon as the port appears.
echo "Port found; flashing..."
sudo -E env "PATH=$PATH" idf.py -p /dev/ttyACM0 flash
