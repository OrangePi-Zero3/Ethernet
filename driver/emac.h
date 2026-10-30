/*
 * Orange Pi Zero3 (Allwinner H616/H618) EMAC0 NetAdapterCx driver
 * Shared definitions.
 */
#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <ntintsafe.h>

#include <netadaptercx.h>
#include <netadapter.h>
#include <net/logicaladdress.h>
#include <net/virtualaddress.h>

#include "emac_regs.h"
#include "netringiterator.h"

/* ------------------------------------------------------------------ tuning */
#define EMAC_POOL_TAG               'camE'
#define EMAC_MAC_LEN                6
#define EMAC_MTU                    1500
#define EMAC_MAX_FRAME              1514        /* without FCS */
#define EMAC_RX_BUFFER_MAX          (EMAC_MAX_FRAME + EMAC_FCS_SIZE + 2)
#define EMAC_RX_BUFFER_ALIGN        64
#define EMAC_TX_RING_HINT           128
#define EMAC_RX_RING_HINT           256
#define EMAC_TX_SLOT_SIZE           2048        /* >= 1514; one slot per TX descriptor */
/*
 * The OS may hand over a packet in several fragments; they are all copied into one TX slot,
 * so the hardware only ever sees a single FIRST|LAST descriptor (the path EmacDxe proved).
 */
#define EMAC_TX_MAX_FRAGMENTS       16
#define EMAC_DEFAULT_PHY_ADDRESS    1           /* EMAC_PHY_ADDRESS in EmacDxe.h */
#define EMAC_MAX_MULTICAST          (EMAC_ADDR_SLOTS - 1)
#define EMAC_LINK_POLL_MS           1000
#define EMAC_MAX_LINK_SPEED         1000000000ULL

#define EMAC_SUPPORTED_FILTERS                                              \
    (NetPacketFilterFlagDirected | NetPacketFilterFlagMulticast |           \
     NetPacketFilterFlagAllMulticast | NetPacketFilterFlagBroadcast |       \
     NetPacketFilterFlagPromiscuous)

/* The format string must carry its own "\n"; this keeps the macro valid with zero extra arguments. */
#define EMAC_LOG(...) \
    DbgPrintEx(DPFLTR_IHVNETWORK_ID, DPFLTR_ERROR_LEVEL, "EmacWin: " __VA_ARGS__)

/* ------------------------------------------------------------------ types */
typedef struct _EMAC_ADAPTER EMAC_ADAPTER;

typedef struct _EMAC_DEVICE_CONTEXT {
    EMAC_ADAPTER *Adapter;
} EMAC_DEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(EMAC_DEVICE_CONTEXT, EmacGetDeviceContext)

typedef struct _EMAC_INTERRUPT_CONTEXT {
    EMAC_ADAPTER *Adapter;
} EMAC_INTERRUPT_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(EMAC_INTERRUPT_CONTEXT, EmacGetInterruptContext)

typedef struct _EMAC_TIMER_CONTEXT {
    EMAC_ADAPTER *Adapter;
} EMAC_TIMER_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(EMAC_TIMER_CONTEXT, EmacGetTimerContext)

typedef struct _EMAC_TX_PACKET {
    ULONG FirstDesc;
    ULONG NumDesc;
} EMAC_TX_PACKET;

typedef struct _EMAC_TXQUEUE {
    EMAC_ADAPTER *Adapter;
    NETPACKETQUEUE Queue;
    const NET_RING_COLLECTION *Rings;
    NET_EXTENSION VirtualAddressExtension;
    WDFCOMMONBUFFER DescBuffer;
    EMAC_DESC_SLOT *Desc;
    ULONG DescBus;                  /* bus address of Desc[0] */
    WDFCOMMONBUFFER SlotBuffer;     /* NumDesc slots of EMAC_TX_SLOT_SIZE bytes */
    UCHAR *SlotVa;
    ULONG SlotBus;
    ULONG NumDesc;                  /* == packet ring element count */
    ULONG NumPackets;
    ULONG DescIndex;                /* next descriptor / slot to fill */
    EMAC_TX_PACKET *Packets;
} EMAC_TXQUEUE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(EMAC_TXQUEUE, EmacGetTxQueueContext)

