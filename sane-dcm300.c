/* sane - Scanner Access Now Easy.

   This file is part of the SANE package.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston,
   MA 02111-1307, USA.

   As a special exception, the authors of SANE give permission for
   additional uses of the libraries contained in this release of SANE.

   The exception is that, if you link a SANE library with other files
   to produce an executable, this does not by itself cause the
   resulting executable to be covered by the GNU General Public
   License.  Your use of that executable is in no way restricted on
   account of linking the SANE library code into it.

   This exception does not, however, invalidate any other reasons why
   the executable file might be covered by the GNU General Public
   License.

   If you submit changes to SANE to the maintainers to be included in
   a subsequent release, you agree by submitting the changes that
   those changes may be distributed with this exception intact.

   If you write modifications of your own for SANE, it is your choice
   whether to permit this exception to apply to your modifications.
   If you do not wish that, delete this exception notice.

   --------------------------------------------------------------------------

   This file implements a SANE backend for HP ScanJet 3500 series scanners.
   Currently supported:
    - HP ScanJet 3500C
    - HP ScanJet 3530C
    - HP ScanJet 3570C

   SANE FLOW DIAGRAM

   - sane_init() : initialize backend, attach scanners
   . - sane_get_devices() : query list of scanner devices
   . - sane_open() : open a particular scanner device
   . . - sane_set_io_mode : set blocking mode
   . . - sane_get_select_fd : get scanner fd
   . . - sane_get_option_descriptor() : get option information
   . . - sane_control_option() : change option values
   . .
   . . - sane_start() : start image acquisition
   . .   - sane_get_parameters() : returns actual scan parameters
   . .   - sane_read() : read image data (from pipe)
   . .
   . . - sane_cancel() : cancel operation
   . - sane_close() : close opened scanner device
   - sane_exit() : terminate use of backend


   There are some device specific routines in this file that are in "#if 0"
   sections - these are left in place for documentation purposes in case
   somebody wants to implement features that use those routines.

*/

/* ------------------------------------------------------------------------- */

#include "sane/config.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include <sys/types.h>
#include <unistd.h>
#ifdef HAVE_LIBC_H
# include <libc.h>		/* NeXTStep/OpenStep */
#endif

#include "sane/sane.h"
#include "sane/sanei_usb.h"
#include "sane/saneopts.h"
#include "sane/sanei_config.h"
#include "sane/sanei_thread.h"
#include "sane/sanei_backend.h"

typedef int (*dcm300_callback) (void *param, unsigned bytes, void *data);

#define DEBUG 1
#define MM_PER_INCH 1
#define SCANNER_UNIT_TO_FIXED_MM(number) (number * MM_PER_INCH / 1)
#define FIXED_MM_TO_SCANNER_UNIT(number) (number * 1 / MM_PER_INCH)

#define MSG_ERR         1
#define MSG_USER        5
#define MSG_INFO        6
#define FLOW_CONTROL    10
#define MSG_IO          15
#define MSG_IO_READ     17
#define IO_CMD          20
#define IO_CMD_RES      20
#define MSG_GET         25
/* ------------------------------------------------------------------------- */

enum dcm300_option
{
  OPT_NUM_OPTS = 0,

/*  OPT_RESOLUTION, */
  OPT_EXPOSURE,
  OPT_GAIN_RED,
  OPT_GAIN_GREEN,
  OPT_GAIN_BLUE,
  OPT_GEOMETRY_GROUP,
  OPT_TL_X,
  OPT_TL_Y,
  OPT_BR_X,
  OPT_BR_Y,
  OPT_MODE_GROUP,
  OPT_MODE,

  NUM_OPTIONS
};

typedef struct
{
  int left;
  int top;
  int right;
  int bottom;
} dcm300_rect;

struct dcm300_data
{
  struct dcm300_data *next;
  char *devicename;

  int sfd;
  int pipe_r;
  int pipe_w;
  int reader_pid;

  int resolution;
  int exposure;
  int gain_red, gain_green, gain_blue;
  int mode;

  time_t last_scan;

  dcm300_rect request_pixel;
#if 0
  int rounded_left;
  int rounded_top;
  int rounded_right;
  int rounded_bottom;
#endif
  int bytes_per_scan_line;
  int scan_width_pixels;
  int scan_height_pixels;

  SANE_Option_Descriptor opt[NUM_OPTIONS];
  SANE_Device sane;
};

struct dcm300_write_info
{
  struct dcm300_data *scanner;
  int bytesleft;
};

typedef struct detailed_calibration_data
{
  unsigned char const *channeldata[3];
  unsigned resolution_divisor;
} detailed_calibration_data;

static struct dcm300_data *first_dev = 0;
static struct dcm300_data **new_dev = &first_dev;
static int num_devices = 0;
static SANE_Int res_list[] =
  { 1, 1 }; /* list: number of items, item1, ... */
static const SANE_Range range_x =
  { 0, 1023, 1 }; /* range: min, max, quantization */
static const SANE_Range range_y =
  { 0, 767, 1 };
static const SANE_Range range_exp =
  { 0, 2999, 1 };
static const SANE_Range range_gain_red =
  { 0, 255, 1 };
static const SANE_Range range_gain_green =
  { 0, 255, 1 };
static const SANE_Range range_gain_blue =
  { 0, 255, 1 };

#define HP3500_COLOR_SCAN 0
#define HP3500_TOTAL_SCANS 1

static char const *scan_mode_list[HP3500_TOTAL_SCANS + 1] = { 0 };

static SANE_Status attachScanner (const char *name);
static SANE_Status init_options (struct dcm300_data *scanner);
static int reader_process (void *);
static void calculateDerivedValues (struct dcm300_data *scanner);
static void do_reset (struct dcm300_data *scanner);
static void do_cancel (struct dcm300_data *scanner);

/*
 * used by sane_get_devices
 */
static const SANE_Device **devlist = 0;

/*
 * SANE Interface
 */


/**
 * Called by SANE initially.
 * 
 * From the SANE spec:
 * This function must be called before any other SANE function can be
 * called. The behavior of a SANE backend is undefined if this
 * function is not called first. The version code of the backend is
 * returned in the value pointed to by version_code. If that pointer
 * is NULL, no version code is returned. Argument authorize is either
 * a pointer to a function that is invoked when the backend requires
 * authentication for a specific resource or NULL if the frontend does
 * not support authentication.
 */
SANE_Status
sane_init (SANE_Int * version_code, SANE_Auth_Callback authorize)
{
  authorize = authorize;	/* get rid of compiler warning */

  DBG_INIT ();
  DBG (10, "sane_init\n");

  sanei_usb_init ();
  sanei_thread_init ();

  if (version_code)
    *version_code = SANE_VERSION_CODE (V_MAJOR, V_MINOR, 0);

  DBG (10, "usb find devices\n");
  sanei_usb_find_devices (0x1578, 0x0076, attachScanner);

  return SANE_STATUS_GOOD;
}


