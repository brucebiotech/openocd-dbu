// SPDX-License-Identifier: GPL-2.0-or-later

/***************************************************************************
 *   Copyright (C) 2018 by Mickaël Thomas                                  *
 *   mickael9@gmail.com                                                    *
 *                                                                         *
 *   Copyright (C) 2016 by Maksym Hilliaka                                 *
 *   oter@frozen-team.com                                                  *
 *                                                                         *
 *   Copyright (C) 2016 by Phillip Pearson                                 *
 *   pp@myelin.co.nz                                                       *
 *                                                                         *
 *   Copyright (C) 2014 by Paul Fertser                                    *
 *   fercerpav@gmail.com                                                   *
 *                                                                         *
 *   Copyright (C) 2013 by mike brown                                      *
 *   mike@theshedworks.org.uk                                              *
 *                                                                         *
 *   Copyright (C) 2013 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk                                                  *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <hidapi.h>
#include <helper/log.h>

#include "io_hid.h"

struct dbu_backend_data {
	hid_device *dev_handle;
};

struct dbu_report_size {
	unsigned short vid;
	unsigned short pid;
	unsigned int report_size;
};

static const struct dbu_report_size report_size_quirks[] = {
	/* Third gen Atmel tools use a report size of 512 */
    /* This list of PIDs comes from toolinfo.py in Microchip's pyedbglib. */
	// Atmel JTAG-ICE 3
	{ .vid = 0x03eb, .pid = 0x2140, .report_size = 512 },
	// Atmel-ICE
	{ .vid = 0x03eb, .pid = 0x2141, .report_size = 512 },
	// Atmel Power Debugger
	{ .vid = 0x03eb, .pid = 0x2144, .report_size = 512 },
	// EDBG (found on Xplained Pro boards)
	{ .vid = 0x03eb, .pid = 0x2111, .report_size = 512 },
	// Zero (???)
	{ .vid = 0x03eb, .pid = 0x2157, .report_size = 512 },
	// EDBG with Mass Storage (found on Xplained Pro boards)
	{ .vid = 0x03eb, .pid = 0x2169, .report_size = 512 },
	// Commercially available EDBG (for third-party use)
	{ .vid = 0x03eb, .pid = 0x216a, .report_size = 512 },
	// Kraken (???)
	{ .vid = 0x03eb, .pid = 0x2170, .report_size = 512 },

	{ .vid = 0, .pid = 0, .report_size = 0 }
};


static void dbu_hid_close(struct dbu *dap);
static int dbu_hid_alloc(struct dbu *dap, unsigned int pkt_sz);
static void dbu_hid_free(struct dbu *dap);

static int dbu_hid_open(struct dbu *dap, uint16_t vids[], uint16_t pids[], const char *serial)
{
	hid_device *dev = NULL;
	int i;
	struct hid_device_info *devs, *cur_dev;
	unsigned short target_vid, target_pid;

	target_vid = 0;
	target_pid = 0;

	if (hid_init() != 0) {
		LOG_ERROR("unable to open HIDAPI");
		return ERROR_FAIL;
	}

	/*
	 * The CMSIS-DAP specification stipulates:
	 * "The Product String must contain "CMSIS-DAP" somewhere in the string. This is used by the
	 * debuggers to identify a CMSIS-DAP compliant Debug Unit that is connected to a host computer."
	 */
	devs = hid_enumerate(0x0, 0x0);
	cur_dev = devs;
	while (cur_dev) {
		bool found = false;

		if (vids[0] == 0) {
			if (!cur_dev->product_string) {
				LOG_DEBUG("Cannot read product string of device 0x%x:0x%x",
					  cur_dev->vendor_id, cur_dev->product_id);
			} else if (wcsstr(cur_dev->product_string, L"CMSIS-DAP")) {
				/* if the user hasn't specified VID:PID *and*
				 * product string contains "CMSIS-DAP", pick it
				 */
				found = true;
			}
		} else {
			/* otherwise, exhaustively compare against all VID:PID in list */
			for (i = 0; vids[i] || pids[i]; i++) {
				if ((vids[i] == cur_dev->vendor_id) && (pids[i] == cur_dev->product_id))
					found = true;
			}
		}

		/* LPC-LINK2 has cmsis-dap on interface 0 and other HID functions on other interfaces */
		if (cur_dev->vendor_id == 0x1fc9 && cur_dev->product_id == 0x0090 && cur_dev->interface_number != 0)
			found = false;

		if (found) {
			/* check serial number matches if given */
			if (!serial)
				break;

			if (cur_dev->serial_number) {
				size_t len = mbstowcs(NULL, serial, 0) + 1;
				wchar_t *wserial = malloc(len * sizeof(wchar_t));
				if (!wserial) {
					LOG_ERROR("unable to allocate serial number buffer");
					return ERROR_FAIL;
				}
				mbstowcs(wserial, serial, len);

				if (wcscmp(wserial, cur_dev->serial_number) == 0) {
					free(wserial);
					break;
				} else {
					free(wserial);
					wserial = NULL;
				}
			}
		}

		cur_dev = cur_dev->next;
	}

	if (cur_dev) {
		target_vid = cur_dev->vendor_id;
		target_pid = cur_dev->product_id;
	}

	if (target_vid == 0 && target_pid == 0) {
		hid_free_enumeration(devs);
		return ERROR_FAIL;
	}

	dap->bdata = malloc(sizeof(struct dbu_backend_data));
	if (!dap->bdata) {
		LOG_ERROR("unable to allocate memory");
		return ERROR_FAIL;
	}

	dev = hid_open_path(cur_dev->path);
	hid_free_enumeration(devs);

	if (!dev) {
		LOG_ERROR("unable to open CMSIS-DAP device 0x%x:0x%x", target_vid, target_pid);
		return ERROR_FAIL;
	}

	/* allocate default packet buffer, may be changed later.
	 * currently with HIDAPI we have no way of getting the output report length
	 * without this info we cannot communicate with the adapter.
	 * For the moment we have to hard code the packet size */

	unsigned int packet_size = 64;

	/* Check for adapters that are known to have unusual report lengths. */
	for (i = 0; report_size_quirks[i].vid != 0; i++) {
		if (report_size_quirks[i].vid == target_vid &&
		    report_size_quirks[i].pid == target_pid) {
			packet_size = report_size_quirks[i].report_size;
		}
	}
	/* TODO: HID report descriptor should be parsed instead of
	 * hardcoding a match by VID/PID */

	dap->bdata->dev_handle = dev;

	int retval = dbu_hid_alloc(dap, packet_size);
	if (retval != ERROR_OK) {
		dbu_hid_close(dap);
		return ERROR_FAIL;
	}

	dap->command = dap->packet_buffer + REPORT_ID_SIZE;
	dap->response = dap->packet_buffer;
	return ERROR_OK;
}

