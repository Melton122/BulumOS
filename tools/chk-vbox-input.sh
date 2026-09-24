#!/bin/bash
K=/home/melton/BulumOS/kernel/.config.x86_64
echo "== input/serio/psmouse =="
grep -E '^CONFIG_(INPUT|INPUT_MOUSEDEV|KEYBOARD_ATKBD|SERIO|SERIO_I8042|MOUSE_PS2|MOUSE_VMMOUSE)=' "$K"
echo "== fb/vga =="
grep -E '^CONFIG_(FB|FB_EFI|FRAMEBUFFER_CONSOLE|VGA_CONSOLE)=' "$K"
echo "== misc =="
grep -E '^CONFIG_(DEVTMPFS|SYSVIPC|EXT4_FS)=' "$K"