/**
 * Called by SANE to find out about supported devices.
 * 
 * From the SANE spec:
 * This function can be used to query the list of devices that are
 * available. If the function executes successfully, it stores a
 * pointer to a NULL terminated array of pointers to SANE_Device
 * structures in *device_list. The returned list is guaranteed to
 * remain unchanged and valid until (a) another call to this function
 * is performed or (b) a call to sane_exit() is performed. This
 * function can be called repeatedly to detect when new devices become
 * available. If argument local_only is true, only local devices are
 * returned (devices directly attached to the machine that SANE is
 * running on). If it is false, the device list includes all remote
 * devices that are accessible to the SANE library.
 * 
 * SANE does not require that this function is called before a
 * sane_open() call is performed. A device name may be specified
 * explicitly by a user which would make it unnecessary and
 * undesirable to call this function first.
 */
SANE_Status
sane_get_devices (const SANE_Device *** device_list, SANE_Bool local_only)
{
  int i;
  struct dcm300_data *dev;

  DBG (10, "sane_get_devices local_only=%d num_devices=%d \n", local_only, num_devices);
  
  if (devlist)
    free (devlist);
  devlist = calloc (num_devices + 1, sizeof (SANE_Device *));
  if (!devlist)
    return SANE_STATUS_NO_MEM;

  for (dev = first_dev, i = 0; i < num_devices; dev = dev->next)
    devlist[i++] = &dev->sane;
  devlist[i++] = 0;

  *device_list = devlist;

  return SANE_STATUS_GOOD;
}


/**
 * Called to establish connection with the scanner. This function will
 * also establish meaningful defauls and initialize the options.
 *
 * From the SANE spec:
 * This function is used to establish a connection to a particular
 * device. The name of the device to be opened is passed in argument
 * name. If the call completes successfully, a handle for the device
 * is returned in *h. As a special case, specifying a zero-length
 * string as the device requests opening the first available device
 * (if there is such a device).
 */
SANE_Status
sane_open (SANE_String_Const name, SANE_Handle * handle)
{
  struct dcm300_data *dev = NULL;
  struct dcm300_data *scanner = NULL;

  if (name[0] == 0)
    {
      DBG (10, "sane_open: no device requested, using default\n");
      if (first_dev)
	{
	  scanner = (struct dcm300_data *) first_dev;
	  DBG (10, "sane_open: device %s found\n", first_dev->sane.name);
	}
    }
  else
    {
      DBG (10, "sane_open: device %s requested\n", name);

      for (dev = first_dev; dev; dev = dev->next)
	{
	  if (strcmp (dev->sane.name, name) == 0)
	    {
	      DBG (10, "sane_open: device %s found\n", name);
	      scanner = (struct dcm300_data *) dev;
	    }
	}
    }

  if (!scanner)
    {
      DBG (10, "sane_open: no device found\n");
      return SANE_STATUS_INVAL;
    }

  *handle = scanner;

  init_options (scanner);

  scanner->resolution = 1;
  scanner->exposure = 200;
  scanner->gain_red = 31;
  scanner->gain_green = 25;
  scanner->gain_blue = 40;
  scanner->request_pixel.left = 0;
  scanner->request_pixel.top = 0;
  scanner->request_pixel.right = 1023;
  scanner->request_pixel.bottom = 767;
  scanner->mode = 0;
  DBG (10, "resoluton=%d,left=%d top=%d right=%d bottom=%d\n", 
    scanner->resolution, 
    scanner->request_pixel.left, scanner->request_pixel.top, 
    scanner->request_pixel.right, scanner->request_pixel.bottom );
  calculateDerivedValues (scanner);

  return SANE_STATUS_GOOD;

}


/**
 * An advanced method we don't support but have to define.
 */
SANE_Status
sane_set_io_mode (SANE_Handle h, SANE_Bool non_blocking)
{
  DBG (10, "sane_set_io_mode\n");
  DBG (99, "%d %p\n", non_blocking, h);
  return SANE_STATUS_UNSUPPORTED;
}


/**
 * An advanced method we don't support but have to define.
 */
SANE_Status
sane_get_select_fd (SANE_Handle h, SANE_Int * fdp)
{
  struct dcm300_data *scanner = (struct dcm300_data *) h;
  DBG (10, "sane_get_select_fd\n");
  *fdp = scanner->pipe_r;
  DBG (99, "%p %d\n", h, *fdp);
  return SANE_STATUS_GOOD;
}


/**
 * Returns the options we know.
 *
 * From the SANE spec:
 * This function is used to access option descriptors. The function
 * returns the option descriptor for option number n of the device
 * represented by handle h. Option number 0 is guaranteed to be a
 * valid option. Its value is an integer that specifies the number of
 * options that are available for device handle h (the count includes
 * option 0). If n is not a valid option index, the function returns
 * NULL. The returned option descriptor is guaranteed to remain valid
 * (and at the returned address) until the device is closed.
 */
const SANE_Option_Descriptor *
sane_get_option_descriptor (SANE_Handle handle, SANE_Int option)
{
  struct dcm300_data *scanner = handle;

  DBG (MSG_GET,
       "sane_get_option_descriptor: \"%s\"\n", scanner->opt[option].name);

  if ((unsigned) option >= NUM_OPTIONS)
    return NULL;
  return &scanner->opt[option];
}


/**
 * Gets or sets an option value.
 * 
 * From the SANE spec:
 * This function is used to set or inquire the current value of option
 * number n of the device represented by handle h. The manner in which
 * the option is controlled is specified by parameter action. The
 * possible values of this parameter are described in more detail
 * below.  The value of the option is passed through argument val. It
 * is a pointer to the memory that holds the option value. The memory
 * area pointed to by v must be big enough to hold the entire option
 * value (determined by member size in the corresponding option
 * descriptor).
 * 
 * The only exception to this rule is that when setting the value of a
 * string option, the string pointed to by argument v may be shorter
 * since the backend will stop reading the option value upon
 * encountering the first NUL terminator in the string. If argument i
 * is not NULL, the value of *i will be set to provide details on how
 * well the request has been met.
 */
