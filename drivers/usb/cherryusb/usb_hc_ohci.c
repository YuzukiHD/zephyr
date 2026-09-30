/*
 * Copyright (c) 2024, sakumisu
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * OHCI companion controller of the EHCI port. The root hub and the
 * controller initialisation come from the CherryUSB OHCI port; the transfers
 * (control, bulk and interrupt) are implemented here with the data cache
 * maintenance this SoC needs.
 */

#include "usb_hc_ohci.h"

/* Frame Interval / Periodic Start.
 *
 * At 12Mbps, there are 12000 bit time in each 1Msec frame.
 */

#define OHCI_FMINTERVAL_FI    (12000 - 1)
#define OHCI_FMINTERVAL_FSMPS ((6 * (OHCI_FMINTERVAL_FI - 210)) / 7)
#define DEFAULT_FMINTERVAL    ((OHCI_FMINTERVAL_FSMPS << OHCI_FMINT_FSMPS_SHIFT) | OHCI_FMINTERVAL_FI)
#define DEFAULT_PERSTART      ((OHCI_FMINTERVAL_FI * 9) / 10)

struct ohci_hcd g_ohci_hcd[CONFIG_USBHOST_MAX_BUS];

USB_NOCACHE_RAM_SECTION struct ohci_ed_hw g_ohci_ed_pool[CONFIG_USBHOST_MAX_BUS][CONFIG_USB_OHCI_ED_NUM];
USB_NOCACHE_RAM_SECTION struct ohci_hcca ohci_hcca[CONFIG_USBHOST_MAX_BUS] __attribute__((aligned(256)));


/* Transfers: one endpoint descriptor per (device address, endpoint) stays linked into the
 * controller lists and is reused; its transfer descriptors form a ring.
 */
struct ohci_ep_priv {
    bool valid;
    uint8_t type;
    uint8_t addr;   /* device address */
    uint8_t ep;     /* endpoint address (0 for control) */
    uint8_t first;  /* ring index of the first TD of the active transfer */
    uint8_t count;  /* TDs of the active transfer, without the dummy */
    struct usbh_urb *urb;
};

static struct ohci_ep_priv g_ohci_ep[CONFIG_USBHOST_MAX_BUS][CONFIG_USB_OHCI_ED_NUM];

#define OHCI_TD_NOTACCESSED (0xFU << GTD_STATUS_CC_SHIFT)
#define OHCI_TD_MAXLEN      4096U

static inline void ohci_desc_clean(void *p)
{
    usb_dcache_clean((uintptr_t)p, CONFIG_USB_OHCI_ALIGN_SIZE);
}

static inline void ohci_desc_invalidate(void *p)
{
    usb_dcache_invalidate((uintptr_t)p, CONFIG_USB_OHCI_ALIGN_SIZE);
}

static inline struct ohci_td_hw *ohci_td_next(struct ohci_ed_hw *ed, struct ohci_td_hw *td)
{
    return (td == &ed->td_pool[CONFIG_USB_OHCI_TD_NUM - 1]) ? &ed->td_pool[0] : td + 1;
}

