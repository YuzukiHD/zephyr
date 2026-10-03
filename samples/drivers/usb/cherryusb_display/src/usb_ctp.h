/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef USB_CTP_H_
#define USB_CTP_H_

/* start the thread that reads the touch panel and sends its points to the USB touch screen */
void usb_ctp_start(void);

#endif