SANE_Status
sane_control_option (SANE_Handle handle, SANE_Int option,
		     SANE_Action action, void *val, SANE_Int * info)
{
  struct dcm300_data *scanner = (struct dcm300_data *) handle;
  SANE_Status status;
  SANE_Word cap;
  SANE_Int dummy;
  int i;

  /* Make sure that all those statements involving *info cannot break (better
   * than having to do "if (info) ..." everywhere!)
   */
  if (info == 0)
    info = &dummy;

  *info = 0;

  if (option >= NUM_OPTIONS)
    return SANE_STATUS_INVAL;

  cap = scanner->opt[option].cap;

  /*
   * SANE_ACTION_GET_VALUE: We have to find out the current setting and
   * return it in a human-readable form (often, text).
   */
  if (action == SANE_ACTION_GET_VALUE)
    {
      DBG (MSG_GET, "sane_control_option: get value \"%s\"\n",
	   scanner->opt[option].name);
      DBG (11, "\tcap = %d\n", cap);

      if (!SANE_OPTION_IS_ACTIVE (cap))
	{
	  DBG (10, "\tinactive\n");
	  return SANE_STATUS_INVAL;
	}

      switch (option)
	{
	case OPT_NUM_OPTS:
	  *(SANE_Word *) val = NUM_OPTIONS;
	  return SANE_STATUS_GOOD;

#if 0
	case OPT_RESOLUTION:
	  *(SANE_Word *) val = scanner->resolution;
	  return SANE_STATUS_GOOD;
#endif
	case OPT_EXPOSURE:
	  *(SANE_Word *) val = scanner->exposure;
	  return SANE_STATUS_GOOD;

	case OPT_GAIN_RED:
	  *(SANE_Word *) val = scanner->gain_red;
	  return SANE_STATUS_GOOD;

	case OPT_GAIN_GREEN:
	  *(SANE_Word *) val = scanner->gain_green;
	  return SANE_STATUS_GOOD;

	case OPT_GAIN_BLUE:
	  *(SANE_Word *) val = scanner->gain_blue;
	  return SANE_STATUS_GOOD;

	case OPT_TL_X:
	  *(SANE_Word *) val = scanner->request_pixel.left;
	  return SANE_STATUS_GOOD;

	case OPT_TL_Y:
	  *(SANE_Word *) val = scanner->request_pixel.top;
	  return SANE_STATUS_GOOD;

	case OPT_BR_X:
	  *(SANE_Word *) val = scanner->request_pixel.right;
	  return SANE_STATUS_GOOD;

	case OPT_BR_Y:
	  *(SANE_Word *) val = scanner->request_pixel.bottom;
	  return SANE_STATUS_GOOD;

	case OPT_MODE:
	  strcpy ((SANE_Char *) val, scan_mode_list[scanner->mode]);
	  return SANE_STATUS_GOOD;
	}
    }
  else if (action == SANE_ACTION_SET_VALUE)
    {
      DBG (10, "sane_control_option: set value \"%s\"\n",
	   scanner->opt[option].name);

      if (!SANE_OPTION_IS_ACTIVE (cap))
	{
	  DBG (10, "\tinactive\n");
	  return SANE_STATUS_INVAL;
	}

      if (!SANE_OPTION_IS_SETTABLE (cap))
	{
	  DBG (10, "\tnot settable\n");
	  return SANE_STATUS_INVAL;
	}

      status = sanei_constrain_value (scanner->opt + option, val, info);
      if (status != SANE_STATUS_GOOD)
	{
	  DBG (10, "\tbad value\n");
	  return status;
	}

      /*
       * Note - for those options which can assume one of a list of
       * valid values, we can safely assume that they will have
       * exactly one of those values because that's what
       * sanei_constrain_value does. Hence no "else: invalid" branches
       * below.
       */
      switch (option)
	{
#if 0
	case OPT_RESOLUTION:
	  if (scanner->resolution == *(SANE_Word *) val)
	    {
	      return SANE_STATUS_GOOD;
	    }
	  scanner->resolution = (*(SANE_Word *) val);
	  calculateDerivedValues (scanner);
	  *info |= SANE_INFO_RELOAD_PARAMS;
	  return SANE_STATUS_GOOD;
#endif
	case OPT_EXPOSURE:
	  if (scanner->exposure == *(SANE_Word *) val)
	    {
	      return SANE_STATUS_GOOD;
	    }
	  scanner->exposure = (*(SANE_Word *) val);
	  calculateDerivedValues (scanner);
	  *info |= SANE_INFO_RELOAD_PARAMS;
	  return SANE_STATUS_GOOD;

	case OPT_GAIN_RED:
	  if (scanner->gain_red == *(SANE_Word *) val)
	    {
	      return SANE_STATUS_GOOD;
	    }
	  scanner->gain_red = (*(SANE_Word *) val);
	  calculateDerivedValues (scanner);
	  *info |= SANE_INFO_RELOAD_PARAMS;
	  return SANE_STATUS_GOOD;

	case OPT_GAIN_GREEN:
	  if (scanner->gain_green == *(SANE_Word *) val)
	    {
	      return SANE_STATUS_GOOD;
	    }
	  scanner->gain_green = (*(SANE_Word *) val);
	  calculateDerivedValues (scanner);
	  *info |= SANE_INFO_RELOAD_PARAMS;
	  return SANE_STATUS_GOOD;

	case OPT_GAIN_BLUE:
	  if (scanner->gain_blue == *(SANE_Word *) val)
	    {
	      return SANE_STATUS_GOOD;
	    }
	  scanner->gain_blue = (*(SANE_Word *) val);
	  calculateDerivedValues (scanner);
	  *info |= SANE_INFO_RELOAD_PARAMS;
	  return SANE_STATUS_GOOD;

	case OPT_TL_X:
	  if (scanner->request_pixel.left == *(SANE_Word *) val)
	    return SANE_STATUS_GOOD;
	  scanner->request_pixel.left = *(SANE_Word *) val;
	  calculateDerivedValues (scanner);
	  *info |= SANE_INFO_RELOAD_PARAMS;
	  return SANE_STATUS_GOOD;

	case OPT_TL_Y:
	  if (scanner->request_pixel.top == *(SANE_Word *) val)
	    return SANE_STATUS_GOOD;
	  scanner->request_pixel.top = *(SANE_Word *) val;
	  calculateDerivedValues (scanner);
	  *info |= SANE_INFO_RELOAD_PARAMS;
	  return SANE_STATUS_GOOD;

	case OPT_BR_X:
	  if (scanner->request_pixel.right == *(SANE_Word *) val)
	      return SANE_STATUS_GOOD;
	  scanner->request_pixel.right = *(SANE_Word *) val;
	  calculateDerivedValues (scanner);
	  *info |= SANE_INFO_RELOAD_PARAMS;
	  return SANE_STATUS_GOOD;

	case OPT_BR_Y:
	  if (scanner->request_pixel.bottom == *(SANE_Word *) val)
	      return SANE_STATUS_GOOD;
	  scanner->request_pixel.bottom = *(SANE_Word *) val;
	  calculateDerivedValues (scanner);
	  *info |= SANE_INFO_RELOAD_PARAMS;
	  return SANE_STATUS_GOOD;

	case OPT_MODE:
	  for (i = 0; scan_mode_list[i]; ++i)
	    {
	      if (!strcmp ((SANE_Char const *) val, scan_mode_list[i]))
		{
		  DBG (10, "Setting scan mode to %s (request: %s)\n",
		       scan_mode_list[i], (SANE_Char const *) val);
		  scanner->mode = i;
		  return SANE_STATUS_GOOD;
		}
	    }
	  /* Impossible */
	  return SANE_STATUS_INVAL;
	}			/* switch */
    }				/* else */
  return SANE_STATUS_INVAL;
}