int ohci_init(struct usbh_bus *bus)
{
    volatile uint32_t timeout = 0;
    uint32_t regval;
    struct ohci_ed_hw *ed;

    memset(&g_ohci_hcd[bus->hcd.hcd_id], 0, sizeof(struct ohci_hcd));
    memset(g_ohci_ed_pool[bus->hcd.hcd_id], 0, sizeof(struct ohci_ed_hw) * CONFIG_USB_OHCI_ED_NUM);
    memset(g_ohci_ep[bus->hcd.hcd_id], 0, sizeof(g_ohci_ep[bus->hcd.hcd_id]));
    memset(&ohci_hcca[bus->hcd.hcd_id], 0, sizeof(struct ohci_hcca));
    usb_dcache_clean((uintptr_t)&ohci_hcca[bus->hcd.hcd_id], sizeof(struct ohci_hcca));

    for (uint32_t i = 0; i < 32; i++) {
        ohci_hcca[bus->hcd.hcd_id].inttbl[i] = 0;
    }

    for (uint8_t index = 0; index < CONFIG_USB_OHCI_ED_NUM; index++) {
        ed = &g_ohci_ed_pool[bus->hcd.hcd_id][index];
        if ((uint32_t)&ed->hw % 32) {
            USB_LOG_ERR("struct ohci_ed_hw is not align 32\r\n");
            return -USB_ERR_INVAL;
        }
        for (uint8_t i = 0; i < CONFIG_USB_OHCI_TD_NUM; i++) {
            if ((uint32_t)&ed->td_pool[i] % 32) {
                USB_LOG_ERR("struct ohci_td_hw is not align 32\r\n");
                return -USB_ERR_INVAL;
            }
        }
    }

    for (uint8_t index = 0; index < CONFIG_USB_OHCI_ED_NUM; index++) {
        ed = &g_ohci_ed_pool[bus->hcd.hcd_id][index];
        ed->waitsem = usb_osal_sem_create(0);
        USB_ASSERT(ed->waitsem != NULL);
    }

    USB_LOG_INFO("OHCI hcrevision:0x%02x\r\n", (unsigned int)OHCI_HCOR->hcrevision);

    OHCI_HCOR->hcintdis = OHCI_INT_MIE;
    OHCI_HCOR->hccontrol = 0;

    OHCI_HCOR->hccmdsts = OHCI_CMDST_HCR;
    while (OHCI_HCOR->hccmdsts & OHCI_CMDST_HCR) {
        usb_osal_msleep(1);
        timeout++;
        if (timeout > 100) {
            return -USB_ERR_TIMEOUT;
        }
    }

    OHCI_HCOR->hcfminterval = DEFAULT_FMINTERVAL;
    OHCI_HCOR->hcperiodicstart = DEFAULT_PERSTART;
    OHCI_HCOR->hclsthreshold = 0x628;

    OHCI_HCOR->hccontrolheaded = 0;
    OHCI_HCOR->hcbulkheaded = 0;
    OHCI_HCOR->hchcca = (uintptr_t)&ohci_hcca[bus->hcd.hcd_id];

    /* Clear pending interrupts */
    regval = OHCI_HCOR->hcintsts;
    OHCI_HCOR->hcintsts = regval;

    /* Put HC in operational state */
    regval = OHCI_HCOR->hccontrol;
    regval &= ~OHCI_CTRL_CBSR;
    regval &= ~OHCI_CTRL_HCFS_MASK;
    regval |= OHCI_CTRL_HCFS_OPER;
    regval |= OHCI_CTRL_CBSR;
    regval |= OHCI_CTRL_CLE | OHCI_CTRL_BLE | OHCI_CTRL_PLE;
    OHCI_HCOR->hccontrol = regval;

    g_ohci_hcd[bus->hcd.hcd_id].n_ports = OHCI_HCOR->hcrhdescriptora & OHCI_RHDESCA_NDP_MASK;
    USB_LOG_INFO("OHCI n_ports:%d\r\n", g_ohci_hcd[bus->hcd.hcd_id].n_ports);

    OHCI_HCOR->hcrhdescriptora &= ~OHCI_RHDESCA_PSM;
    OHCI_HCOR->hcrhdescriptora &= ~OHCI_RHDESCA_NPS;

    /* Set global power in HcRhStatus */
    OHCI_HCOR->hcrhsts = OHCI_RHSTATUS_SGP;
    usb_osal_msleep(20);

    /* Enable OHCI interrupts */
    OHCI_HCOR->hcinten = OHCI_INT_WDH | OHCI_INT_RHSC | OHCI_INT_UE | OHCI_INT_MIE;

    return 0;
}

int ohci_deinit(struct usbh_bus *bus)
{
    uint32_t regval;
    struct ohci_ed_hw *ed;

    /* Disable OHCI interrupts */
    OHCI_HCOR->hcintdis = OHCI_INT_WDH | OHCI_INT_RHSC | OHCI_INT_MIE;

    /* Clear pending interrupts */
    regval = OHCI_HCOR->hcintsts;
    OHCI_HCOR->hcintsts = regval;

    OHCI_HCOR->hcrhsts &= ~OHCI_RHSTATUS_SGP;

    regval = OHCI_HCOR->hccontrol;
    regval &= ~OHCI_CTRL_HCFS_MASK;
    regval |= OHCI_CTRL_HCFS_SUSPEND;
    OHCI_HCOR->hccontrol = regval;

    for (uint8_t index = 0; index < CONFIG_USB_OHCI_ED_NUM; index++) {
        ed = &g_ohci_ed_pool[bus->hcd.hcd_id][index];
        usb_osal_sem_delete(ed->waitsem);
    }

    return 0;
}

uint16_t ohci_get_frame_number(struct usbh_bus *bus)
{
    return OHCI_HCOR->hcfmnumber;
}

