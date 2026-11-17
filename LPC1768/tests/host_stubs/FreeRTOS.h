/* Host test stand-in: 1 ms ticks from a clock the test moves */
#ifndef HOST_FREERTOS_H
#define HOST_FREERTOS_H
#include <stdint.h>
typedef uint32_t portTickType;
#define configTICK_RATE_HZ  1000U
#define portTICK_RATE_MS    1U
#endif
