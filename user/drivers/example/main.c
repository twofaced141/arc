#include "libdriver.h"
#define DRIVER_NAME  "example"

void _start(void)
{
    puts("example: started\n");
    svc_register(DRIVER_NAME, (uint64_t)(uintptr_t)"example_driver");
    puts("\n");
    driver_exit(0);
}
