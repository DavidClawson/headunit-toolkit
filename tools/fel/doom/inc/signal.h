#pragma once
typedef void (*sighandler_t)(int);
#define SIGINT 2
#define SIG_DFL ((sighandler_t)0)
sighandler_t signal(int s, sighandler_t h);
