/* the demo workspace's C file - syntax highlighting exhibit A */
#include <stdio.h>

#define GREETING "Hello from UnoCode Desktop"

int main(void)
{
    int i;
    for (i = 0; i < 3; i++)
        printf("%s (%d)\n", GREETING, i);
    return 0;
}