int ohci_roothub_control(struct usbh_bus *bus, struct usb_setup_packet *setup, uint8_t *buf)
{
    uint8_t nports;
    uint8_t port;
    uint32_t temp;

    nports = g_ohci_hcd[bus->hcd.hcd_id].n_ports;

    port = setup->wIndex;
    if (setup->bmRequestType & USB_REQUEST_RECIPIENT_DEVICE) {
        switch (setup->bRequest) {
            case HUB_REQUEST_CLEAR_FEATURE:
                switch (setup->wValue) {
                    case HUB_FEATURE_HUB_C_LOCALPOWER:
                        break;
                    case HUB_FEATURE_HUB_C_OVERCURRENT:
                        break;
                    default:
                        return -USB_ERR_NOTSUPP;
                }
                break;
            case HUB_REQUEST_SET_FEATURE:
                switch (setup->wValue) {
                    case HUB_FEATURE_HUB_C_LOCALPOWER:
                        break;
                    case HUB_FEATURE_HUB_C_OVERCURRENT:
                        break;
                    default:
                        return -USB_ERR_NOTSUPP;
                }
                break;
            case HUB_REQUEST_GET_DESCRIPTOR:
                break;
            case HUB_REQUEST_GET_STATUS:
                memset(buf, 0, 4);
                break;
            default:
                break;
        }
    } else if (setup->bmRequestType & USB_REQUEST_RECIPIENT_OTHER) {
        switch (setup->bRequest) {
            case HUB_REQUEST_CLEAR_FEATURE:
                if (!port || port > nports) {
                    return -USB_ERR_INVAL;
                }

                switch (setup->wValue) {
                    case HUB_PORT_FEATURE_ENABLE:
                        temp = OHCI_RHPORTST_CCS;
                        break;
                    case HUB_PORT_FEATURE_SUSPEND:
                        temp = OHCI_HCOR->hccontrol;
                        temp &= ~OHCI_CTRL_HCFS_MASK;
                        temp |= OHCI_CTRL_HCFS_RESUME;
                        OHCI_HCOR->hccontrol = temp;

                        usb_osal_msleep(20);

                        temp = OHCI_HCOR->hccontrol;
                        temp &= ~OHCI_CTRL_HCFS_MASK;
                        temp |= OHCI_CTRL_HCFS_OPER;
                        OHCI_HCOR->hccontrol = temp;

                        temp = OHCI_RHPORTST_POCI;
                        break;
                    case HUB_PORT_FEATURE_C_SUSPEND:
                        temp = OHCI_RHPORTST_PSSC;
                        break;
                    case HUB_PORT_FEATURE_POWER:
                        OHCI_HCOR->hcrhsts = OHCI_RHSTATUS_CGP;
                        temp = OHCI_RHPORTST_LSDA;
                        break;
                    case HUB_PORT_FEATURE_C_CONNECTION:
                        temp = OHCI_RHPORTST_CSC;
                        break;
                    case HUB_PORT_FEATURE_C_ENABLE:
                        temp = OHCI_RHPORTST_PESC;
                        break;
                    case HUB_PORT_FEATURE_C_OVER_CURREN:
                        temp = OHCI_RHPORTST_OCIC;
                        break;
                    case HUB_PORT_FEATURE_C_RESET:
                        temp = OHCI_RHPORTST_PRSC;
                        break;
                    default:
                        return -USB_ERR_NOTSUPP;
                }
                OHCI_HCOR->hcrhportsts[port - 1] = temp;
                break;
            case HUB_REQUEST_SET_FEATURE:
                if (!port || port > nports) {
                    return -USB_ERR_INVAL;
                }

                switch (setup->wValue) {
                    case HUB_PORT_FEATURE_SUSPEND:
                        temp = OHCI_HCOR->hccontrol;
                        temp &= ~OHCI_CTRL_HCFS_MASK;
                        temp |= OHCI_CTRL_HCFS_SUSPEND;
                        OHCI_HCOR->hccontrol = temp;

                        break;
                    case HUB_PORT_FEATURE_POWER:
                        OHCI_HCOR->hcrhsts = OHCI_RHSTATUS_SGP;
                        break;
                    case HUB_PORT_FEATURE_RESET:
                        OHCI_HCOR->hcrhportsts[port - 1] = OHCI_RHPORTST_PRS;

                        while (OHCI_HCOR->hcrhportsts[port - 1] & OHCI_RHPORTST_PRS) {
                        }
                        break;

                    default:
                        return -USB_ERR_NOTSUPP;
                }
                break;
            case HUB_REQUEST_GET_STATUS:
                if (!port || port > nports) {
                    return -USB_ERR_INVAL;
                }
                temp = OHCI_HCOR->hcrhportsts[port - 1];
                memcpy(buf, &temp, 4);
                break;
            default:
                break;
        }
    }
    return 0;
}

