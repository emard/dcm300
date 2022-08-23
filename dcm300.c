#include "dcm300.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

int verbose = 0;

struct dcm300 dcm300_device;
struct dcm300 *dcm300 = &dcm300_device;
struct usb_vendor_product usb_vendor_product_list[] = {
  { 0x1578, 0x0076, "DCM300" },
  { 0, 0, NULL },
};

/* opens raw image data
** sets serial parameters 
** and returns file descriptor id 
*/
int dcm300_open_simulation(struct dcm300 *dcm300)
{
  int fd = -1;
  int retry = 0;
  char *device_name = dcm300->name;

  dcm300->fd = -1;
  if(dcm300->simulation != 1)
    return fd;
  for(retry = 0; fd < 0 && retry < 1; retry++)
  {
    fd = open(device_name, O_RDONLY);
    if(fd >= 0)
      break;
  }
  dcm300->fd = fd;
  
  if (fd == -1) {
    perror("dcm300_open: Unable to open raw image file");
    return -1;
  }

  return fd;
}

/* opens raw image data
** sets serial parameters 
** and returns file descriptor id 
*/
int dcm300_close_simulation(struct dcm300 *dcm300)
{
  if(dcm300->simulation != 1)
    return 0;
  if(dcm300->fd > 0)
    close(dcm300->fd);
  return 0;
}

int dcm300_read_simulation(struct dcm300 *dcm300, u8 *buffer, int bytes)
{
  if(dcm300->simulation != 1)
    return 0;
  if(dcm300->fd > 0)
    return read(dcm300->fd, buffer, bytes);
  return 0;
}

/*
** this will only rewind a file to its beginning
** simulates a request for image written to the camera
*/
int dcm300_write_simulation(struct dcm300 *dcm300, u8 *buffer, int bytes)
{
  if(dcm300->simulation != 1)
    return 0;
  if(dcm300->fd > 0)
    lseek(dcm300->fd, 0, SEEK_SET);
  return bytes;
}


int dcm300_find_hardware(struct dcm300 *dcm300)
{
  struct usb_bus *busses;
  struct usb_bus *bus;
  struct usb_device *dev;
  usb_dev_handle *usbdev;
  struct usb_vendor_product *supported;
  int i;

  /* Find the device */
  usb_init();
  usb_find_busses();
  usb_find_devices();
  busses = usb_get_busses();
  
  dcm300->usb_dev_handle = NULL;

  for(bus=busses; bus; bus=bus->next) {
    for(dev=bus->devices; dev; dev=dev->next) {
      for(i = 0; usb_vendor_product_list[i].vendor_id != 0; i++)
      {
        supported = &(usb_vendor_product_list[i]);
        if(dev->descriptor.idVendor  == supported->vendor_id &&
           dev->descriptor.idProduct == supported->product_id) 
        {
          fprintf(stderr, "found: %s\n", supported->name);
          /* supported device found */
	  usbdev = usb_open(dev);
	  dcm300->usb_dev_handle = usbdev;
#if 0
          tusb_info->usb_vendor_product = supported;
#endif
          if(usb_claim_interface(usbdev, 0)) {
            fprintf(stderr, "usb_claim_interface failed\n");
          }
          else
          {
            return 0;
#if 0
            dcm300_capture(tusb_info);
#endif
          }
        }
      }
    }
  }
  return -1;

}

int dcm300_write_hardware(struct dcm300 *dcm300, u8 *buffer, int bytes)
{
  if(dcm300->simulation == 1)
    return 0;
  if(dcm300->usb_dev_handle)
    return usb_bulk_write(dcm300->usb_dev_handle, 2, (char *) buffer, bytes, 500);
  return 0;
}

int dcm300_read_hardware(struct dcm300 *dcm300, u8 *buffer, int bytes)
{
  if(dcm300->simulation == 1)
    return 0;
  if(dcm300->usb_dev_handle)
    return usb_bulk_read(dcm300->usb_dev_handle, 6, (char *)buffer, bytes, 2000);
  return 0;
}