/**
 * Called by SANE when a page acquisition operation is to be started.
 *
 */
SANE_Status
sane_start (SANE_Handle handle)
{
  struct dcm300_data *scanner = handle;
  int defaultFds[2];
  int ret;

  DBG (10, "sane_start\n");

  if (scanner->sfd < 0)
    {
      /* first call */
      DBG (10, "sane_start opening USB device\n");
      if (sanei_usb_open (scanner->sane.name, &(scanner->sfd)) !=
	  SANE_STATUS_GOOD)
	{
	  DBG (MSG_ERR,
	       "sane_start: open of %s failed:\n", scanner->sane.name);
	  return SANE_STATUS_INVAL;
	}
    }

  calculateDerivedValues (scanner);

  DBG (10, "\tbytes per line = %d\n", scanner->bytes_per_scan_line);
  DBG (10, "\tpixels_per_line = %d\n", scanner->scan_width_pixels);
  DBG (10, "\tlines = %d\n", scanner->scan_height_pixels);


  /* create a pipe, fds[0]=read-fd, fds[1]=write-fd */
  if (pipe (defaultFds) < 0)
    {
      DBG (MSG_ERR, "ERROR: could not create pipe\n");
      do_cancel (scanner);
      return SANE_STATUS_IO_ERROR;
    }

  scanner->pipe_r = defaultFds[0];
  scanner->pipe_w = defaultFds[1];

  ret = SANE_STATUS_GOOD;

  scanner->reader_pid = sanei_thread_begin (reader_process, scanner);
  time (&scanner->last_scan);

  if (scanner->reader_pid == -1)
    {
      DBG (MSG_ERR, "cannot fork reader process.\n");
      DBG (MSG_ERR, "%s", strerror (errno));
      ret = SANE_STATUS_IO_ERROR;
    }

  if (sanei_thread_is_forked ())
    {
      close (scanner->pipe_w);
    }

  if (ret == SANE_STATUS_GOOD)
    {
      DBG (10, "sane_start: ok\n");
    }

  return ret;
}


/**
 * Called by SANE to retrieve information about the type of data
 * that the current scan will return.
 *
 * From the SANE spec:
 * This function is used to obtain the current scan parameters. The
 * returned parameters are guaranteed to be accurate between the time
 * a scan has been started (sane_start() has been called) and the
 * completion of that request. Outside of that window, the returned
 * values are best-effort estimates of what the parameters will be
 * when sane_start() gets invoked.
 * 
 * Calling this function before a scan has actually started allows,
 * for example, to get an estimate of how big the scanned image will
 * be. The parameters passed to this function are the handle h of the
 * device for which the parameters should be obtained and a pointer p
 * to a parameter structure.
 */
SANE_Status
sane_get_parameters (SANE_Handle handle, SANE_Parameters * params)
{
  struct dcm300_data *scanner = (struct dcm300_data *) handle;


  DBG (10, "sane_get_parameters\n");

  calculateDerivedValues (scanner);

  params->format =
    (scanner->mode == HP3500_COLOR_SCAN) ? SANE_FRAME_RGB : SANE_FRAME_GRAY;
  params->depth = 8;

  params->pixels_per_line = scanner->scan_width_pixels;
  params->lines = scanner->scan_height_pixels;

  params->bytes_per_line = scanner->bytes_per_scan_line;

  params->last_frame = 1;
  DBG (10, "\tdepth %d\n", params->depth);
  DBG (10, "\tlines %d\n", params->lines);
  DBG (10, "\tpixels_per_line %d\n", params->pixels_per_line);
  DBG (10, "\tbytes_per_line %d\n", params->bytes_per_line);
  return SANE_STATUS_GOOD;
}


/**
 * Called by SANE to read data.
 * 
 * In this implementation, sane_read does nothing much besides reading
 * data from a pipe and handing it back. On the other end of the pipe
 * there's the reader process which gets data from the scanner and
 * stuffs it into the pipe.
 * 
 * From the SANE spec:
 * This function is used to read image data from the device
 * represented by handle h.  Argument buf is a pointer to a memory
 * area that is at least maxlen bytes long.  The number of bytes
 * returned is stored in *len. A backend must set this to zero when
 * the call fails (i.e., when a status other than SANE_STATUS_GOOD is
 * returned).
 * 
 * When the call succeeds, the number of bytes returned can be
 * anywhere in the range from 0 to maxlen bytes.
 */
SANE_Status
sane_read (SANE_Handle handle, SANE_Byte * buf,
	   SANE_Int max_len, SANE_Int * len)
{
  struct dcm300_data *scanner = (struct dcm300_data *) handle;
  ssize_t nread;
  int source = scanner->pipe_r;

  *len = 0;

  nread = read (source, buf, max_len);
  DBG (30, "sane_read: read %ld bytes of %ld\n",
       (long) nread, (long) max_len);

  if (nread < 0)
    {
      if (errno == EAGAIN)
	{
	  return SANE_STATUS_GOOD;
	}
      else
	{
	  do_cancel (scanner);
	  return SANE_STATUS_IO_ERROR;
	}
    }

  *len = nread;

  if (nread == 0)
    {
      close (source);
      DBG (10, "sane_read: pipe closed\n");
      return SANE_STATUS_EOF;
    }

  return SANE_STATUS_GOOD;
}				/* sane_read */


