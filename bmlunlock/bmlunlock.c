#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#define BML_UNLOCK_ALL				0x8A29		///< unlock all partition RO -> RW

int main(int argc, char** argv) {
    int fd = open("/dev/block/bml5", O_RDWR | O_LARGEFILE);
    return ioctl(fd, BML_UNLOCK_ALL, 0);
}
