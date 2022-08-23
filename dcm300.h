#ifndef DCMROO_H
#define DCM300_H
#include <usb.h>
#include "binarytype.h"

/* struct for exchanging messages with dcm300 adapter */

#define MAXBULK 16384
#define BAYER_CIRCULAR 32768
#define RGB_MAX (3*BAYER_CIRCULAR/8)

/* commands that can be sent to dcm300 */

struct bt_commit {
  char command;
  char len;
  char data[1];
};

struct bt_command {
  char command;
  char len;
};

struct bt_role {
  char command;
  char len;
  char role;     /* see ROLE_* flags */ 
  char peer[1];
};

struct bt_uart {
  char command;
  char len;
  char rate;
  char stopbits;
  char parity;
};

struct bt_identity {
  char command;
  char len;
  char name[1];
};

struct bt_identity2 {
  char command;
  char len;
  char name[1];
};

struct bt_security {
  char command;
  char len;
  char encryption;
  char pin_len;
  char pin[1];
};

struct dcm300 {
  int fd; /* raw file open descriptor */
  usb_dev_handle *usb_dev_handle; /* open libusb device */
  char *name; /* device name or raw image filename */
  int simulation; /* 0-use real hardware 1-simulation using raw file */
  u16 x, y; /* offset from where to grab the image5~ */
  u16 w, h; /* x-width, y-height of the image */
  u16 exposure;
  s8 red, green, blue; /* RGB gain */
  int raw; /* 0-downscale 1-output raw bayer data */
  int output; /* output file descriptor */
  int bayer_from; /* from this byte of output start bayer data */
  int bayer_read; /* total bytes of raw bayer stream read so far, index to bayer circular */
  int bayer_end; /* end of bayer data */
  int bayer_width; /* how many bytes has one RGGB line */
  u8 bayer_circular[BAYER_CIRCULAR]; /* circular buffer for bayer conversion on-the-fly */
};

/* request for dcm300 snapshot packet 
*/
struct dcm300_request {
  unsigned char unknown1a;      /* 2c */
  unsigned char unknown1a_0[1]; /* 00 */
  unsigned char unknown1b;      /* 0e */
  unsigned char unknown1b_0[1]; /* 00 */
  unsigned char unknown1c;      /* 01 */
  unsigned char unknown1_0[7];  /* 00 ... */
  unsigned char unknown2;       /* 20 */
  unsigned char unknown2_0[1];  /* 00 ... */
  unsigned char gamma;          /* maybe gamma maybe not, values seen: bf,d6,da,b0,e1 */
  unsigned char unknown3;       /* 05 */
  unsigned char resolution_y_lo, resolution_y_hi;
  unsigned char unknown3_0[2];  /* 00 ... */
  unsigned char resolution_x_lo, resolution_x_hi;
  unsigned char unknown4_0[2];  /* 00 ... */
  unsigned char offset_x_lo, offset_x_hi;
  unsigned char offset_x_hlo, offset_x_hhi;
  // unsigned char unknown5_0[2];  /* 00 ... */
  unsigned char offset_y_lo, offset_y_hi;
  unsigned char unknown6_0[2];  /* 00 ... */
  unsigned char exposure_lo, exposure_hi; /* lo,hi = exp + 20 */
  unsigned char unknown7_0[2];  /* 00 ... */
  char gain_red, gain_green, gain_blue;
  unsigned char unknown8_0[1];
  unsigned char unknown9;       /* 02 */
  unsigned char unknown9_0[23]; /* 00 ... */
};



/* list of supported devices */
struct usb_vendor_product {
 u16 vendor_id, product_id;
 char *name;
};



extern struct usb_vendor_product usb_vendor_product_list[];
        
        
extern int bt_rate2int[], bt_rate2int_n;
extern char bt_parity2char[];
extern int bt_stopbits2int[];
#if 0
extern int fd;
extern char *device;
#endif
extern struct dcm300 *dcm300;
extern int verbose;

int dcm300_open(struct dcm300 *dcm300);
int dcm300_close(struct dcm300 *dcm300);
int dcm300_read(struct dcm300 *dcm300, u8 *buffer, int bytes);
int dcm300_write(struct dcm300 *dcm300, u8 *buffer, int bytes);
int dcm300_get_image(struct dcm300 *dcm300);

int bt_close(struct dcm300 *bt);

int bt_copystring(char *packet, char *string, int maxlen);

int bt_read_security(struct dcm300 *bt, struct bt_security *security);
int bt_read_identity(struct dcm300 *bt, struct bt_identity *name);
int bt_read_uart(struct dcm300 *bt, struct bt_uart *uart);
int bt_read_role(struct dcm300 *bt, struct bt_role *role);

int bt_set_identity(struct dcm300 *bt, char *name);
int bt_set_uart(struct dcm300 *bt, int rate, char parity, int stopbits);
int bt_set_security(struct dcm300 *bt, char *pin);
int bt_set_role(struct dcm300 *bt, int master, int discoverable, char *peer);

#endif