/**
 * Cancels a scan. 
 *
 * It has been said on the mailing list that sane_cancel is a bit of a
 * misnomer because it is routinely called to signal the end of a
 * batch - quoting David Mosberger-Tang:
 * 
 * > In other words, the idea is to have sane_start() be called, and
 * > collect as many images as the frontend wants (which could in turn
 * > consist of multiple frames each as indicated by frame-type) and
 * > when the frontend is done, it should call sane_cancel(). 
 * > Sometimes it's better to think of sane_cancel() as "sane_stop()"
 * > but that name would have had some misleading connotations as
 * > well, that's why we stuck with "cancel".
 * 
 * The current consensus regarding duplex and ADF scans seems to be
 * the following call sequence: sane_start; sane_read (repeat until
 * EOF); sane_start; sane_read...  and then call sane_cancel if the
 * batch is at an end. I.e. do not call sane_cancel during the run but
 * as soon as you get a SANE_STATUS_NO_DOCS.
 * 
 * From the SANE spec:
 * This function is used to immediately or as quickly as possible
 * cancel the currently pending operation of the device represented by
 * handle h.  This function can be called at any time (as long as
 * handle h is a valid handle) but usually affects long-running
 * operations only (such as image is acquisition). It is safe to call
 * this function asynchronously (e.g., from within a signal handler).
 * It is important to note that completion of this operaton does not
 * imply that the currently pending operation has been cancelled. It
 * only guarantees that cancellation has been initiated. Cancellation
 * completes only when the cancelled call returns (typically with a
 * status value of SANE_STATUS_CANCELLED).  Since the SANE API does
 * not require any other operations to be re-entrant, this implies
 * that a frontend must not call any other operation until the
 * cancelled operation has returned.
 */
void
sane_cancel (SANE_Handle h)
{
  DBG (10, "sane_cancel\n");
  do_cancel ((struct dcm300_data *) h);
}


/**
 * Ends use of the scanner.
 * 
 * From the SANE spec:
 * This function terminates the association between the device handle
 * passed in argument h and the device it represents. If the device is
 * presently active, a call to sane_cancel() is performed first. After
 * this function returns, handle h must not be used anymore.
 */
void
sane_close (SANE_Handle handle)
{
  DBG (10, "sane_close\n");
  do_reset (handle);
  do_cancel (handle);
}


/**
 * Terminates the backend.
 * 
 * From the SANE spec:
 * This function must be called to terminate use of a backend. The
 * function will first close all device handles that still might be
 * open (it is recommended to close device handles explicitly through
 * a call to sane_clo-se(), but backends are required to release all
 * resources upon a call to this function). After this function
 * returns, no function other than sane_init() may be called
 * (regardless of the status value returned by sane_exit(). Neglecting
 * to call this function may result in some resources not being
 * released properly.
 */
void
sane_exit (void)
{
  struct dcm300_data *dev, *next;

  DBG (10, "sane_exit\n");

  for (dev = first_dev; dev; dev = next)
    {
      next = dev->next;
      free (dev->devicename);
      free (dev);
    }

  if (devlist)
    free (devlist);
}

/*
 * The scanning code
 */

static SANE_Status
attachScanner (const char *devicename)
{
  struct dcm300_data *dev;

  DBG (15, "attach_scanner: %s\n", devicename);

  for (dev = first_dev; dev; dev = dev->next)
    {
      if (strcmp (dev->sane.name, devicename) == 0)
	{
	  DBG (5, "attach_scanner: scanner already attached (is ok)!\n");
	  return SANE_STATUS_GOOD;
	}
    }


  if (NULL == (dev = malloc (sizeof (*dev))))
    return SANE_STATUS_NO_MEM;
  memset (dev, 0, sizeof (*dev));

  dev->devicename = strdup (devicename);
  dev->sfd = -1;
  dev->last_scan = 0;
  dev->reader_pid = 0;
  dev->pipe_r = dev->pipe_w = -1;

  dev->sane.name = dev->devicename;
  dev->sane.vendor = "ScopeTek";
  dev->sane.model = "DCM300";
  dev->sane.type = "still camera";

  ++num_devices;
  *new_dev = dev;

  DBG (15, "attach_scanner: done\n");

  return SANE_STATUS_GOOD;
}

