/*
 * Vendor-specific bulk-IN class for high-throughput device->host transfers.
 *
 * Why this exists: Zephyr's CDC ACM driver caps TX at ~160 KB/s because
 * cdc_acm_tx_fifo_handler keeps at most ONE USB transfer in flight (the
 * CDC_ACM_TX_FIFO_BUSY guard). The USB Full-Speed hardware can do ~700 KB/s
 * on a bulk endpoint; the limit is in the class driver. By writing our own
 * thin class with multiple in-flight transfers we bypass that ceiling.
 *
 * Design:
 *   - Single bulk-IN endpoint (device -> host, MPS 64 B for FS, 512 B for HS).
 *   - Pool of N=PWR_BULK_BUF_COUNT pre-allocated net_bufs.
 *   - Producer (cmd_download via pwr_bulk_send()) blocks on a counted
 *     semaphore until a buf is available, copies bytes in, calls
 *     usbd_ep_enqueue, advances. Many transfers can be in flight at once.
 *   - request() handler runs on TX completion, frees the buf, releases the
 *     semaphore so the producer can submit the next chunk.
 *
 * No OUT endpoint: download is device->host only. Could add one later for
 * acks/control if needed.
 */

#include <zephyr/kernel.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/drivers/usb/udc.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>

#include "usbd_pwr_bulk.h"

LOG_MODULE_REGISTER(usbd_pwr_bulk, LOG_LEVEL_INF);

#define PWR_BULK_BUF_COUNT     8     /* concurrent in-flight TX transfers */
#define PWR_BULK_EP_MPS_FS     64
#define PWR_BULK_EP_MPS_HS     512

struct pwr_bulk_desc {
	struct usb_if_descriptor if0;
	struct usb_ep_descriptor if0_in_ep_fs;
	struct usb_ep_descriptor if0_in_ep_hs;
	struct usb_desc_header nil_desc;
};

struct pwr_bulk_data {
	struct pwr_bulk_desc *desc;
	const struct usb_desc_header **fs_desc;
	const struct usb_desc_header **hs_desc;
	atomic_t enabled;
	struct usbd_class_data *c_data;
	struct k_sem free_slots;     /* count of available TX buffer slots */
	struct k_sem all_done;        /* given when free_slots returns to MAX */
};

static struct pwr_bulk_desc pwr_bulk_desc_inst = {
	.if0 = {
		.bLength = sizeof(struct usb_if_descriptor),
		.bDescriptorType = USB_DESC_INTERFACE,
		.bInterfaceNumber = 0,        /* filled in by the USBD core */
		.bAlternateSetting = 0,
		.bNumEndpoints = 1,
		.bInterfaceClass = USB_BCC_VENDOR,
		.bInterfaceSubClass = 0,
		.bInterfaceProtocol = 0,
		.iInterface = 0,
	},
	.if0_in_ep_fs = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = 0x81,    /* address auto-assigned by core */
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(PWR_BULK_EP_MPS_FS),
		.bInterval = 0,
	},
	.if0_in_ep_hs = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = 0x81,
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(PWR_BULK_EP_MPS_HS),
		.bInterval = 0,
	},
	.nil_desc = { 0, 0 },
};

static const struct usb_desc_header *pwr_bulk_fs_desc[] = {
	(struct usb_desc_header *)&pwr_bulk_desc_inst.if0,
	(struct usb_desc_header *)&pwr_bulk_desc_inst.if0_in_ep_fs,
	(struct usb_desc_header *)&pwr_bulk_desc_inst.nil_desc,
};

static const struct usb_desc_header *pwr_bulk_hs_desc[] = {
	(struct usb_desc_header *)&pwr_bulk_desc_inst.if0,
	(struct usb_desc_header *)&pwr_bulk_desc_inst.if0_in_ep_hs,
	(struct usb_desc_header *)&pwr_bulk_desc_inst.nil_desc,
};

static struct pwr_bulk_data pwr_bulk_data_inst = {
	.desc = &pwr_bulk_desc_inst,
	.fs_desc = pwr_bulk_fs_desc,
	.hs_desc = pwr_bulk_hs_desc,
};

static uint8_t pwr_bulk_get_bulk_in(struct usbd_class_data *const c_data)
{
	struct pwr_bulk_data *data = usbd_class_get_private(c_data);
	struct usbd_context *uds_ctx = usbd_class_get_ctx(c_data);

	if (usbd_bus_speed(uds_ctx) == USBD_SPEED_HS) {
		return data->desc->if0_in_ep_hs.bEndpointAddress;
	}
	return data->desc->if0_in_ep_fs.bEndpointAddress;
}

static int pwr_bulk_request(struct usbd_class_data *const c_data,
			    struct net_buf *const buf, const int err)
{
	struct pwr_bulk_data *data = usbd_class_get_private(c_data);
	struct udc_buf_info *bi = (struct udc_buf_info *)net_buf_user_data(buf);

