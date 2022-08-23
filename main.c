/* main.c
**
** Configures BT-0240 Bluetooth RS232C Adapter
**
** License: GPL
**
** Emard
**
*/
#include <stdio.h>   /* Standard input/output definitions */
#include <string.h>  /* String function definitions */
#include <unistd.h>  /* UNIX standard function definitions */
#include <fcntl.h>   /* File control definitions */
#include <errno.h>   /* Error number definitions */
#include <termios.h> /* POSIX terminal control definitions */

// #include <sys/socket.h>

#include "dcm300.h"
#include "cmdline.h"

struct gengetopt_args_info args_info;
struct gengetopt_args_info *args = &args_info;

int fd;
char *device = "/dev/ttyS0";

int main(int argc, char **argv) 
{
#if 0
  struct bt_uart btuart;
  struct bt_role btrole;
  struct bt_identity btname;
  struct bt_security btsecurity;
  char string[256];
  char *peer = NULL;
  char *pin = NULL;
  char *name;
  int bps;
  char parity;
  int stopbits;
  int master = 0;
  int discoverable = 1;
  int set_role = 0, set_security = 0, set_uart = 0, set_name = 0;
#endif
#if 0
  struct sockaddr_rc mac = { 0 };
  char mac_str[100];
#endif

  cmdline_parser(argc, argv, args);
  verbose = args->verbose_given ? 1 : 0;
  dcm300->name = NULL;
  dcm300->simulation = 0;

  if(args->device_given)
  {
    dcm300->name = args->device_arg;
    if(strlen(dcm300->name) > 0)
      if(dcm300->name[0] == '/' || dcm300->name[0] == '.')
        dcm300->simulation = 1;
  }

  dcm300->exposure = args->exposure_arg;
  dcm300->red	   = args->red_arg;
  dcm300->green	   = args->green_arg;
  dcm300->blue	   = args->blue_arg;
  dcm300->output   = STDOUT_FILENO;
  
  dcm300->x        = 0;
  dcm300->y        = 0;
  dcm300->w        = 2048;
  dcm300->h        = 1536;

  dcm300->raw = args->raw_given ? 1 : 0;

  fd = dcm300_open(dcm300);

  if(fd < 0)
  {
    perror("can't open device");
    return 1;
  }

  dcm300_get_image(dcm300);

  dcm300_close(dcm300);
  
  return 0;
}