static SANE_Status
init_options (struct dcm300_data *scanner)
{
  int i;
  SANE_Option_Descriptor *opt;

  memset (scanner->opt, 0, sizeof (scanner->opt));

  for (i = 0; i < NUM_OPTIONS; ++i)
    {
      scanner->opt[i].name = "filler";
      scanner->opt[i].size = sizeof (SANE_Word);
      scanner->opt[i].cap = SANE_CAP_INACTIVE;
    }

  opt = scanner->opt + OPT_NUM_OPTS;
  opt->title = SANE_TITLE_NUM_OPTIONS;
  opt->desc = SANE_DESC_NUM_OPTIONS;
  opt->cap = SANE_CAP_SOFT_DETECT;
#if 0
  opt = scanner->opt + OPT_RESOLUTION;
  opt->name = SANE_NAME_SCAN_RESOLUTION;
  opt->title = SANE_TITLE_SCAN_RESOLUTION;
  opt->desc = SANE_DESC_SCAN_RESOLUTION;
  opt->type = SANE_TYPE_INT;
  opt->constraint_type = SANE_CONSTRAINT_WORD_LIST;
  opt->constraint.word_list = res_list;
  opt->unit = SANE_UNIT_PIXEL;
  opt->cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
#endif
  opt = scanner->opt + OPT_EXPOSURE;
  opt->name = SANE_NAME_BRIGHTNESS;
  opt->title = SANE_I18N ("Exposure");
  opt->desc = SANE_I18N ("Exposure value");
  opt->type = SANE_TYPE_INT;
  opt->unit = SANE_UNIT_NONE;
  opt->constraint_type = SANE_CONSTRAINT_RANGE;
  opt->constraint.range = &range_exp;
  opt->cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;

  opt = scanner->opt + OPT_GAIN_RED;
  opt->name = SANE_I18N ("red");
  opt->title = SANE_I18N ("Red Gain");
  opt->desc = SANE_I18N ("Red Gain");
  opt->type = SANE_TYPE_INT;
  opt->unit = SANE_UNIT_NONE;
  opt->constraint_type = SANE_CONSTRAINT_RANGE;
  opt->constraint.range = &range_gain_red;
  opt->cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;

  opt = scanner->opt + OPT_GAIN_GREEN;
  opt->name = SANE_I18N ("green");
  opt->title = SANE_I18N ("Green Gain");
  opt->desc = SANE_I18N ("Green Gain");
  opt->type = SANE_TYPE_INT;
  opt->unit = SANE_UNIT_NONE;
  opt->constraint_type = SANE_CONSTRAINT_RANGE;
  opt->constraint.range = &range_gain_green;
  opt->cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;

  opt = scanner->opt + OPT_GAIN_BLUE;
  opt->name = SANE_I18N ("blue");
  opt->title = SANE_I18N ("Blue Gain");
  opt->desc = SANE_I18N ("Blue Gain");
  opt->type = SANE_TYPE_INT;
  opt->unit = SANE_UNIT_NONE;
  opt->constraint_type = SANE_CONSTRAINT_RANGE;
  opt->constraint.range = &range_gain_blue;
  opt->cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;


  opt = scanner->opt + OPT_GEOMETRY_GROUP;
  opt->title = SANE_I18N ("Geometry");
  opt->desc = SANE_I18N ("Geometry Group");
  opt->type = SANE_TYPE_GROUP;
  opt->constraint_type = SANE_CONSTRAINT_NONE;

  opt = scanner->opt + OPT_TL_X;
  opt->name = SANE_NAME_SCAN_TL_X;
  opt->title = SANE_TITLE_SCAN_TL_X;
  opt->desc = SANE_DESC_SCAN_TL_X;
  opt->type = SANE_TYPE_INT;
  opt->unit = SANE_UNIT_PIXEL;
  opt->constraint_type = SANE_CONSTRAINT_RANGE;
  opt->constraint.range = &range_x;
  opt->cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;

  opt = scanner->opt + OPT_TL_Y;
  opt->name = SANE_NAME_SCAN_TL_Y;
  opt->title = SANE_TITLE_SCAN_TL_Y;
  opt->desc = SANE_DESC_SCAN_TL_Y;
  opt->type = SANE_TYPE_INT;
  opt->unit = SANE_UNIT_PIXEL;
  opt->constraint_type = SANE_CONSTRAINT_RANGE;
  opt->constraint.range = &range_y;
  opt->cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;

  opt = scanner->opt + OPT_BR_X;
  opt->name = SANE_NAME_SCAN_BR_X;
  opt->title = SANE_TITLE_SCAN_BR_X;
  opt->desc = SANE_DESC_SCAN_BR_X;
  opt->type = SANE_TYPE_INT;
  opt->unit = SANE_UNIT_PIXEL;
  opt->constraint_type = SANE_CONSTRAINT_RANGE;
  opt->constraint.range = &range_x;
  opt->cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;

  opt = scanner->opt + OPT_BR_Y;
  opt->name = SANE_NAME_SCAN_BR_Y;
  opt->title = SANE_TITLE_SCAN_BR_Y;
  opt->desc = SANE_DESC_SCAN_BR_Y;
  opt->type = SANE_TYPE_INT;
  opt->unit = SANE_UNIT_PIXEL;
  opt->constraint_type = SANE_CONSTRAINT_RANGE;
  opt->constraint.range = &range_y;
  opt->cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;

  if (!scan_mode_list[0])
    {
      scan_mode_list[HP3500_COLOR_SCAN] = SANE_VALUE_SCAN_MODE_COLOR;
      scan_mode_list[1] = 0;
#if 0
      scan_mode_list[HP3500_GRAY_SCAN] = SANE_VALUE_SCAN_MODE_GRAY;
      scan_mode_list[HP3500_LINEART_SCAN] = SANE_VALUE_SCAN_MODE_LINEART;
      scan_mode_list[HP3500_TOTAL_SCANS] = 0;
#endif
    }

  opt = scanner->opt + OPT_MODE_GROUP;
  opt->title = SANE_I18N ("Scan Mode Group");
  opt->desc = SANE_I18N ("Scan Mode Group");
  opt->type = SANE_TYPE_GROUP;
  opt->constraint_type = SANE_CONSTRAINT_NONE;

  opt = scanner->opt + OPT_MODE;
  opt->name = SANE_NAME_SCAN_MODE;
  opt->title = SANE_TITLE_SCAN_MODE;
  opt->desc = SANE_DESC_SCAN_MODE;
  opt->type = SANE_TYPE_STRING;
  opt->constraint_type = SANE_CONSTRAINT_STRING_LIST;
  opt->constraint.string_list = scan_mode_list;
  opt->cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;

  return SANE_STATUS_GOOD;
}

static void
do_reset (struct dcm300_data *scanner)
{
  scanner = scanner;		/* kill warning */
}

static void
do_cancel (struct dcm300_data *scanner)
{
  if (scanner->reader_pid > 0)
    {

      if (sanei_thread_kill (scanner->reader_pid) == 0)
	{
	  int exit_status;

	  sanei_thread_waitpid (scanner->reader_pid, &exit_status);
	}
      scanner->reader_pid = 0;
    }
  if (scanner->pipe_r >= 0)
    {
      close (scanner->pipe_r);
      scanner->pipe_r = -1;
    }
}

static void
calculateDerivedValues (struct dcm300_data *scanner)
{

  DBG (12, "calculateDerivedValues\n");

  /* Convert the SANE_FIXED values for the scan area into 1/1200 inch 
   * scanner units */

  DBG (12, "\tleft margin: %u\n", scanner->request_pixel.left);
  DBG (12, "\ttop margin: %u\n", scanner->request_pixel.top);
  DBG (12, "\tright margin: %u\n", scanner->request_pixel.right);
  DBG (12, "\tbottom margin: %u\n", scanner->request_pixel.bottom);

  /* add +1 because we include first and last pixel 0-1023, 0-767 */
  scanner->scan_width_pixels = scanner->request_pixel.right - scanner->request_pixel.left + 1;
  scanner->scan_height_pixels = scanner->request_pixel.bottom - scanner->request_pixel.top + 1;

  scanner->bytes_per_scan_line = scanner->scan_width_pixels * 3;

  DBG (12, "calculateDerivedValues: ok\n");
}

/* From here on in we have the original code written for the scanner demo */

static int udh;
static int cancelled_scan = 0;
static int do_warmup = 1;

int printhex(char *tag, unsigned char *a, int n)
{
  int i, pos = 0;
  char buffer[1024];
  
  if(n < 0)
  {
    printf("no data\n");
    return 0;
  }
  
  for(i = 0; i < n && pos < 1000; i++, pos += 3)
    sprintf(buffer + pos, "%02x ", a[i]);
  sprintf(buffer + pos, "(%d)\n", n);
  DBG(10, "%s%s\n", tag, buffer);
  
  return 0;
}


static int
dcm300_scan_test  (unsigned x,
	      unsigned y,
	      unsigned w,
	      unsigned h,
	      unsigned exposure,
	      unsigned gain_red,
	      unsigned gain_green,
	      unsigned gain_blue, 
	      dcm300_callback cbfunc, void *param)
{
  unsigned int i, j;
  unsigned char rgbline[1024][3];
  
  for(i = 0; i < w; i++)
  {
    rgbline[i][0] = gain_red;
    rgbline[i][1] = gain_green;
    rgbline[i][2] = gain_blue;
  }
  
  for (j = 0; j < h; j++)
    (*cbfunc)(param, 3*w, rgbline);
  
  return 0;
}