/* ---- list management (task context) ---- */

static void ohci_list_enable(struct usbh_bus *bus, uint32_t bit, bool on)
{
    uint32_t regval = OHCI_HCOR->hccontrol;

    if (on) {
        regval |= bit;
    } else {
        regval &= ~bit;
    }
    OHCI_HCOR->hccontrol = regval;
}

static uint32_t ohci_list_bit(uint8_t type)
{
    switch (type) {
        case USB_ENDPOINT_TYPE_CONTROL:
            return OHCI_CTRL_CLE;
        case USB_ENDPOINT_TYPE_BULK:
            return OHCI_CTRL_BLE;
        default:
            return OHCI_CTRL_PLE;
    }
}

/* Append a descriptor to the list of its transfer type; the descriptor has to be idle and skipped */
static void ohci_ed_link(struct usbh_bus *bus, struct ohci_ed_hw *ed, uint8_t type)
{
    struct ohci_hcca *hcca = &ohci_hcca[bus->hcd.hcd_id];
    volatile uint32_t *head;
    struct ohci_ed_hw *tail;
    uint32_t cur;

    if (type == USB_ENDPOINT_TYPE_INTERRUPT) {
        /* one chain polled every frame: every slot of the interrupt table points to it */
        ed->hw.nexted = hcca->inttbl[0];
        ohci_desc_clean(ed);
        for (uint32_t i = 0; i < 32; i++) {
            hcca->inttbl[i] = OHCI_PTR2ADDR(ed);
        }
        usb_dcache_clean((uintptr_t)hcca->inttbl, sizeof(hcca->inttbl));
        return;
    }

    head = (type == USB_ENDPOINT_TYPE_CONTROL) ? &OHCI_HCOR->hccontrolheaded : &OHCI_HCOR->hcbulkheaded;

    ed->hw.nexted = 0;
    ohci_desc_clean(ed);

    cur = *head;
    if (cur == 0) {
        *head = OHCI_PTR2ADDR(ed);
        return;
    }

    tail = OHCI_ADDR2ED(cur);
    while (tail->hw.nexted) {
        tail = OHCI_ADDR2ED(tail->hw.nexted);
    }
    tail->hw.nexted = OHCI_PTR2ADDR(ed);
    ohci_desc_clean(tail);
}

/* Take a descriptor out of its list. The controller is told to stop that list first. */
static void ohci_ed_unlink(struct usbh_bus *bus, struct ohci_ed_hw *ed, uint8_t type)
{
    struct ohci_hcca *hcca = &ohci_hcca[bus->hcd.hcd_id];
    uint32_t bit = ohci_list_bit(type);
    volatile uint32_t *head;
    struct ohci_ed_hw *prev;
    uint32_t addr = OHCI_PTR2ADDR(ed);

    ohci_list_enable(bus, bit, false);
    usb_osal_msleep(2);

    if (type == USB_ENDPOINT_TYPE_INTERRUPT) {
        uint32_t cur = hcca->inttbl[0];

        if (cur == addr) {
            for (uint32_t i = 0; i < 32; i++) {
                hcca->inttbl[i] = ed->hw.nexted;
            }
            usb_dcache_clean((uintptr_t)hcca->inttbl, sizeof(hcca->inttbl));
        } else {
            prev = OHCI_ADDR2ED(cur);
            while (prev && prev->hw.nexted != addr) {
                prev = prev->hw.nexted ? OHCI_ADDR2ED(prev->hw.nexted) : NULL;
            }
            if (prev) {
                prev->hw.nexted = ed->hw.nexted;
                ohci_desc_clean(prev);
            }
        }
    } else {
        head = (type == USB_ENDPOINT_TYPE_CONTROL) ? &OHCI_HCOR->hccontrolheaded : &OHCI_HCOR->hcbulkheaded;

        if (*head == addr) {
            *head = ed->hw.nexted;
        } else {
            prev = OHCI_ADDR2ED(*head);
            while (prev && prev->hw.nexted != addr) {
                prev = prev->hw.nexted ? OHCI_ADDR2ED(prev->hw.nexted) : NULL;
            }
            if (prev) {
                prev->hw.nexted = ed->hw.nexted;
                ohci_desc_clean(prev);
            }
        }

        if (type == USB_ENDPOINT_TYPE_CONTROL) {
            OHCI_HCOR->hccontrolcurrented = 0;
        } else {
            OHCI_HCOR->hcbulkcurrented = 0;
        }
    }

    ohci_list_enable(bus, bit, true);
}

