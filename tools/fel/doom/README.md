# Doom on the head unit

[doomgeneric](https://github.com/ozkl/doomgeneric) running bare-metal on the
F133, loaded over FEL. It runs at 29 fps with stereo sound effects, and touch,
knob and key controls. There's no music yet.

**Licence:** this directory is **GPLv2**, the same as Doom and doomgeneric,
because our glue (`libc.c`, `dg_f133.c`, `snd_f133.c`, `inc/`) links against
them. The rest of the repository keeps its own licence.

```sh
./fetch.sh          # doomgeneric at a pinned commit + the shareware DOOM1.WAD (SHA-1 checked)
make                # -> doom.bin
cd .. && ./fel-catch.sh 600 && ./felrun.py doom/doom.bin --load 0x43800000:doom/doom1.wad --timeout 3600
```

| Control | Action |
|---|---|
| Touch the game area | Joystick from the centre: up/down to walk, left/right to turn |
| Left strip | Use (doors, switches) |
| Right strip | Fire |
| Top strip, phone key | Menu (Esc) |
| Bottom strip | Enter (Y on yes/no prompts) |
| Knob | Turn (up/down in menus) |
| Knob push | Fire (Enter in menus, Y on prompts) |
| Menu → Quit Game | Returns to FEL |

The details are in [docs/devlog.md](../../../docs/devlog.md) under "Doom runs
on the head unit".