/* image get packet for dcm300
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
  unsigned char unknown3a;      /* maybe gamma maybe not, values seen: bf,d6,da,b0,e1 */
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
  unsigned char gain_red, gain_green, gain_blue;
  unsigned char unknown8_0[1];
  unsigned char unknown9;       /* 02 */
  unsigned char unknown9_0[23]; /* 00 ... */
};

/* parameters for taking a snapshot with dcm300
*/
struct dcm300_snapshot {
  unsigned int resolution_x, resolution_y;
  unsigned int offset_x, offset_y;
  unsigned int exposure;
  unsigned char gain_red, gain_green, gain_blue;
};

int dcm300_create_request(struct dcm300_snapshot *s, struct dcm300_request *r)
{
  memset(r, 0, sizeof(*r));

  r->unknown1a = 0x2c;
  r->unknown1b = 0x0e;
  r->unknown1c = 0x01;

  r->unknown2 = 0x20;
  r->unknown3a = 0xbf;
  r->unknown3 = 0x05;
  r->unknown9 = 0x02;
  
  r->resolution_x_lo = (s->resolution_x) % 256;
  r->resolution_x_hi = (s->resolution_x) / 256;
  r->resolution_y_lo = (s->resolution_y) % 256;
  r->resolution_y_hi = (s->resolution_y) / 256;

  r->offset_x_lo = (s->offset_x) % 256;
  r->offset_x_hi = (s->offset_x) / 256;

  r->offset_y_lo = (s->offset_y) % 256;
  r->offset_y_hi = (s->offset_y) / 256;

  r->exposure_lo = (s->exposure + 20) % 256;
  r->exposure_hi = (s->exposure + 20) / 256;

  r->gain_red   = s->gain_red;
  r->gain_green = s->gain_green;
  r->gain_blue  = s->gain_blue;


  return 0;
}

#define BAYER_CIRCULAR 32768

/* returns last bayer_start that was left unprocessed */
int bayer_circular_downscale(
              unsigned char *bayer_array, 
              int bayer_width, /* number of columns in a row of the bayer array */
              int *bayer_start, /* modified to the next start */
              int bayer_stop, /* end of read bytes */
              unsigned char *rgb_array,
              int *rgb_len)
{
  int  i, j, irgb;
  int bayer_last;
  
  /* even number of complete bayer lines */  
  bayer_last = *bayer_start + (bayer_stop - *bayer_start) - ((bayer_stop - *bayer_start) % (2*bayer_width));
  DBG(30, "bayer: start %08x stop %08x last %08x\n", *bayer_start, bayer_stop, bayer_last);
  irgb = 0;
  for(i = *bayer_start; i < bayer_last; i += 2*bayer_width )
  {
    for(j = i; j < i + bayer_width; j += 2)
    {
      rgb_array[irgb++] = bayer_array[ (j) % BAYER_CIRCULAR];
      rgb_array[irgb++] = (bayer_array[ (j + 1) % BAYER_CIRCULAR] + bayer_array[ (j + bayer_width) % BAYER_CIRCULAR ])/2;
      rgb_array[irgb++] = bayer_array[ (j + bayer_width + 1) % BAYER_CIRCULAR];
      if(irgb > 3*8192)
      {
        DBG(1, "cat't fit to rgb - need bayer force exit\n");
        goto force_exit;
      }
    }
  }
  
  force_exit:;
  
  *bayer_start = i;
  *rgb_len = irgb;
  return i;
}
              

