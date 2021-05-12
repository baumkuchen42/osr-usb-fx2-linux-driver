/*****************************************************
 * USB driver test application for the OSR FX2 board *
 * Nick Mikstas                                      *
 * Based on usb-skeleton.c and osrfx2.c              *
 *****************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <memory.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/poll.h>

#define BUF_LEN 9
#define SEG_LEN 6
#define BAR_LEN 6
#define CHAR_BUF_LEN 32
#define SLEEP_TIME 200000L

// returns a pointer to the bargraph attribute value
static char *get_bargraph_state(void) {
    const char *attrname = "/sys/class/usbmisc/osrfx2_0/device/bargraph";   
    char        attrvalue[BUF_LEN] = {0};
    int         fd, count;

    fd = open(attrname, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "Error opening %s\n", attrname);
        return NULL;
    }

    count = read(fd, &attrvalue, sizeof(attrvalue));
    close(fd);
    if (count == 8) {
    	// returns pointer to duplicate of queried attribute value
        return strdup(attrvalue);
    }

    return NULL;
}

static int set_bargraph_state(unsigned char value) {
    const char *attrname = "/sys/class/usbmisc/osrfx2_0/device/bargraph";
    char attrvalue [32];
    int  fd, count, len;
    
    // transforms input argument to decimal number
    snprintf(attrvalue, sizeof(attrvalue), "%d", value);
    len = strlen(attrvalue) + 1;

    fd = open(attrname, O_WRONLY);
    if (fd == -1) {
        return -1;
    }

    count = write(fd, &attrvalue, len);
    close(fd);
    // also return general error if not everything was written
    if (count != len) {
        return -1;
    }

    return 0;
}

int main(void) {
    const char *devpath = "/dev/osrfx2_0";
    char last_sw_status[BUF_LEN] = {0};
    char this_sw_status[BUF_LEN] = {0};
    char write_buffer[CHAR_BUF_LEN];
    char read_buffer[CHAR_BUF_LEN];
    unsigned long int dt = 0;
    int write_handle, read_handle, write_length, read_length;
    unsigned int packet_num = 0;
    int index = 0;

	// the pattern the led lights cycle through
    unsigned char bar_pattern [] = {0x01 | 0x80, 0x02 | 0x40, 0x04 | 0x20, 0x08 | 0x10, 0x04 | 0x20, 0x02 | 0x40};

    write_handle = open(devpath, O_WRONLY | O_NONBLOCK);
    if (write_handle == -1) {
        fprintf(stderr, "open for write: %s failed\n", devpath);
        return -1;
    }

    read_handle = open(devpath, O_RDONLY | O_NONBLOCK);
    if (read_handle == -1) {
        fprintf(stderr, "open for read: %s failed\n", devpath);
        return -1;
    }

    while(1) {  
 
        /*Report switch changes and current component states*/
        if(strcmp(last_sw_status, this_sw_status) != 0) {
            fprintf(stdout, "Bargraph status:  %s\n", get_bargraph_state());
            fprintf(stdout, "\n");
            strcpy(last_sw_status, this_sw_status);
        }

        /*Update and bargraph display*/
        set_bargraph_state(bar_pattern [index % BAR_LEN]);
        index++;

        /*Check if time to read/write to bulk endpoint*/
        // sends a test packet every 5 seconds
        if(dt >= 5000000L) {
            dt = 0;

            sprintf(write_buffer, "Test packet %u", packet_num);

            printf("Writing to bulk endpoint: %s\n", write_buffer);          

            /*Write to bulk endpoint*/
            write_length = write(write_handle, write_buffer, strlen(write_buffer));
            if (write_length < 0) {
                fprintf(stderr, "write error\n");
                return -1;
            }
 
            /*Initialize read buffer*/
            memset(read_buffer, 0, CHAR_BUF_LEN);

            /*Read from bulk endpoint*/
            read_length = read(read_handle, read_buffer, strlen(write_buffer));
            if (read_length < 0) {
                fprintf(stderr, "read error\n", read_length);
                return -1;
            }

            printf("Read from bulk endpoint:  %s\n\n", read_buffer);
            packet_num++;
        }

		// sleeps for certain amount of microseconds
        usleep(SLEEP_TIME);

        /*add elapsed sleep time to dt*/
        dt += SLEEP_TIME;
    }

    return 0;
}
