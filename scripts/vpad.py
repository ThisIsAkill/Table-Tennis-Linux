#!/usr/bin/env python3
# Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
"""Virtual Xbox 360 gamepad for unattended controller tests.

    scripts/vpad.py FIFO

Creates the pad through /dev/uinput (needs write access to it) and reads one
command per line from FIFO: a button name (A B X Y LB RB BACK START GUIDE LS
RS), a D-pad direction (UP DOWN LEFT RIGHT), LT/RT, or "quit". Each command is
a short press. Runs until "quit" or until the FIFO's writers are gone for good.
"""
import os
import sys
import time

from evdev import AbsInfo, UInput, ecodes as e

BUTTONS = {
    "A": e.BTN_A, "B": e.BTN_B, "X": e.BTN_X, "Y": e.BTN_Y,
    "LB": e.BTN_TL, "RB": e.BTN_TR, "BACK": e.BTN_SELECT, "START": e.BTN_START,
    "GUIDE": e.BTN_MODE, "LS": e.BTN_THUMBL, "RS": e.BTN_THUMBR,
}
HAT = {"UP": (e.ABS_HAT0Y, -1), "DOWN": (e.ABS_HAT0Y, 1),
       "LEFT": (e.ABS_HAT0X, -1), "RIGHT": (e.ABS_HAT0X, 1)}
TRIGGERS = {"LT": e.ABS_Z, "RT": e.ABS_RZ}

STICK = AbsInfo(0, -32768, 32767, 16, 128, 0)
CAPS = {
    e.EV_KEY: list(BUTTONS.values()),
    e.EV_ABS: [(e.ABS_X, STICK), (e.ABS_Y, STICK), (e.ABS_RX, STICK), (e.ABS_RY, STICK),
               (e.ABS_Z, AbsInfo(0, 0, 255, 0, 0, 0)), (e.ABS_RZ, AbsInfo(0, 0, 255, 0, 0, 0)),
               (e.ABS_HAT0X, AbsInfo(0, -1, 1, 0, 0, 0)), (e.ABS_HAT0Y, AbsInfo(0, -1, 1, 0, 0, 0))],
}


def press(pad, name):
    if name in BUTTONS:
        pad.write(e.EV_KEY, BUTTONS[name], 1); pad.syn(); time.sleep(0.15)
        pad.write(e.EV_KEY, BUTTONS[name], 0); pad.syn()
    elif name in HAT:
        axis, value = HAT[name]
        pad.write(e.EV_ABS, axis, value); pad.syn(); time.sleep(0.15)
        pad.write(e.EV_ABS, axis, 0); pad.syn()
    elif name in TRIGGERS:
        pad.write(e.EV_ABS, TRIGGERS[name], 255); pad.syn(); time.sleep(0.15)
        pad.write(e.EV_ABS, TRIGGERS[name], 0); pad.syn()
    else:
        print(f"vpad: unknown command {name!r}", file=sys.stderr)


def main():
    fifo = sys.argv[1]
    pad = UInput(CAPS, name="Microsoft X-Box 360 pad", vendor=0x045E, product=0x028E,
                 version=0x110, bustype=e.BUS_USB)
    print(f"vpad: {pad.device.path}", flush=True)
    try:
        while True:
            with open(fifo) as commands:
                for line in commands:
                    name = line.strip().upper()
                    if name == "QUIT":
                        return
                    if name:
                        press(pad, name)
    finally:
        pad.close()


if __name__ == "__main__":
    main()
