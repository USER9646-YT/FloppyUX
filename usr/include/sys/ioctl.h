#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H

#define TIOCSECHO  0x54F0     // terminal: echo off (0) or on (1)
#define TIOCSRAW   0x54F1     // terminal: raw (1) / line-buffered (0) mode
#define TIOCGWINSZ 0x5413     // terminal: get size (struct winsize *)

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

#endif