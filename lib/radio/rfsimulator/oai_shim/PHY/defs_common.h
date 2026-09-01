#pragma once
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <math.h>

#define T(...)         do {} while (0)
#define T_INT(x)       (x)
#define T_BUFFER(p, n) (p), (n)
#define T_USRP_TX_ANT0 0
#define T_USRP_RX_ANT0 0
#define SPEED_OF_LIGHT 299792458.0