typedef struct _EMAC_RXQUEUE {
    EMAC_ADAPTER *Adapter;
    NETPACKETQUEUE Queue;
    const NET_RING_COLLECTION *Rings;
    NET_EXTENSION LogicalAddressExtension;
    WDFCOMMONBUFFER DescBuffer;
    EMAC_DESC_SLOT *Desc;
    ULONG DescBus;
    ULONG NumDesc;                  /* == fragment ring element count */
} EMAC_RXQUEUE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(EMAC_RXQUEUE, EmacGetRxQueueContext)

struct _EMAC_ADAPTER {
    WDFDEVICE Device;
    NETADAPTER NetAdapter;
    NETCONFIGURATION Config;
    WDFDMAENABLER DmaEnabler;

    volatile UCHAR *Regs;           /* EMAC0, 0x05020000 */
    volatile UCHAR *Sid;            /* eFuse SID, 0x03006200 */

    WDFINTERRUPT Interrupt;
    WDFSPINLOCK Lock;               /* MDIO, CTL0, RX/TX control read-modify-write */
    WDFTIMER LinkTimer;

    NET_ADAPTER_LINK_LAYER_ADDRESS PermanentAddress;
    NET_ADAPTER_LINK_LAYER_ADDRESS CurrentAddress;

    NET_PACKET_FILTER_FLAGS PacketFilter;
    ULONG McastCount;
    NET_ADAPTER_LINK_LAYER_ADDRESS Mcast[EMAC_MAX_MULTICAST];

    ULONG PhyAddress;
    BOOLEAN PhyReady;
    BOOLEAN AdapterStarted;

    /* link state, protected by Lock */
    BOOLEAN LinkUp;
    ULONG LinkSpeedMbps;
    BOOLEAN FullDuplex;

    /* interrupt arming; IntEnShadow is protected by the interrupt lock */
    ULONG IntEnShadow;
    volatile LONG IntPending;

    EMAC_TXQUEUE *TxQueue;
    EMAC_RXQUEUE *RxQueue;
    BOOLEAN TxRunning;
    BOOLEAN RxRunning;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(EMAC_ADAPTER, EmacGetAdapterContext)

/* ------------------------------------------------------------ register access */
#define EMAC_RD(a, off)     READ_REGISTER_ULONG((volatile ULONG *)((a)->Regs + (off)))
#define EMAC_WR(a, off, v)  WRITE_REGISTER_ULONG((volatile ULONG *)((a)->Regs + (off)), (v))
#define EMAC_OR(a, off, v)  EMAC_WR(a, off, EMAC_RD(a, off) | (v))
#define EMAC_AND(a, off, v) EMAC_WR(a, off, EMAC_RD(a, off) & (v))

/* ------------------------------------------------------------------ phy.c */
NTSTATUS EmacMdioRead(EMAC_ADAPTER *A, UCHAR Reg, USHORT *Value);
NTSTATUS EmacMdioWrite(EMAC_ADAPTER *A, UCHAR Reg, USHORT Value);
NTSTATUS EmacPhyInitialize(EMAC_ADAPTER *A);                /* PASSIVE_LEVEL only */
NTSTATUS EmacPhyReadLink(EMAC_ADAPTER *A, BOOLEAN *Up, ULONG *SpeedMbps, BOOLEAN *Full);

/* --------------------------------------------------------------- datapath.c */
NTSTATUS EmacEvtCreateTxQueue(NETADAPTER NetAdapter, NETTXQUEUE_INIT *Init);
NTSTATUS EmacEvtCreateRxQueue(NETADAPTER NetAdapter, NETRXQUEUE_INIT *Init);
EVT_WDF_INTERRUPT_ISR EmacEvtInterruptIsr;
EVT_WDF_INTERRUPT_DPC EmacEvtInterruptDpc;
void EmacSetIntMask(EMAC_ADAPTER *A, ULONG Set, ULONG Clear);

/* ---------------------------------------------------------------- driver.c */
void EmacMacApplyLink(EMAC_ADAPTER *A);                      /* caller holds A->Lock */
void EmacMacProgramFilters(EMAC_ADAPTER *A);                 /* caller holds A->Lock */
