#ifndef BLIKACHU_USBD_SETUP_H
#define BLIKACHU_USBD_SETUP_H

#include <zephyr/usb/usbd.h>

/* Bring up the USB device controller with CDC ACM + MSC composite. Returns
 * 0 on success, negative errno on failure.
 */
int usbd_setup(void);

/* Register a callback that receives all USBD events (configured /
 * suspended / resumed / MSC LUN ejected / etc.). Call before
 * `usbd_setup()`. Pass NULL to clear.
 */
void usbd_setup_register_msg_cb(usbd_msg_cb_t cb);

/* Detach from the bus and re-attach — forces the host to re-probe the
 * composite device. The only reliable device-side way to make the host
 * notice an MSC medium change (macOS polls an idle volume lazily, if at
 * all). Blips the CDC console. Blocks ~150 ms; do not call from ISR or
 * the input dispatcher.
 */
int usbd_setup_reattach(void);

#endif
