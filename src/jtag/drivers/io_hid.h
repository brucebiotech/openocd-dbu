/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef OPENOCD_JTAG_DRIVERS_DBU_DAP_H
#define OPENOCD_JTAG_DRIVERS_DBU_DAP_H

#include <stdint.h>

struct dbu_backend;
struct dbu_backend_data;

struct pending_transfer_result {
	uint8_t cmd;
	uint32_t data;
	void *buffer;
};

/* Up to MIN(packet_count, MAX_PENDING_REQUESTS) requests may be issued
 * until the first response arrives */
#define MAX_PENDING_REQUESTS 4


struct pending_request_block {
	struct pending_transfer_result *transfers;
	unsigned int transfer_count;
	uint8_t command;
};


struct dbu {
	struct dbu_backend_data *bdata;
	const struct dbu_backend *backend;
	unsigned int packet_size;
	unsigned int packet_usable_size;
	unsigned int packet_buffer_size;
	uint8_t *packet_buffer;
	uint8_t *command;
	uint8_t *response;

	/* DP/AP register r/w operation counters used for checking the packet size
	 * that would result from the queue run */
	unsigned int write_count;
	unsigned int read_count;

	/* We can use DAP_TransferBlock only if all SWD operations in the packet
	 * are either all writes or all reads and use the same DP/AP register.
	 * The following variables keep track of it */
	uint8_t common_swd_cmd;
	bool swd_cmds_differ;

	/* Pending requests are organized as a FIFO - circular buffer */
	struct pending_request_block pending_fifo[MAX_PENDING_REQUESTS];
	unsigned int packet_count;
	unsigned int pending_fifo_put_idx, pending_fifo_get_idx;
	unsigned int pending_fifo_block_count;

	uint16_t caps;
	bool quirk_mode;	/* enable expensive workarounds */

	uint32_t swo_buf_sz;
	bool trace_enabled;
};

struct dbu_backend {
	const char *name;
	int (*open)(struct dbu *dap, uint16_t vids[], uint16_t pids[], const char *serial);
	void (*close)(struct dbu *dap);
	int (*read)(struct dbu *dap, int transfer_timeout_ms,
			    struct timeval *wait_timeout);
	int (*write)(struct dbu *dap, int len, int timeout_ms);
	int (*packet_buffer_alloc)(struct dbu *dap, unsigned int pkt_sz);
	void (*packet_buffer_free)(struct dbu *dap);
	void (*cancel_all)(struct dbu *dap);
};

extern const struct dbu_backend dbu_hid_backend;
extern const struct dbu_backend dbu_usb_backend;
extern const struct command_registration dbu_usb_subcommand_handlers[];

#define REPORT_ID_SIZE   1

#endif
