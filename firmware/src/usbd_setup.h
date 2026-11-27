#ifndef BLIKACHU_USBD_SETUP_H
#define BLIKACHU_USBD_SETUP_H

#include <zephyr/usb/usbd.h>

/* Bring up the USB device controller with CDC ACM + DFU composite. Returns
 * 0 on success, negative errno on failure.
 */
int usbd_setup(void);

/* Register a callback that receives all USBD events (configured /
 * suspended / resumed / etc.). Call before
 * `usbd_setup()`. Pass NULL to clear.
 */
void usbd_setup_register_msg_cb(usbd_msg_cb_t cb);

#endif