int dcm300_create_request(struct dcm300 *dcm300, struct dcm300_request *r)
{
  memset(r, 0, sizeof(*r));
  
  r->unknown1a = 0x2c;
  r->unknown1b = 0x0e;
  r->unknown1c = 0x01;

  r->unknown2 = 0x20;
  r->unknown3 = 0x05;
  r->unknown9 = 0x02;
  
  r->resolution_x_lo = (dcm300->w) % 256;
  r->resolution_x_hi = (dcm300->w) / 256;
  r->resolution_y_lo = (dcm300->h) % 256;
  r->resolution_y_hi = (dcm300->h) / 256;

  r->offset_x_lo = (dcm300->x) % 256;
  r->offset_x_hi = (dcm300->x) / 256;

  r->offset_y_lo = (dcm300->y) % 256;
  r->offset_y_hi = (dcm300->y) / 256;

  r->exposure_lo = (dcm300->exposure + 20) % 256;
  r->exposure_hi = (dcm300->exposure + 20) / 256;

  r->gain_red   = dcm300->red;
  r->gain_green = dcm300->green;
  r->gain_blue  = dcm300->blue;

  r->gamma = 191;

  return 0;
}


int dcm300_open(struct dcm300 *dcm300)
{
  if (dcm300->simulation == 1)
    return dcm300_open_simulation(dcm300);
  else
  {
    if(dcm300_find_hardware(dcm300))
      return -1;
    else
    {
      // hardware found (and opened)
      return 0;
    }
  }
  return -1;
}

int dcm300_close(struct dcm300 *dcm300)
{
  if (dcm300->simulation == 1)
    return dcm300_close_simulation(dcm300);
  return -1;
}

int dcm300_read(struct dcm300 *dcm300, u8 *buffer, int bytes)
{
  if (dcm300->simulation == 1)
    return dcm300_read_simulation(dcm300, buffer, bytes);
  else
    return dcm300_read_hardware(dcm300, buffer, bytes);
  return -1;
}

int dcm300_write(struct dcm300 *dcm300, u8 *buffer, int bytes)
{
  if (dcm300->simulation == 1)
    return dcm300_write_simulation(dcm300, buffer, bytes);
  else
    return dcm300_write_hardware(dcm300, buffer, bytes);
  return -1;
}

/*
** pointer of the next byte to be written in the circular buffer
*/
u8* dcm300_circular(struct dcm300 *dcm300)
{
  return dcm300->bayer_circular + ((unsigned int)(dcm300->bayer_read) % BAYER_CIRCULAR);
}


/* do the bayer on-the-fly using a circular buffer */
int dcm300_output_bayer(struct dcm300 *dcm300, int len)
{
  int  i, j, irgb;
  u8 rgb_array[RGB_MAX];
  u8 *bayer_array;
  int *bayer_start, bayer_stop, bayer_last, bayer_width;

  bayer_array = dcm300->bayer_circular;

#if 0
  fprintf(stderr, "bayer from=%08x read=%08x len=%d\n",
   dcm300->bayer_from, dcm300->bayer_read, len);

  fprintf(stderr, "image %dx%d\n", dcm300->w, dcm300->h);
#endif

  bayer_width = dcm300->bayer_width;
  bayer_start = &(dcm300->bayer_from);
  bayer_stop = dcm300->bayer_read + len;
  if(bayer_stop > dcm300->bayer_end)
    bayer_stop = dcm300->bayer_end;
  /* even number of bayer lines because they come as alternating RG and GB rows*/  
  bayer_last = *bayer_start + (bayer_stop - *bayer_start) - ((bayer_stop - *bayer_start) % (2*bayer_width));
#if 0
  fprintf(stderr, "bayer: start=%d read=%d stop=%d last=%d width=%d ", 
    *bayer_start, dcm300->bayer_read, bayer_stop, bayer_last, bayer_width);
#endif
  irgb = 0;
  for(i = *bayer_start; i < bayer_last; i += 2*bayer_width )
  {
    for(j = i; j < i + bayer_width; j += 2)
    {
      rgb_array[irgb++] = bayer_array[ (j) % BAYER_CIRCULAR];
      rgb_array[irgb++] = (bayer_array[ (j + 1) % BAYER_CIRCULAR] + bayer_array[ (j + bayer_width) % BAYER_CIRCULAR ])/2;
      rgb_array[irgb++] = bayer_array[ (j + bayer_width + 1) % BAYER_CIRCULAR];
      if(irgb > RGB_MAX)
      {
        // DBG(1, "cat't fit to rgb - need bayer force exit\n");
        goto force_exit;
      }
    }
  }
  
  force_exit:;
  
  *bayer_start = i;
  dcm300->bayer_read = i;
  // *rgb_len = irgb;
  /* write the data */
#if 0
  fprintf(stderr, "bayer out irgb=%d\n", irgb);
#endif
  write(dcm300->output, rgb_array, irgb);

  return 0;
}

