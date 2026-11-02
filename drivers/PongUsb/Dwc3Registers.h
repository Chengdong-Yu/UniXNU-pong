/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Generic Synopsys DWC3 register fields, adapted from NativeDwc3.
 * Copyright (c) 2026 The NativeDwc3 authors. See LICENSE.
 * No donor board addresses, GCC offsets or PHY constants are used.
 */
#ifndef PONG_DWC3_REGISTERS_H
#define PONG_DWC3_REGISTERS_H
// Register windows inside the block
#define DWC3_XHCI_REGS_START     0x0000
#define DWC3_GLOBALS_REGS_START  0xC100
#define DWC3_GLOBALS_REGS_END    0xC6FF
#define DWC3_DEVICE_REGS_START   0xC700
#define DWC3_DEVICE_REGS_END     0xCBFF
#define DWC3_OTG_REGS_START      0xCC00

// --- global registers -----------------------------------------------------
#define DWC3_GSBUSCFG0           0xC100
#define DWC3_GCTL                0xC110
#define DWC3_GEVTEN              0xC114
#define DWC3_GSTS                0xC118
#define DWC3_GSNPSID             0xC120
#define DWC3_GUID                0xC128
#define DWC3_GUCTL               0xC12C
#define DWC3_GHWPARAMS0          0xC140
#define DWC3_GHWPARAMS1          0xC144
#define DWC3_GHWPARAMS2          0xC148
#define DWC3_GHWPARAMS3          0xC14C
#define DWC3_GHWPARAMS4          0xC150
#define DWC3_GHWPARAMS6          0xC158

#define DWC3_GUSB2PHYCFG(n)      (0xC200 + ((n) * 0x04))
#define DWC3_GUSB3PIPECTL(n)     (0xC2C0 + ((n) * 0x04))
#define DWC3_GTXFIFOSIZ(n)       (0xC300 + ((n) * 0x04))
#define DWC3_GRXFIFOSIZ(n)       (0xC380 + ((n) * 0x04))

#define DWC3_GEVNTADRLO(n)       (0xC400 + ((n) * 0x10))
#define DWC3_GEVNTADRHI(n)       (0xC404 + ((n) * 0x10))
#define DWC3_GEVNTSIZ(n)         (0xC408 + ((n) * 0x10))
#define DWC3_GEVNTCOUNT(n)       (0xC40C + ((n) * 0x10))

// --- device registers -----------------------------------------------------
#define DWC3_DCFG                0xC700
#define DWC3_DCTL                0xC704
#define DWC3_DEVTEN              0xC708
#define DWC3_DSTS                0xC70C
#define DWC3_DGCMDPAR            0xC710
#define DWC3_DGCMD               0xC714
#define DWC3_DALEPENA            0xC720
#define DWC3_DEPCMDPAR2(n)       (0xC800 + ((n) * 0x10))
#define DWC3_DEPCMDPAR1(n)       (0xC804 + ((n) * 0x10))
#define DWC3_DEPCMDPAR0(n)       (0xC808 + ((n) * 0x10))
#define DWC3_DEPCMD(n)           (0xC80C + ((n) * 0x10))

// --- GCTL bits ------------------------------------------------------------
#define DWC3_GCTL_CORESOFTRESET  (1u << 11)
#define DWC3_GCTL_PRTCAP(n)      (((n) & (3u << 12)) >> 12)
#define DWC3_GCTL_PRTCAPDIR(n)   ((n) << 12)
#define DWC3_GCTL_PRTCAP_HOST    1u
#define DWC3_GCTL_PRTCAP_DEVICE  2u
#define DWC3_GCTL_PRTCAP_OTG     3u
#define DWC3_GCTL_SCALEDOWN(n)   ((n) << 4)
#define DWC3_GCTL_DISSCRAMBLE    (1u << 3)
#define DWC3_GCTL_DSBLCLKGTNG    (1u << 0)

// --- GUSB2PHYCFG bits -----------------------------------------------------
#define DWC3_GUSB2PHYCFG_PHYSOFTRST  (1u << 31)
#define DWC3_GUSB2PHYCFG_SUSPHY      (1u << 6)
#define DWC3_GUSB2PHYCFG_ULPI_UTMI   (1u << 4)
#define DWC3_GUSB2PHYCFG_ENBLSLPM    (1u << 8)

// --- DCTL bits ------------------------------------------------------------
#define DWC3_DCTL_RUN_STOP       (1u << 31)
#define DWC3_DCTL_CSFTRST        (1u << 30)
#define DWC3_DCTL_L0VLDO_VOLT(n) (((n) & 0x7u) << 6)
#define DWC3_DCTL_HIRD_THRES(n)  (((n) & 0x1Fu) << 24)
#define DWC3_DCTL_IGNORE_STREAM  (1u << 18)
/* Hold the D+ pull-up without VBUS detection (device side). */
#define DWC3_DCTL_KEEP_CONNECT   (1u << 19)

// --- DSTS bits ------------------------------------------------------------
#define DWC3_DSTS_CONNECTSPD(n)  ((n) & 0x7u)
#define DWC3_DSTS_SOF_FN(n)      (((n) >> 8) & 0x3FFFu)
#define DWC3_DSTS_DEVCTRLHLT     (1u << 22)