/* Find the descriptor of an endpoint or set up a new one */
static struct ohci_ed_hw *ohci_ed_get(struct usbh_bus *bus, struct usbh_urb *urb, uint8_t type)
{
    uint8_t id = bus->hcd.hcd_id;
    uint8_t addr = urb->hport->dev_addr;
    uint8_t ep = (type == USB_ENDPOINT_TYPE_CONTROL) ? 0 : urb->ep->bEndpointAddress;
    struct ohci_ep_priv *free_slot = NULL;
    struct ohci_ep_priv *idle_slot = NULL;
    struct ohci_ed_hw *ed;
    struct ohci_td_hw *dummy;
    uint32_t i;

    for (i = 0; i < CONFIG_USB_OHCI_ED_NUM; i++) {
        struct ohci_ep_priv *p = &g_ohci_ep[id][i];

        if (p->valid && p->addr == addr && p->ep == ep && p->type == type) {
            return &g_ohci_ed_pool[id][i];
        }
        if (!p->valid && free_slot == NULL) {
            free_slot = p;
        }
        if (p->valid && p->urb == NULL && idle_slot == NULL) {
            idle_slot = p;
        }
    }

    if (free_slot == NULL && idle_slot == NULL) {
        return NULL;
    }

    if (free_slot == NULL) {
        /* reuse the descriptor of an idle endpoint, most likely of a device that is gone */
        i = idle_slot - g_ohci_ep[id];
        ohci_ed_unlink(bus, &g_ohci_ed_pool[id][i], idle_slot->type);
        idle_slot->valid = false;
        free_slot = idle_slot;
    }

    i = free_slot - g_ohci_ep[id];
    ed = &g_ohci_ed_pool[id][i];

    free_slot->addr = addr;
    free_slot->ep = ep;
    free_slot->type = type;
    free_slot->urb = NULL;
    free_slot->first = 0;
    free_slot->count = 0;

    /* an idle descriptor: head and tail point to the same dummy descriptor */
    dummy = &ed->td_pool[0];
    memset(&dummy->hw, 0, sizeof(dummy->hw));
    ed->hw.ctrl = ED_CONTROL_SKIP;
    ed->hw.tailp = OHCI_PTR2ADDR(dummy);
    ed->hw.headp = OHCI_PTR2ADDR(dummy);
    ohci_desc_clean(&dummy->hw);
    ohci_desc_clean(ed);

    ohci_ed_link(bus, ed, type);
    free_slot->valid = true;
    return ed;
}

/* ---- transfers ---- */

static struct ohci_td_hw *ohci_td_fill(struct ohci_ed_hw *ed, struct ohci_td_hw *td, struct usbh_urb *urb,
                                       uint32_t dp, uint32_t toggle, uint32_t buf, uint32_t len)
{
    struct ohci_td_hw *next = ohci_td_next(ed, td);

    td->hw.ctrl = OHCI_TD_NOTACCESSED | toggle | dp | GTD_STATUS_R;
    if (dp == GTD_STATUS_DP_OUT || dp == GTD_STATUS_DP_SETUP) {
        td->hw.ctrl &= ~GTD_STATUS_R;
    }
    if (len) {
        td->hw.cbp = buf;
        td->hw.be = buf + len - 1;
    } else {
        td->hw.cbp = 0;
        td->hw.be = 0;
    }
    td->hw.nexttd = OHCI_PTR2ADDR(next);
    td->urb = urb;
    td->dir_in = (dp == GTD_STATUS_DP_IN);
    td->buf_start = buf;
    td->length = len;
    ohci_desc_clean(&td->hw);
    return next;
}

/* Chain the data stage of a transfer in chunks the descriptors can address */
static struct ohci_td_hw *ohci_td_fill_data(struct ohci_ed_hw *ed, struct ohci_td_hw *td, struct usbh_urb *urb,
                                            uint32_t dp, uint32_t first_toggle, uint8_t *buffer,
                                            uint32_t buflen, uint8_t *count)
{
    uint32_t toggle = first_toggle;

    do {
        uint32_t chunk = buflen > OHCI_TD_MAXLEN ? OHCI_TD_MAXLEN : buflen;

        td = ohci_td_fill(ed, td, urb, dp, toggle, (uint32_t)(uintptr_t)buffer, chunk);
        (*count)++;
        buffer += chunk;
        buflen -= chunk;
        toggle = 0; /* the descriptor keeps the toggle from here on */
    } while (buflen > 0);

    return td;
}

