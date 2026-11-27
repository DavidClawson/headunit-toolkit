#pragma once
#include <stddef.h>
int access(const char *path, int mode);
int unlink(const char *path);
int usleep(unsigned us);
unsigned sleep(unsigned s);
int close(int fd);
int isatty(int fd);
#define F_OK 0
#define R_OK 4
#define W_OK 2
