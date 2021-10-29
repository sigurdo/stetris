# stetris

Play a bad version of tetris on a sensehat on a raspberry pi 4B.

Required OS is provided by NTNU. I should have a copy somewhere in my onedrive.

You sometimes need to modify the variables `X` and `fb_dev_path` to match whatever
the driver interface files are called in `/dev/`.

Run:

```
gcc -o stetris stetris.c && ./stetris
```

, or alternatively

```
make
```