int ohci_submit_urb(struct usbh_urb *urb)
{
    struct usbh_bus *bus = urb->hport->bus;
    uint8_t id = bus->hcd.hcd_id;
    uint8_t type = USB_GET_ENDPOINT_TYPE(urb->ep->bmAttributes);
    struct ohci_ed_hw *ed;
    struct ohci_ep_priv *priv;
    struct ohci_td_hw *td, *first;
    uint32_t toggle;
    uint8_t count = 0;
    bool in;
    size_t flags;
    int ret = 0;

    if (type == USB_ENDPOINT_TYPE_ISOCHRONOUS) {
        return -USB_ERR_NOTSUPP;
    }

    if (urb->transfer_buffer_length > (uint32_t)(CONFIG_USB_OHCI_TD_NUM - 3) * OHCI_TD_MAXLEN) {
        return -USB_ERR_RANGE;
    }

    if (!urb->hport->connected) {
        return -USB_ERR_NOTCONN;
    }

    ed = ohci_ed_get(bus, urb, type);
    if (ed == NULL) {
        return -USB_ERR_NOMEM;
    }
    priv = &g_ohci_ep[id][ed - g_ohci_ed_pool[id]];

    flags = usb_osal_enter_critical_section();
    if (priv->urb != NULL) {
        usb_osal_leave_critical_section(flags);
        return -USB_ERR_BUSY;
    }
    priv->urb = urb;
    urb->hcpriv = ed;
    urb->errorcode = -USB_ERR_BUSY;
    urb->actual_length = 0;
    usb_osal_leave_critical_section(flags);

    /* the ring: the dummy descriptor the head points to becomes the first one of the transfer */
    ohci_desc_invalidate(ed);
    first = OHCI_ADDR2TD(ed->hw.tailp);
    td = first;

    if (type == USB_ENDPOINT_TYPE_CONTROL) {
        bool data_in = !!(urb->setup->bmRequestType & USB_REQUEST_DIR_IN);

        usb_dcache_clean((uintptr_t)urb->setup, USB_ALIGN_UP(sizeof(struct usb_setup_packet), CONFIG_USB_ALIGN_SIZE));

        td = ohci_td_fill(ed, td, urb, GTD_STATUS_DP_SETUP, GTD_STATUS_T_DATA0,
                          (uint32_t)(uintptr_t)urb->setup, sizeof(struct usb_setup_packet));
        count++;

        if (urb->transfer_buffer_length > 0) {
            if (data_in) {
                usb_dcache_invalidate((uintptr_t)urb->transfer_buffer, USB_ALIGN_UP(urb->transfer_buffer_length, CONFIG_USB_ALIGN_SIZE));
            } else {
                usb_dcache_clean((uintptr_t)urb->transfer_buffer, USB_ALIGN_UP(urb->transfer_buffer_length, CONFIG_USB_ALIGN_SIZE));
            }
            td = ohci_td_fill_data(ed, td, urb, data_in ? GTD_STATUS_DP_IN : GTD_STATUS_DP_OUT,
                                   GTD_STATUS_T_DATA1, urb->transfer_buffer, urb->transfer_buffer_length, &count);
        }

        /* status stage, opposite direction (always in when there is no data stage) */
        in = (urb->transfer_buffer_length > 0) ? !data_in : true;
        td = ohci_td_fill(ed, td, urb, in ? GTD_STATUS_DP_IN : GTD_STATUS_DP_OUT, GTD_STATUS_T_DATA1, 0, 0);
        count++;
    } else {
        in = !!(urb->ep->bEndpointAddress & 0x80);
        toggle = urb->data_toggle ? GTD_STATUS_T_DATA1 : GTD_STATUS_T_DATA0;

        if (urb->transfer_buffer_length > 0) {
            if (in) {
                usb_dcache_invalidate((uintptr_t)urb->transfer_buffer, USB_ALIGN_UP(urb->transfer_buffer_length, CONFIG_USB_ALIGN_SIZE));
            } else {
                usb_dcache_clean((uintptr_t)urb->transfer_buffer, USB_ALIGN_UP(urb->transfer_buffer_length, CONFIG_USB_ALIGN_SIZE));
            }
        }
        td = ohci_td_fill_data(ed, td, urb, in ? GTD_STATUS_DP_IN : GTD_STATUS_DP_OUT, toggle,
                               urb->transfer_buffer, urb->transfer_buffer_length, &count);
    }

    /* the new dummy descriptor at the end */
    memset(&td->hw, 0, sizeof(td->hw));
    ohci_desc_clean(&td->hw);

    priv->first = first - ed->td_pool;
    priv->count = count;
    ed->td_count = count;

    ed->hw.ctrl = (urb->hport->dev_addr << ED_CONTROL_FA_SHIFT) |
                  ((type == USB_ENDPOINT_TYPE_CONTROL ? 0 : (urb->ep->bEndpointAddress & 0x0f)) << ED_CONTROL_EN_SHIFT) |
                  (urb->hport->speed == USB_SPEED_LOW ? ED_CONTROL_SPPED_LOW : 0) |
                  ((uint32_t)USB_GET_MAXPACKETSIZE(urb->ep->wMaxPacketSize) << ED_CONTROL_MPS_SHIFT);
    /* publishing the tail makes the transfer visible to the controller */
    ed->hw.tailp = OHCI_PTR2ADDR(td);
    ohci_desc_clean(ed);

    if (type == USB_ENDPOINT_TYPE_CONTROL) {
        OHCI_HCOR->hccmdsts = OHCI_CMDST_CLF;
    } else if (type == USB_ENDPOINT_TYPE_BULK) {
        OHCI_HCOR->hccmdsts = OHCI_CMDST_BLF;
    }

    if (urb->timeout > 0) {
        ret = usb_osal_sem_take(ed->waitsem, urb->timeout);
        if (ret < 0) {
            urb->timeout = 0;
            ohci_kill_urb(urb);
            return ret;
        }
        urb->timeout = 0;
        ret = urb->errorcode;
    }

    return ret;
}