// --- GSNPSID --------------------------------------------------------------
// High 16 bits identify the core; 0x5533 is DWC_usb3 / 0x5534 is usb31.
#define DWC3_GSNPSID_MASK        0xFFFF0000u
#define DWC3_GSNPSID_DWC_USB3    0x55330000u
#define DWC3_GSNPSID_DWC_USB31   0x55340000u
/* DWC31_IP in dwc3/core.h: this board reports 0x33313130 */
#define DWC3_GSNPSID_DWC_USB31_3331 0x33310000u
#define DWC3_GSNPSREV_MASK       0x0000FFFFu

#define DWC3_OSTS             0xCC10

// --- endpoint commands ----------------------------------------------------
// DEPCMD layout: bits[5:0] command, bit8 CMDIOC, bit10 CMDACT, status[15:12].
#define DWC3_DEPCMD_CMDACT        (1u << 10)
#define DWC3_DEPCMD_CMDIOC        (1u << 8)
#define DWC3_DEPCMD_STATUS(x)     (((x) >> 12) & 0x0Fu)
#define DWC3_DEPCMD_PARAM(x)      ((x) << 16)

#define DWC3_DEPCMD_SETEPCONFIG       0x01u
#define DWC3_DEPCMD_SETTRANSFRESOURCE 0x02u
#define DWC3_DEPCMD_SETSTALL          0x04u
#define DWC3_DEPCMD_CLEARSTALL        0x05u
#define DWC3_DEPCMD_STARTTRANSFER     0x06u
#define DWC3_DEPCMD_ENDTRANSFER       0x08u
#define DWC3_DEPCMD_DEPSTARTCFG       0x09u

#define DWC3_DALEPENA_EP(n)       (1u << (n))

// --- DEPCFG parameter 0 / 1 (gadget.h) ------------------------------------
#define DWC3_DEPCFG_EP_TYPE(n)          (((n) & 0x3u) << 1)
#define DWC3_DEPCFG_MAX_PACKET_SIZE(n)  (((n) & 0x7FFu) << 3)
#define DWC3_DEPCFG_FIFO_NUMBER(n)      (((n) & 0x1Fu) << 17)
#define DWC3_DEPCFG_XFER_COMPLETE_EN    (1u << 8)
#define DWC3_DEPCFG_XFER_IN_PROGRESS_EN (1u << 9)
#define DWC3_DEPCFG_XFER_NOT_READY_EN   (1u << 10)
#define DWC3_DEPCFG_EP_NUMBER(n)        (((n) & 0x1Fu) << 25)
#define DWC3_DEPXFERCFG_NUM_XFER_RES(n) ((n) & 0xFFFFu)

// --- transfer requests ----------------------------------------------------
// {bpl,bph,size,ctrl}; the core writes back the remaining size and TRBSTS.
#define DWC3_TRB_CTRL_HWO         (1u << 0)
#define DWC3_TRB_CTRL_LST         (1u << 1)
#define DWC3_TRB_CTRL_CHN         (1u << 2)
#define DWC3_TRB_CTRL_ISP_IMI     (1u << 10)
#define DWC3_TRB_CTRL_IOC         (1u << 11)
#define DWC3_TRB_CTRL_CSP         (1u << 3)
#define DWC3_TRB_SIZE_MASK        0x00FFFFFFu
#define DWC3_TRB_SIZE_TRBSTS(x)   (((x) >> 28) & 0xFu)
#define DWC3_TRBSTS_SETUP_PENDING 2u

// TRBCTL occupies ctrl[9:4]; using the raw type number here leaves type 0,
// which the core completes without transferring anything.
#define DWC3_TRB_CTRL_TRBCTL(n)     (((n) & 0x3Fu) << 4)
#define DWC3_TRBCTL_NORMAL          DWC3_TRB_CTRL_TRBCTL(1)
#define DWC3_TRBCTL_CONTROL_SETUP   DWC3_TRB_CTRL_TRBCTL(2)
#define DWC3_TRBCTL_CONTROL_STATUS2 DWC3_TRB_CTRL_TRBCTL(3)
#define DWC3_TRBCTL_CONTROL_STATUS3 DWC3_TRB_CTRL_TRBCTL(4)
#define DWC3_TRBCTL_CONTROL_DATA    DWC3_TRB_CTRL_TRBCTL(5)

// --- endpoint events ------------------------------------------------------
// Endpoint event: number[5:1], event[9:6], status[15:12].
#define DWC3_DEPEVT_XFERCOMPLETE  1u
#define DWC3_DEPEVT_XFERNOTREADY  3u
#define DEPEVT_STATUS_CONTROL_DATA   1u
#define DEPEVT_STATUS_CONTROL_STATUS 2u

// --- device events --------------------------------------------------------
// Device event: type[11:8], event_info[24:16].
#define DWC3_DEV_EVENT_DISCONNECT   0u
#define DWC3_DEV_EVENT_RESET        1u
#define DWC3_DEV_EVENT_CONNECT_DONE 2u
#define DWC3_DEV_EVENT_LINK_STATUS  3u

// --- DCFG -----------------------------------------------------------------
#define DWC3_DCFG_DEVADDR(n)      (((n) & 0x7Fu) << 3)
#define DWC3_DCFG_DEVADDR_MASK    (0x7Fu << 3)

#endif // NABU_DWC3_REGS_H