	ARG_UNUSED(err);

	/* This was a TX completion for our bulk-IN endpoint. Release the
	 * buffer back to the UDC pool and signal the producer that a slot
	 * is free again. */
	if (bi->ep == pwr_bulk_get_bulk_in(c_data)) {
		net_buf_unref(buf);
		k_sem_give(&data->free_slots);
		if (k_sem_count_get(&data->free_slots) >= PWR_BULK_BUF_COUNT) {
			k_sem_give(&data->all_done);
		}
	}
	return 0;
}

static void *pwr_bulk_get_desc(struct usbd_class_data *const c_data,
			       const enum usbd_speed speed)
{
	struct pwr_bulk_data *data = usbd_class_get_private(c_data);

	if (USBD_SUPPORTS_HIGH_SPEED && speed == USBD_SPEED_HS) {
		return data->hs_desc;
	}
	return data->fs_desc;
}

static void pwr_bulk_enable(struct usbd_class_data *const c_data)
{
	struct pwr_bulk_data *data = usbd_class_get_private(c_data);

	atomic_set(&data->enabled, 1);
	LOG_INF("pwr-bulk enabled");
}

static void pwr_bulk_disable(struct usbd_class_data *const c_data)
{
	struct pwr_bulk_data *data = usbd_class_get_private(c_data);

	atomic_set(&data->enabled, 0);
	LOG_INF("pwr-bulk disabled");
}

static int pwr_bulk_init(struct usbd_class_data *c_data)
{
	struct pwr_bulk_data *data = usbd_class_get_private(c_data);

	data->c_data = c_data;
	k_sem_init(&data->free_slots, PWR_BULK_BUF_COUNT, PWR_BULK_BUF_COUNT);
	k_sem_init(&data->all_done, 1, 1);   /* starts "done" */
	return 0;
}

static struct usbd_class_api pwr_bulk_api = {
	.request = pwr_bulk_request,
	.get_desc = pwr_bulk_get_desc,
	.enable = pwr_bulk_enable,
	.disable = pwr_bulk_disable,
	.init = pwr_bulk_init,
};

USBD_DEFINE_CLASS(pwr_bulk, &pwr_bulk_api, &pwr_bulk_data_inst, NULL);

/* ---- public API --------------------------------------------------------- */

int pwr_bulk_send(const uint8_t *data, size_t len)
{
	struct pwr_bulk_data *pd = &pwr_bulk_data_inst;

	if (!atomic_get(&pd->enabled) || pd->c_data == NULL) {
		return -ENOTCONN;
	}

	while (len > 0) {
		/* Wait for a free slot in the pool. Blocks the producer when
		 * all PWR_BULK_BUF_COUNT bufs are in flight. */
		(void)k_sem_take(&pd->free_slots, K_FOREVER);
		(void)k_sem_take(&pd->all_done, K_NO_WAIT);   /* not idle any more */

		uint8_t ep = pwr_bulk_get_bulk_in(pd->c_data);
		size_t chunk = len;

		/* Cap chunk at MPS * some packets so each transfer is sized
		 * roughly the same as the UDC pool entry. The UDC pool size
		 * is set via CONFIG_UDC_BUF_POOL_SIZE; transfers larger than
		 * the per-buf size split or fail at usbd_ep_buf_alloc. */
		if (chunk > 1024) {
			chunk = 1024;
		}

		/* Alloc from the shared UDC buf pool. The pool can be smaller
		 * than PWR_BULK_BUF_COUNT * chunk (CONFIG_UDC_BUF_POOL_SIZE
		 * is shared with CDC + MSC). Treat alloc failure as
		 * back-pressure: drop the slot, wait briefly for in-flight
		 * transfers to complete + release pool memory, retry. */
		struct net_buf *buf = usbd_ep_buf_alloc(pd->c_data, ep, chunk);

		if (buf == NULL) {
			k_sem_give(&pd->free_slots);
			k_sleep(K_MSEC(1));
			continue;
		}
		net_buf_add_mem(buf, data, chunk);
		int err = usbd_ep_enqueue(pd->c_data, buf);

		if (err) {
			LOG_ERR("ep_enqueue err=%d", err);
			net_buf_unref(buf);
			k_sem_give(&pd->free_slots);
			return err;
		}

		data += chunk;
		len  -= chunk;
	}
	return 0;
}

void pwr_bulk_flush(void)
{
	struct pwr_bulk_data *pd = &pwr_bulk_data_inst;

	/* Wait for the request handler to drain all in-flight transfers and
	 * give the all_done semaphore once free_slots is back to full. */
	(void)k_sem_take(&pd->all_done, K_FOREVER);
	k_sem_give(&pd->all_done);
}