int ohci_kill_urb(struct usbh_urb *urb)
{
    struct usbh_bus *bus;
    struct ohci_ed_hw *ed;
    struct ohci_ep_priv *priv;
    size_t flags;

    if (!urb || !urb->hport || !urb->hcpriv || !urb->hport->bus) {
        return -USB_ERR_INVAL;
    }

    bus = urb->hport->bus;
    ed = (struct ohci_ed_hw *)urb->hcpriv;
    priv = &g_ohci_ep[bus->hcd.hcd_id][ed - g_ohci_ed_pool[bus->hcd.hcd_id]];

    flags = usb_osal_enter_critical_section();
    if (priv->urb != urb) {
        usb_osal_leave_critical_section(flags);
        return -USB_ERR_INVAL;
    }
    /* the interrupt handler can no longer complete it */
    priv->urb = NULL;
    usb_osal_leave_critical_section(flags);

    /* stop the controller from using the descriptor, then drop the transfer */
    ed->hw.ctrl |= ED_CONTROL_SKIP;
    ohci_desc_clean(ed);
    usb_osal_msleep(2);

    ed->hw.headp = ed->hw.tailp;
    ed->hw.ctrl &= ~ED_CONTROL_SKIP;
    ohci_desc_clean(ed);
    ed->td_count = 0;

    urb->hcpriv = NULL;
    urb->errorcode = -USB_ERR_SHUTDOWN;
    if (urb->complete) {
        urb->complete(urb->arg, urb->errorcode);
    }

    return 0;
}

static int ohci_cc_to_errno(uint32_t cc)
{
    switch (cc) {
        case TD_CC_STALL:
            return -USB_ERR_STALL;
        case 0x8: /* data overrun */
        case 0xc: /* buffer overrun */
            return -USB_ERR_BABBLE;
        default:
            return -USB_ERR_IO;
    }
}

