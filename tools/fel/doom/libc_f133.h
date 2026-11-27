/* libc_f133.h: the Doom port's memory map and exit hook (libc.c). */
#pragma once
#include <stddef.h>
#define WAD_ADDR 0x43800000u          /* felrun --load 0x43800000:doom1.wad */
extern void *libc_exit_jmp[5];        /* __builtin_setjmp buffer: exit() lands here */
extern int libc_exit_code;
size_t libc_heap_used(void);