static void dbu_hid_close(struct dbu *dap)
{
	hid_close(dap->bdata->dev_handle);
	hid_exit();
	free(dap->bdata);
	dap->bdata = NULL;
	dbu_hid_free(dap);
}

static int dbu_hid_read(struct dbu *dap, int transfer_timeout_ms,
							  struct timeval *wait_timeout)
{
	int timeout_ms;
	if (wait_timeout)
		timeout_ms = wait_timeout->tv_usec / 1000 + wait_timeout->tv_sec * 1000;
	else
		timeout_ms = transfer_timeout_ms;

	int retval = hid_read_timeout(dap->bdata->dev_handle,
								  dap->packet_buffer, dap->packet_buffer_size,
								  timeout_ms);
	if (retval == 0) {
		return ERROR_TIMEOUT_REACHED;
	} else if (retval == -1) {
		LOG_ERROR("error reading data: %ls", hid_error(dap->bdata->dev_handle));
		return ERROR_FAIL;
	}

	return retval;
}

static int dbu_hid_write(struct dbu *dap, int txlen, int timeout_ms)
{
	(void) timeout_ms;

	dap->packet_buffer[0] = 0; /* HID report number */

	/* Pad the rest of the TX buffer with 0's */
	memset(dap->command + txlen, 0, dap->packet_size - txlen);

	/* write data to device */
	int retval = hid_write(dap->bdata->dev_handle, dap->packet_buffer, dap->packet_buffer_size);
	if (retval == -1) {
		LOG_ERROR("error writing data: %ls", hid_error(dap->bdata->dev_handle));
		return ERROR_FAIL;
	}

	return retval;
}

static int dbu_hid_alloc(struct dbu *dap, unsigned int pkt_sz)
{
	unsigned int packet_buffer_size = pkt_sz + REPORT_ID_SIZE;
	uint8_t *buf = malloc(packet_buffer_size);
	if (!buf) {
		LOG_ERROR("unable to allocate CMSIS-DAP packet buffer");
		return ERROR_FAIL;
	}

	dap->packet_buffer = buf;
	dap->packet_size = pkt_sz;
	dap->packet_usable_size = pkt_sz;
	dap->packet_buffer_size = packet_buffer_size;

	dap->command = dap->packet_buffer + REPORT_ID_SIZE;
	dap->response = dap->packet_buffer;

	return ERROR_OK;
}

static void dbu_hid_free(struct dbu *dap)
{
	free(dap->packet_buffer);
	dap->packet_buffer = NULL;
}

static void dbu_hid_cancel_all(struct dbu *dap)
{
}

const struct dbu_backend dbu_hid_backend = {
	.name = "dbu",
	.open = dbu_hid_open,
	.close = dbu_hid_close,
	.read = dbu_hid_read,
	.write = dbu_hid_write,
	.packet_buffer_alloc = dbu_hid_alloc,
	.packet_buffer_free = dbu_hid_free,
	.cancel_all = dbu_hid_cancel_all,
};