/* Complete the transfer of a descriptor when the controller finished with it */
static void ohci_check_ed(struct usbh_bus *bus, struct ohci_ed_hw *ed, struct ohci_ep_priv *priv)
{
    struct usbh_urb *urb = priv->urb;
    struct ohci_td_hw *td;
    uint32_t headp;
    int errorcode = 0;
    uint32_t actual = 0;
    size_t flags;

    ohci_desc_invalidate(ed);
    headp = ed->hw.headp;

    if ((headp & ED_HEADP_ADDR_MASK) != ed->hw.tailp && !(headp & ED_HEADP_H)) {
        return;
    }

    td = &ed->td_pool[priv->first];
    for (uint8_t i = 0; i < priv->count; i++, td = ohci_td_next(ed, td)) {
        uint32_t cc;

        ohci_desc_invalidate(&td->hw);
        cc = (td->hw.ctrl & GTD_STATUS_CC_MASK) >> GTD_STATUS_CC_SHIFT;
        if (cc == 0xF || cc == 0xE) {
            break;
        }
        if (cc != TD_CC_NOERROR) {
            errorcode = ohci_cc_to_errno(cc);
            break;
        }
        actual += (td->hw.cbp == 0) ? td->length : (td->hw.cbp - td->buf_start);
    }

    if (headp & ED_HEADP_H) {
        /* halted by an error (or by a stall): resume at the dummy descriptor for the next transfer */
        ed->hw.headp = ed->hw.tailp;
        ohci_desc_clean(ed);
        if (errorcode == 0) {
            errorcode = -USB_ERR_IO;
        }
    }

    flags = usb_osal_enter_critical_section();
    if (priv->urb != urb) {
        usb_osal_leave_critical_section(flags);
        return;
    }
    priv->urb = NULL;
    usb_osal_leave_critical_section(flags);

    ed->td_count = 0;
    urb->hcpriv = NULL;
    urb->actual_length = actual;
    urb->data_toggle = !!(headp & ED_HEADP_C);
    urb->errorcode = errorcode;

#ifdef CONFIG_USB_DCACHE_ENABLE
    if (errorcode == 0 && urb->transfer_buffer && urb->actual_length) {
        bool in = (USB_GET_ENDPOINT_TYPE(urb->ep->bmAttributes) == USB_ENDPOINT_TYPE_CONTROL)
                      ? !!(urb->setup->bmRequestType & USB_REQUEST_DIR_IN)
                      : !!(urb->ep->bEndpointAddress & 0x80);

        if (in) {
            usb_dcache_invalidate((uintptr_t)urb->transfer_buffer, USB_ALIGN_UP(urb->transfer_buffer_length, CONFIG_USB_ALIGN_SIZE));
        }
    }
#endif

    if (urb->timeout) {
        usb_osal_sem_give(ed->waitsem);
    }

    if (urb->complete) {
        if (urb->errorcode < 0) {
            urb->complete(urb->arg, urb->errorcode);
        } else {
            urb->complete(urb->arg, urb->actual_length);
        }
    }
}

void OHCI_IRQHandler(uint8_t busid)
{
    uint32_t usbsts;
    struct usbh_bus *bus;

    bus = &g_usbhost_bus[busid];

    usbsts = OHCI_HCOR->hcintsts & OHCI_HCOR->hcinten;

    if (usbsts & OHCI_INT_RHSC) {
        OHCI_HCOR->hcintsts = OHCI_INT_RHSC;
        for (int port = 0; port < CONFIG_USBHOST_MAX_RHPORTS; port++) {
            uint32_t portsc = OHCI_HCOR->hcrhportsts[port];

            if (portsc & OHCI_RHPORTST_CSC) {
                if (OHCI_HCOR->hcrhsts & OHCI_RHSTATUS_DRWE) {
                    /* If DRWE is set, Connect Status Change indicates a remote wake-up event */
                } else {
                    bus->hcd.roothub.int_buffer[0] |= (1 << (port + 1));
                    usbh_hub_thread_wakeup(&bus->hcd.roothub);
                }
            }
        }
    }

    if (usbsts & OHCI_INT_WDH) {
        /* acknowledge first, a descriptor retired meanwhile raises it again */
        OHCI_HCOR->hcintsts = OHCI_INT_WDH;

        for (uint32_t i = 0; i < CONFIG_USB_OHCI_ED_NUM; i++) {
            struct ohci_ep_priv *priv = &g_ohci_ep[busid][i];

            if (priv->valid && priv->urb != NULL) {
                ohci_check_ed(bus, &g_ohci_ed_pool[busid][i], priv);
            }
        }
    }

    if (usbsts & OHCI_INT_UE) {
        OHCI_HCOR->hcintsts = OHCI_INT_UE;
        USB_LOG_ERR("OHCI unrecoverable error\r\n");
    }
}

#ifndef CONFIG_USB_EHCI_WITH_OHCI
extern void usb_hc_low_level_init(struct usbh_bus *bus);
extern void usb_hc_low_level_deinit(struct usbh_bus *bus);

int usb_hc_init(struct usbh_bus *bus)
{
    usb_hc_low_level_init(bus);
    return ohci_init(bus);
}

int usb_hc_deinit(struct usbh_bus *bus)
{
    ohci_deinit(bus);
    usb_hc_low_level_deinit(bus);
    return 0;
}

int usbh_roothub_control(struct usbh_bus *bus, struct usb_setup_packet *setup, uint8_t *buf)
{
    return ohci_roothub_control(bus, setup, buf);
}

int usbh_submit_urb(struct usbh_urb *urb)
{
    return ohci_submit_urb(urb);
}

int usbh_kill_urb(struct usbh_urb *urb)
{
    return ohci_kill_urb(urb);
}

void USBH_IRQHandler(uint8_t busid)
{
    OHCI_IRQHandler(busid);
}
#endif