static int
dcm300_scan  (unsigned x1,
	      unsigned y1,
	      unsigned w1,
	      unsigned h1,
	      unsigned exposure,
	      unsigned gain_red,
	      unsigned gain_green,
	      unsigned gain_blue, 
	      dcm300_callback cbfunc, void *param)
{
  unsigned int i, j;
  int image_len;
  size_t request_len; /* bytes in reply image and request */
  struct dcm300_request r[1];
  struct dcm300_snapshot s[1];
  SANE_Status result;
  int bulk_header_len = 64; /* first bulk packet after request */
  int bulk_content_len = 16384; /* bulk reply chunk size */
  int bulk_footer_len = 256;  /* trailing bytes. Not necessary size of the last bulk */
                                 /* due to anomaly e.g. at 800x600 , last bulk may be e.g. 512 bytes, 
                                 ** merged 2 things in athe last bulk:
                                 ** 1. last 256 bytes of the image and
                                 ** 2. the trailing 256 bytes */
  int bytes_read = 0; /* bytes of the image read */
  size_t bulk_len; /* temporary value of bulk number of bytes read */
  int bulk_want;
  int bayer_from = 0;
  unsigned char replybuf[BAYER_CIRCULAR];
  unsigned char rgb[3*BAYER_CIRCULAR/2];
  /* speed issue - need to buffer image before writing */
  unsigned char rgbimage[3*1024*768];
  int rgb_len, rgb_left, rgb_num, rgb_done;
  int x = 0, y = 0, w = 1024, h = 768; /* always use this resolution */


  /*
  ** as we will do the simple demosaic rggb->rgb 
  ** resolution will actually be half of other bayer
  ** demosaicing methods
  **
  ** Many sophisticated bayer demosaicings exist. they produce
  ** 3-byte RGB value from each single byte in bayer array,
  ** but IMHO they can't
  ** add much more information to the image but they 
  ** can add blur and may have artefacts
  **
  ** bayer array is RGGB 2048x1536 and this code will
  ** transform it on-the-fly into RGB 1024x768
  **
  **      G=(G1+G2)/2 (mean green)
  **
  ** R G R G
  ** G B G B  -->  RGB RGB
  ** R G R G  -->  RGB RGB
  ** G B G B
  */
  
  s->resolution_x = 0xfff0 & (2*w); 
  s->resolution_y = 0xfff0 & (2*h);
  s->offset_x     = 2*x;
  s->offset_y     = 2*y;
  s->exposure     = exposure;
  s->gain_red     = gain_red;
  s->gain_green   = gain_green;
  s->gain_blue    = gain_blue;
  
  /* read data and demosaic them on-the fly.
  ** each bulk read is 16K long. Use 32K circular buffer
  ** to place alternating reads. Track the position
  ** using 32K modulo and process data as if they 
  ** were linear
  */
  
  /* due to a bug or something we don't know,
  ** image has to be acquired twice otherwise
  ** usb protocol blocks at the next attempt to scan
  */
  for(j = 0; j < 2; j++)
  {
#if 1

    /* make request */
    dcm300_create_request(s, r);

    request_len = sizeof(r);
    image_len = s->resolution_x * s->resolution_y;
    DBG(10, "request created (%d bytes). expecting 64+0x%08X+256 bytes\n", (int) request_len, image_len);
    printhex("request:", (unsigned char *)r, request_len);

    result = sanei_usb_write_bulk (udh, (unsigned char *)r, &request_len);

    if (result != SANE_STATUS_GOOD)
    {
      DBG(1, "request write failed\n");
      goto exitscan;
    }

    bulk_len = bulk_header_len;
    result = sanei_usb_read_bulk (udh, replybuf, &bulk_len);
    if (result != SANE_STATUS_GOOD)
    {
      DBG(1, "header bulk read error\n");
      goto exitscan;
    }
    DBG(20, "header bulk want=%d got=%d\n", bulk_header_len, (int)bulk_len);
    bayer_from = 0;
    rgb_left = 3*w*h;
    rgb_done = 0;
    bulk_want = bulk_len = 0;
    for(bytes_read = 0; bytes_read < image_len && bulk_want == (int)bulk_len; bytes_read += bulk_len)
    {
      bulk_want = image_len - bytes_read > bulk_content_len ? bulk_content_len : image_len - bytes_read;
      bulk_len = (bytes_read % BAYER_CIRCULAR) + bulk_want <= BAYER_CIRCULAR ?  bulk_want : BAYER_CIRCULAR - (bytes_read % BAYER_CIRCULAR);
      result = sanei_usb_read_bulk (udh, replybuf + (bytes_read % BAYER_CIRCULAR), &bulk_len);
      if (result != SANE_STATUS_GOOD)
      {
        DBG(1, "content bulk read error\n");
        goto exitscan1;
      }
      bayer_circular_downscale(replybuf, s->resolution_x, &bayer_from, bytes_read + bulk_len, rgb, &rgb_len);
      rgb_num = 0;
      if(j == 1 && rgb_left > 0)
      {
        rgb_num = rgb_left > rgb_len ? rgb_len : rgb_left;
        memcpy(rgbimage + rgb_done, rgb, rgb_num);
        rgb_done += rgb_num;
        rgb_left -= rgb_num;
      }
      DBG(20, "content bulk at %08X want:%d got:%d rgb:%d\n", bytes_read, bulk_want, (int)bulk_len, rgb_num);
    }
    exitscan1:;
    DBG(20, "content bulks complete\n");
    bulk_want = image_len - bytes_read + bulk_footer_len> bulk_content_len ? bulk_content_len : image_len - bytes_read + bulk_footer_len;
    bulk_len = (bytes_read % BAYER_CIRCULAR) + bulk_want <= BAYER_CIRCULAR ?  bulk_want : BAYER_CIRCULAR - (bytes_read % BAYER_CIRCULAR);
    result = sanei_usb_read_bulk (udh, replybuf + (bytes_read % BAYER_CIRCULAR), &bulk_len);
    if (result != SANE_STATUS_GOOD)
    {
      DBG(1, "footer bulk read error\n");
      goto exitscan;
    }
    bayer_circular_downscale(replybuf, s->resolution_x, &bayer_from, bytes_read + bulk_len, rgb, &rgb_len);     
    rgb_num = 0;
    if(j == 1 && rgb_left > 0)
    {
      rgb_num = rgb_left > rgb_len ? rgb_len : rgb_left;
      memcpy(rgbimage + rgb_done, rgb, rgb_num);
      rgb_done += rgb_num;
      rgb_left -= rgb_num;
    }
    DBG(20, "footer bulk at %08X want:%d got:%d rgb:%d\n", bytes_read, bulk_want, (int)bulk_len, rgb_num);
    if(rgb_done == 3*w*h)
    {
      DBG(10, "write %08X bytes image\n", rgb_done);
      /* (*cbfunc)(param, rgb_done, rgbimage); */
      for(i = 0; i < h1; i++)
        (*cbfunc)(param, 3*w1, rgbimage + 3*(w*(i + y1) + x1) );
    }
    else
    {
      DBG(1, "image size mismatch: want:%08x got:%08x\n", 3*w*h, rgb_done);
    }
#endif
  
  }
  exitscan:;

  return 0;
}


static int
writefunc (struct dcm300_write_info *winfo, int bytes, char *data)
{
  static int warned = 0;

  if (bytes > winfo->bytesleft)
    {
      if (!warned)
	{
	  warned = 1;
	  DBG (1, "Overflow protection triggered\n");
#if 0
	  rt_stop_moving ();
#endif
	}
      bytes = winfo->bytesleft;
      if (!bytes)
	return 0;
    }
  winfo->bytesleft -= bytes;
  return write (winfo->scanner->pipe_w, data, bytes) == bytes;
}

static void
sigtermHandler (int signal)
{
  signal = signal;		/* get rid of compiler warning */
  cancelled_scan = 1;
}

static int
reader_process (void *pv)
{
  struct dcm300_data *scanner = pv;
  time_t t;
  sigset_t ignore_set;
  sigset_t sigterm_set;
  struct SIGACTION act;
  struct dcm300_write_info winfo;

  if (sanei_thread_is_forked ())
    {
      close (scanner->pipe_r);
    }

  sigfillset (&ignore_set);
  sigdelset (&ignore_set, SIGTERM);
#if defined (__APPLE__) && defined (__MACH__)
  sigdelset (&ignore_set, SIGUSR2);
#endif
  sigprocmask (SIG_SETMASK, &ignore_set, 0);

  sigemptyset (&sigterm_set);
  sigaddset (&sigterm_set, SIGTERM);

  memset (&act, 0, sizeof (act));
#ifdef _POSIX_SOURCE
  act.sa_handler = sigtermHandler;
#endif
  sigaction (SIGTERM, &act, 0);


  /* Warm up the lamp again if our last scan ended more than 5 minutes ago. */
  time (&t);
  do_warmup = (t - scanner->last_scan) > 300;

#if 0
  if (getenv ("HP3500_NOWARMUP") && atoi (getenv ("HP3500_NOWARMUP")) > 0)
    do_warmup = 0;
#endif

  udh = scanner->sfd;

  cancelled_scan = 0;

  winfo.scanner = scanner;
  winfo.bytesleft =
    scanner->bytes_per_scan_line * scanner->scan_height_pixels;

  DBG (10, "Scanning at %ddpi, mode=%s\n", scanner->resolution,
       scan_mode_list[scanner->mode]);

  if (dcm300_scan
      (scanner->request_pixel.left,
       scanner->request_pixel.top,
       scanner->scan_width_pixels,
       scanner->scan_height_pixels,
       scanner->exposure, 
       scanner->gain_red, scanner->gain_green, scanner->gain_blue, 
       (dcm300_callback) writefunc,
       &winfo) >= 0)
    exit (SANE_STATUS_GOOD);
  exit (SANE_STATUS_IO_ERROR);
}
