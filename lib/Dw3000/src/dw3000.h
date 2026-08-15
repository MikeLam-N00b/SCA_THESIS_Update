#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <Arduino.h>
#include "dw3000_uart.h"
#include "dw3000_port.h"
#include "dw3000_device_api.h"
#include "dw3000_shared_functions.h"

#define _BV(n)   (1 << (n))
#define __INLINE inline