/* output raw ayer data or make the downscale to RGB */
int dcm300_output(struct dcm300 *dcm300, int len)
{
  if(len > 0)
  {
   if(dcm300->raw)
     write(dcm300->output, dcm300_circular(dcm300), len);
   else
     dcm300_output_bayer(dcm300, len);
  }
  return 0;
}

/* output image header */
int dcm300_output_header(struct dcm300 *dcm300)
{
  char *buffer;
  
  buffer = (char *)dcm300_circular(dcm300);
  
  *buffer = 0;
  
  if(dcm300->raw)
    sprintf(buffer, "%s", "");
  else
    sprintf(buffer, "P6\n%d %d\n255\n", dcm300->w / 2, dcm300->h / 2);
   
  write(dcm300->output, buffer, strlen(buffer));

  return 0;
}

int dcm300_get_image(struct dcm300 *dcm300)
{
  int i, len, want_bytes;
  int expect_image;
  struct dcm300 dcm300small[1];
  struct dcm300_request request[1];

  /* by experimentation I've found out that
  ** there must be 2 consecutive snapshotting with
  ** dcm300 otherwise it becomes unstable 
  ** (bulk read may fail)
  ** to gain some speed, we take small snapshot of
  ** 128x128 size. We don't use image obtained here.
  */
  memcpy(dcm300small, dcm300, sizeof(*dcm300));
  dcm300small->x = dcm300small->y = 0;
  dcm300small->w = dcm300small->h = 128;
  expect_image = dcm300small->w * dcm300small->h;
  dcm300_create_request(dcm300small, request);
  dcm300_write(dcm300small, (u8 *) request, sizeof(request));
  want_bytes = 64;
  len = dcm300_read(dcm300small, dcm300_circular(dcm300small), want_bytes);
  if(len == want_bytes) fprintf(stderr, "[");
  len = want_bytes = MAXBULK;
  for(i = 0; i < expect_image && len == want_bytes; i += len)
  {
    len = dcm300_read(dcm300small, dcm300_circular(dcm300small), want_bytes);
    if(len == want_bytes) fprintf(stderr, ".");
  }
  want_bytes = 256;
  len = dcm300_read(dcm300small, dcm300_circular(dcm300small), want_bytes);
  if(len == want_bytes) fprintf(stderr, "]");
  
  /*
  ** We take the real size snapshot and we do
  ** on-the-fly demoaicing and writing the image to stdout
  ** by experiment there seems some image sizes work stable
  ** and same don't. Here's maximum resolution 2048x1536 and
  ** it's among stable. Longer exposure values (above about 400)
  ** tend to be unstable. Bulk read fails.
  **
  ** BUG: sometimes the image partially gets more exposure.
  ** Upper part of the image has normal exposure and lower
  ** part is about double exposure. Upper and lower part
  ** get divided at random position.
  **
  ** If the image snapshot is taken at regular intervals 
  ** every 1s from 'watch' command and with default
  ** parameters, this bug almost never happens (on my laptop).
  ** It often happens if the snapshot is taken at random times
  */
  expect_image = dcm300->w * dcm300->h;
  dcm300->bayer_from = 0; /* from 64th byte starts next block of bayer image raw data */
  dcm300->bayer_read = -64; /* total raw bayer bytes read so far... */
  dcm300->bayer_width = dcm300->w; /* one bayer horizontal line */
  dcm300->bayer_end = dcm300->bayer_from + expect_image;

  dcm300_create_request(dcm300, request);
  dcm300_write(dcm300, (u8 *) request, sizeof(request));
  
  dcm300_output_header(dcm300);
  want_bytes = 64;
  len = dcm300_read(dcm300, dcm300_circular(dcm300), want_bytes);
  if(len == want_bytes) fprintf(stderr, "[");
#if 0
  fprintf(stderr, "image %dx%d\n", dcm300->w, dcm300->h);
#endif
  dcm300_output(dcm300, len);
  len = want_bytes = MAXBULK;
  for(i = 0; i < expect_image && len == want_bytes; i += len)
  {
    len = dcm300_read(dcm300, dcm300_circular(dcm300), want_bytes);
    if(len == want_bytes) fprintf(stderr, ".");
    dcm300_output(dcm300, len);
  }
  want_bytes = 256;
  len = dcm300_read(dcm300, dcm300_circular(dcm300), want_bytes);
  if(len == want_bytes) fprintf(stderr, "]");
  dcm300_output(dcm300, len);
  fprintf(stderr, "\n");
  return 0;
}
