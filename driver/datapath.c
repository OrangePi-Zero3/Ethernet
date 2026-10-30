/*
 * TX/RX queues and interrupt handling.
 *
 * Ring model (same as Microsoft's RtEthSample): one hardware descriptor per fragment-ring
 * element; the descriptor index equals the fragment index on RX, and TX uses a private free-
 * running index over a ring of the same size. Descriptors live in a WDF common buffer, which
 * on this non-coherent platform (_CCA = 0) is uncached, so only compiler/CPU barriers are
 * needed around ownership hand-over.
 */
#include "emac.h"

/* ============================================================== interrupts */

/* Arm/disarm interrupt sources. Takes the interrupt lock. */
void EmacSetIntMask(EMAC_ADAPTER *A, ULONG Set, ULONG Clear)
{
    WdfInterruptAcquireLock(A->Interrupt);
    A->IntEnShadow = (A->IntEnShadow | Set) & ~Clear;
    EMAC_WR(A, EMAC_INT_EN, A->IntEnShadow);
    WdfInterruptReleaseLock(A->Interrupt);
}

BOOLEAN EmacEvtInterruptIsr(WDFINTERRUPT Interrupt, ULONG MessageId)
{
    EMAC_ADAPTER *A = EmacGetInterruptContext(Interrupt)->Adapter;
    ULONG sta, hit;

    UNREFERENCED_PARAMETER(MessageId);

    sta = EMAC_RD(A, EMAC_INT_STA);
    hit = sta & A->IntEnShadow;         /* only what is armed */
    if (hit == 0) {
        return FALSE;
    }

    EMAC_WR(A, EMAC_INT_STA, hit);      /* write-1-to-clear */

    /* Disarm until the queue re-arms through SetNotificationEnabled. */
    A->IntEnShadow &= ~hit;
    EMAC_WR(A, EMAC_INT_EN, A->IntEnShadow);

    InterlockedOr(&A->IntPending, (LONG)hit);
    WdfInterruptQueueDpcForIsr(Interrupt);
    return TRUE;
}

void EmacEvtInterruptDpc(WDFINTERRUPT Interrupt, WDFOBJECT AssociatedObject)
{
    EMAC_ADAPTER *A = EmacGetInterruptContext(Interrupt)->Adapter;
    ULONG bits = (ULONG)InterlockedExchange(&A->IntPending, 0);
    EMAC_TXQUEUE *tx = A->TxQueue;
    EMAC_RXQUEUE *rx = A->RxQueue;

    UNREFERENCED_PARAMETER(AssociatedObject);

    if ((bits & EMAC_INT_TX_MASK) && tx != NULL) {
        NetTxQueueNotifyMoreCompletedPacketsAvailable(tx->Queue);
    }
    if ((bits & EMAC_INT_RX_MASK) && rx != NULL) {
        NetRxQueueNotifyMoreReceivedPacketsAvailable(rx->Queue);
    }
}

/* ======================================================================= TX */
/*
 * TX does not use framework DMA mapping. Each packet (any number of fragments) is copied into
 * one fixed-size slot of a driver-owned common buffer and sent with a single FIRST|LAST
 * descriptor, exactly the descriptor pattern EmacDxe and U-Boot use. Reason: with framework DMA
 * mapping, NetAdapterCx hit SYSTEM_PTE_MISUSE (0xDA/0x302) in NxDmaAdapter::CleanupNetPacket
 * when completing packets whose buffers the HAL had remapped. Copying also removes every
 * dependency on where (above 4 GB, unaligned, cached) the OS packet buffers live.
 */

static void TxPostPackets(EMAC_TXQUEUE *tx)
{
    EMAC_ADAPTER *A = tx->Adapter;
    NET_RING_PACKET_ITERATOR pi = NetRingGetPostPackets(tx->Rings);
    BOOLEAN posted = FALSE;

    while (NetPacketIteratorHasAny(&pi)) {
        NET_PACKET *packet = NetPacketIteratorGetPacket(&pi);

        if (!packet->Ignore) {
            EMAC_TX_PACKET *ctx = &tx->Packets[NetPacketIteratorGetIndex(&pi)];
            NET_RING_FRAGMENT_ITERATOR fi = NetPacketIteratorGetFragments(&pi);
            ULONG slot = tx->DescIndex;
            UCHAR *dst = tx->SlotVa + (SIZE_T)slot * EMAC_TX_SLOT_SIZE;
            SIZE_T total = 0;
            BOOLEAN fits = TRUE;

            while (NetFragmentIteratorHasAny(&fi)) {
                NET_FRAGMENT *frag = NetFragmentIteratorGetFragment(&fi);
                const NET_FRAGMENT_VIRTUAL_ADDRESS *va = NetExtensionGetFragmentVirtualAddress(
                    &tx->VirtualAddressExtension, NetFragmentIteratorGetIndex(&fi));
                SIZE_T len = (SIZE_T)frag->ValidLength;

                if (total + len > EMAC_TX_SLOT_SIZE) {
                    fits = FALSE;
                } else if (len != 0) {
                    RtlCopyMemory(dst + total, (const UCHAR *)va->VirtualAddress + frag->Offset, len);
                    total += len;
                }
                NetFragmentIteratorAdvance(&fi);
            }

            /* The framework may now reclaim this packet's fragments once we complete it. */
            fi.Iterator.Rings->Rings[NetRingTypeFragment]->NextIndex = NetFragmentIteratorGetIndex(&fi);

            if (fits && total != 0) {
                EMAC_DESC *d = &tx->Desc[slot].Desc;

                d->BufferAddress = tx->SlotBus + slot * EMAC_TX_SLOT_SIZE;
                d->Control = (ULONG)total | EMAC_DESC_FIRST | EMAC_DESC_LAST | EMAC_DESC_CHAIN;
                d->Status = 0;
                KeMemoryBarrier();             /* payload and descriptor visible before OWN */
                d->Status = EMAC_DESC_OWN;

                ctx->FirstDesc = slot;
                ctx->NumDesc = 1;
                tx->DescIndex = (slot + 1) % tx->NumDesc;
                posted = TRUE;
            } else {
                /* Empty or oversized: nothing to send, completed in order without a descriptor. */
                EMAC_LOG("TX packet dropped (%Iu bytes)\n", total);
                ctx->FirstDesc = 0;
                ctx->NumDesc = 0;
            }
        }
        NetPacketIteratorAdvance(&pi);
    }
    NetPacketIteratorSet(&pi);

    if (posted) {
        KeMemoryBarrier();
        WdfSpinLockAcquire(A->Lock);
        EMAC_OR(A, EMAC_TX_CTL1, EMAC_TX_CTL1_TX_DMA_START);
        WdfSpinLockRelease(A->Lock);
    }
}

static void TxCompletePackets(EMAC_TXQUEUE *tx)
{
    NET_RING_PACKET_ITERATOR pi = NetRingGetDrainPackets(tx->Rings);

    while (NetPacketIteratorHasAny(&pi)) {
        NET_PACKET *packet = NetPacketIteratorGetPacket(&pi);

        if (!packet->Ignore) {
            EMAC_TX_PACKET *ctx = &tx->Packets[NetPacketIteratorGetIndex(&pi)];

            if (ctx->NumDesc != 0) {
                EMAC_DESC *d = &tx->Desc[ctx->FirstDesc].Desc;

                if (d->Status & EMAC_DESC_OWN) {
                    break;                      /* hardware still sending it */
                }
                KeMemoryBarrier();
                d->Status = 0;
                ctx->NumDesc = 0;
            }

            {
                NET_RING_FRAGMENT_ITERATOR fi = NetPacketIteratorGetFragments(&pi);
                while (NetFragmentIteratorHasAny(&fi)) {
                    NetFragmentIteratorAdvance(&fi);
                }
                fi.Iterator.Rings->Rings[NetRingTypeFragment]->BeginIndex = NetFragmentIteratorGetIndex(&fi);
            }
        }
        NetPacketIteratorAdvance(&pi);
    }
    NetPacketIteratorSet(&pi);
}

static void EvtTxQueueAdvance(NETPACKETQUEUE Queue)
{
    EMAC_TXQUEUE *tx = EmacGetTxQueueContext(Queue);
    TxPostPackets(tx);
    TxCompletePackets(tx);
}

static void EvtTxQueueSetNotificationEnabled(NETPACKETQUEUE Queue, BOOLEAN Enabled)
{
    EMAC_TXQUEUE *tx = EmacGetTxQueueContext(Queue);
    if (Enabled) {
        EmacSetIntMask(tx->Adapter, EMAC_INT_TX_MASK, 0);
    } else {
        EmacSetIntMask(tx->Adapter, 0, EMAC_INT_TX_MASK);
    }
}

static void EvtTxQueueDestroy(WDFOBJECT Object)
{
    EMAC_TXQUEUE *tx = EmacGetTxQueueContext(Object);
    if (tx->DescBuffer != NULL) {
        WdfObjectDelete(tx->DescBuffer);
        tx->DescBuffer = NULL;
    }
    if (tx->SlotBuffer != NULL) {
        WdfObjectDelete(tx->SlotBuffer);
        tx->SlotBuffer = NULL;
    }
    if (tx->Packets != NULL) {
        ExFreePoolWithTag(tx->Packets, EMAC_POOL_TAG);
        tx->Packets = NULL;
    }
}

static void EvtTxQueueCancel(NETPACKETQUEUE Queue)
{
    /* Nothing to abort in hardware; the queue keeps draining until the OS has everything back. */
    UNREFERENCED_PARAMETER(Queue);
}

static void EvtTxQueueStart(NETPACKETQUEUE Queue)
{
    EMAC_TXQUEUE *tx = EmacGetTxQueueContext(Queue);
    EMAC_ADAPTER *A = tx->Adapter;
    ULONG i;

    EMAC_LOG("TX queue start\n");
    RtlZeroMemory(tx->Desc, (SIZE_T)tx->NumDesc * sizeof(EMAC_DESC_SLOT));
    for (i = 0; i < tx->NumDesc; i++) {
        tx->Desc[i].Desc.Next = tx->DescBus + ((i + 1) % tx->NumDesc) * EMAC_DESC_STRIDE;
    }
    RtlZeroMemory(tx->Packets, sizeof(EMAC_TX_PACKET) * tx->NumPackets);
    tx->DescIndex = 0;
    KeMemoryBarrier();

    A->TxQueue = tx;

    WdfSpinLockAcquire(A->Lock);
    EMAC_WR(A, EMAC_TX_DESC_LIST, tx->DescBus);
    EMAC_OR(A, EMAC_TX_CTL1, EMAC_TX_CTL1_TX_MD | EMAC_TX_CTL1_TX_DMA_EN);
    EMAC_OR(A, EMAC_TX_CTL0, EMAC_TX_CTL0_TX_EN);
    A->TxRunning = TRUE;
    WdfSpinLockRelease(A->Lock);
}

static void EvtTxQueueStop(NETPACKETQUEUE Queue)
{
    EMAC_TXQUEUE *tx = EmacGetTxQueueContext(Queue);
    EMAC_ADAPTER *A = tx->Adapter;

    EMAC_LOG("TX queue stop\n");
    EmacSetIntMask(A, 0, EMAC_INT_TX_MASK);

    WdfSpinLockAcquire(A->Lock);
    EMAC_AND(A, EMAC_TX_CTL0, ~EMAC_TX_CTL0_TX_EN);
    EMAC_AND(A, EMAC_TX_CTL1, ~EMAC_TX_CTL1_TX_DMA_EN);
    A->TxRunning = FALSE;
    WdfSpinLockRelease(A->Lock);

    A->TxQueue = NULL;
    KeFlushQueuedDpcs();
}

NTSTATUS EmacEvtCreateTxQueue(NETADAPTER NetAdapter, NETTXQUEUE_INIT *Init)
{
    EMAC_ADAPTER *A = EmacGetAdapterContext(NetAdapter);
    WDF_OBJECT_ATTRIBUTES attrs;
    NET_PACKET_QUEUE_CONFIG cfg;
    NETPACKETQUEUE queue;
    EMAC_TXQUEUE *tx;
    NET_EXTENSION_QUERY q;
    NET_RING *packetRing;
    WDF_COMMON_BUFFER_CONFIG cbCfg;
    PHYSICAL_ADDRESS pa;
    NTSTATUS status;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, EMAC_TXQUEUE);
    attrs.EvtDestroyCallback = EvtTxQueueDestroy;
    NET_PACKET_QUEUE_CONFIG_INIT(&cfg, EvtTxQueueAdvance, EvtTxQueueSetNotificationEnabled, EvtTxQueueCancel);
    cfg.EvtStart = EvtTxQueueStart;
    cfg.EvtStop = EvtTxQueueStop;

    EMAC_LOG("EvtCreateTxQueue\n");
    status = NetTxQueueCreate(Init, &attrs, &cfg, &queue);
    if (!NT_SUCCESS(status)) { EMAC_LOG("NetTxQueueCreate failed 0x%08lx\n", status); return status; }

    tx = EmacGetTxQueueContext(queue);
    tx->Adapter = A;
    tx->Queue = queue;
    tx->Rings = NetTxQueueGetRingCollection(queue);

    NET_EXTENSION_QUERY_INIT(&q, NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_NAME,
                             NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1, NetExtensionTypeFragment);
    NetTxQueueGetExtension(queue, &q, &tx->VirtualAddressExtension);

    /* One descriptor and one slot per packet-ring element. */
    packetRing = NetRingCollectionGetPacketRing(tx->Rings);
    tx->NumPackets = packetRing->NumberOfElements;
    tx->NumDesc = tx->NumPackets;
    EMAC_LOG("TX queue: %lu packets (one descriptor + %d-byte slot each)\n", tx->NumDesc, EMAC_TX_SLOT_SIZE);

    /* Plain pool memory, freed in EvtTxQueueDestroy. */
    tx->Packets = (EMAC_TX_PACKET *)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                    sizeof(EMAC_TX_PACKET) * tx->NumPackets,
                                                    EMAC_POOL_TAG);
    if (tx->Packets == NULL) {
        EMAC_LOG("TX packet context allocation failed\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Common buffers are always parented to the DMA enabler (a ParentObject is rejected with
     * 0xC020020F); they are deleted in the queue destroy callback. */
    WDF_COMMON_BUFFER_CONFIG_INIT(&cbCfg, FILE_128_BYTE_ALIGNMENT);
    status = WdfCommonBufferCreateWithConfig(A->DmaEnabler, (SIZE_T)tx->NumDesc * sizeof(EMAC_DESC_SLOT),
                                             &cbCfg, WDF_NO_OBJECT_ATTRIBUTES, &tx->DescBuffer);
    if (!NT_SUCCESS(status)) { EMAC_LOG("TX descriptor buffer failed 0x%08lx\n", status); return status; }
    tx->Desc = (EMAC_DESC_SLOT *)WdfCommonBufferGetAlignedVirtualAddress(tx->DescBuffer);
    pa = WdfCommonBufferGetAlignedLogicalAddress(tx->DescBuffer);
    if ((pa.QuadPart >> 32) != 0) {
        EMAC_LOG("TX descriptors above 4 GB (0x%I64x): 32-bit DMA mask not honored\n", pa.QuadPart);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    tx->DescBus = pa.LowPart;

    WDF_COMMON_BUFFER_CONFIG_INIT(&cbCfg, FILE_128_BYTE_ALIGNMENT);
    status = WdfCommonBufferCreateWithConfig(A->DmaEnabler, (SIZE_T)tx->NumDesc * EMAC_TX_SLOT_SIZE,
                                             &cbCfg, WDF_NO_OBJECT_ATTRIBUTES, &tx->SlotBuffer);
    if (!NT_SUCCESS(status)) { EMAC_LOG("TX slot buffer failed 0x%08lx\n", status); return status; }
    tx->SlotVa = (UCHAR *)WdfCommonBufferGetAlignedVirtualAddress(tx->SlotBuffer);
    pa = WdfCommonBufferGetAlignedLogicalAddress(tx->SlotBuffer);
    if ((pa.QuadPart >> 32) != 0 || ((pa.QuadPart + (LONGLONG)tx->NumDesc * EMAC_TX_SLOT_SIZE) >> 32) != 0) {
        EMAC_LOG("TX slots above 4 GB (0x%I64x): 32-bit DMA mask not honored\n", pa.QuadPart);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    tx->SlotBus = pa.LowPart;
    return STATUS_SUCCESS;
}

/* ======================================================================= RX */

static void RxPostDescriptor(EMAC_RXQUEUE *rx, ULONG Index)
{
    NET_FRAGMENT *frag = NetRingGetFragmentAtIndex(
        NetRingCollectionGetFragmentRing(rx->Rings), Index);
    const NET_FRAGMENT_LOGICAL_ADDRESS *la =
        NetExtensionGetFragmentLogicalAddress(&rx->LogicalAddressExtension, Index);
    EMAC_DESC *d = &rx->Desc[Index].Desc;
    ULONG size = (ULONG)frag->Capacity;

    if ((la->LogicalAddress >> 32) != 0 || ((la->LogicalAddress + size) >> 32) != 0) {
        /* Never truncate: a wrong DMA address would write into unrelated memory. */
        EMAC_LOG("RX buffer %lu above 4 GB (0x%I64x), not posted\n", Index, (UINT64)la->LogicalAddress);
        d->Status = 0;
        d->Control = 0;
        return;
    }

    if (size > EMAC_RX_SIZE_MAX) {
        size = EMAC_RX_SIZE_MAX;
    }
    size &= ~3UL;                       /* hardware size granularity is 4 bytes */

    d->BufferAddress = (ULONG)la->LogicalAddress;
    d->Control = size;
    KeMemoryBarrier();
    d->Status = EMAC_DESC_OWN;
}

static void RxPostBuffers(EMAC_RXQUEUE *rx)
{
    NET_RING_FRAGMENT_ITERATOR fi = NetRingGetPostFragments(rx->Rings);
    BOOLEAN posted = FALSE;

    while (NetFragmentIteratorHasAny(&fi)) {
        RxPostDescriptor(rx, NetFragmentIteratorGetIndex(&fi));
        NetFragmentIteratorAdvance(&fi);
        posted = TRUE;
    }
    NetFragmentIteratorSet(&fi);

    if (posted && rx->Adapter->RxRunning) {
        EMAC_ADAPTER *A = rx->Adapter;
        KeMemoryBarrier();
        WdfSpinLockAcquire(A->Lock);
        EMAC_OR(A, EMAC_RX_CTL1, EMAC_RX_CTL1_RX_DMA_START);   /* restart if it ran out of buffers */
        WdfSpinLockRelease(A->Lock);
    }
}

static void RxIndicateReceives(EMAC_RXQUEUE *rx)
{
    NET_RING_FRAGMENT_ITERATOR fi = NetRingGetDrainFragments(rx->Rings);
    NET_RING_PACKET_ITERATOR pi = NetRingGetAllPackets(rx->Rings);

    while (NetFragmentIteratorHasAny(&fi) && NetPacketIteratorHasAny(&pi)) {
        ULONG index = NetFragmentIteratorGetIndex(&fi);
        EMAC_DESC *d = &rx->Desc[index].Desc;
        NET_FRAGMENT *frag;
        NET_PACKET *packet;
        ULONG status;
        ULONG len;

        if (d->Status & EMAC_DESC_OWN) {
            break;
        }
        KeMemoryBarrier();
        status = d->Status;
        len = EMAC_RX_STATUS_LEN(status);       /* includes FCS */

        frag = NetFragmentIteratorGetFragment(&fi);
        packet = NetPacketIteratorGetPacket(&pi);
        packet->FragmentIndex = index;
        packet->FragmentCount = 1;
        frag->Offset = 0;

        if ((status & EMAC_RX_STATUS_ERR_MASK) != 0 ||
            len < EMAC_MIN_RX_FRAME ||
            len > frag->Capacity) {
            /* EmacDxe accepts runts/errors in hardware and drops them in software; so do we. */
            frag->ValidLength = 0;
            packet->Ignore = 1;
        } else {
            frag->ValidLength = len - EMAC_FCS_SIZE;
            packet->Ignore = 0;
        }

        NetFragmentIteratorAdvance(&fi);
        NetPacketIteratorAdvance(&pi);
    }
    NetFragmentIteratorSet(&fi);
    NetPacketIteratorSet(&pi);
}

static void EvtRxQueueAdvance(NETPACKETQUEUE Queue)
{
    EMAC_RXQUEUE *rx = EmacGetRxQueueContext(Queue);
    RxIndicateReceives(rx);
    RxPostBuffers(rx);
}

static void EvtRxQueueSetNotificationEnabled(NETPACKETQUEUE Queue, BOOLEAN Enabled)
{
    EMAC_RXQUEUE *rx = EmacGetRxQueueContext(Queue);
    if (Enabled) {
        EmacSetIntMask(rx->Adapter, EMAC_INT_RX_MASK, 0);
    } else {
        EmacSetIntMask(rx->Adapter, 0, EMAC_INT_RX_MASK);
    }
}

static void EvtRxQueueDestroy(WDFOBJECT Object)
{
    EMAC_RXQUEUE *rx = EmacGetRxQueueContext(Object);
    if (rx->DescBuffer != NULL) {
        WdfObjectDelete(rx->DescBuffer);
        rx->DescBuffer = NULL;
    }
}

static void RxDisableHardware(EMAC_ADAPTER *A)
{
    WdfSpinLockAcquire(A->Lock);
    EMAC_AND(A, EMAC_RX_CTL0, ~EMAC_RX_CTL0_RX_EN);
    EMAC_AND(A, EMAC_RX_CTL1, ~EMAC_RX_CTL1_RX_DMA_EN);
    A->RxRunning = FALSE;
    WdfSpinLockRelease(A->Lock);
}

static void EvtRxQueueCancel(NETPACKETQUEUE Queue)
{
    EMAC_RXQUEUE *rx = EmacGetRxQueueContext(Queue);

    RxDisableHardware(rx->Adapter);

    /* Grab anything already received, then hand every remaining buffer back as ignored. */
    RxIndicateReceives(rx);

    {
        NET_RING_PACKET_ITERATOR pi = NetRingGetAllPackets(rx->Rings);
        while (NetPacketIteratorHasAny(&pi)) {
            NetPacketIteratorGetPacket(&pi)->Ignore = 1;
            NetPacketIteratorAdvance(&pi);
        }
        NetPacketIteratorSet(&pi);
    }
    {
        NET_RING_FRAGMENT_ITERATOR fi = NetRingGetAllFragments(rx->Rings);
        NetFragmentIteratorAdvanceToTheEnd(&fi);
        NetFragmentIteratorSet(&fi);
    }
}

static void EvtRxQueueStart(NETPACKETQUEUE Queue)
{
    EMAC_RXQUEUE *rx = EmacGetRxQueueContext(Queue);
    EMAC_ADAPTER *A = rx->Adapter;
    NET_RING *fragRing = NetRingCollectionGetFragmentRing(rx->Rings);
    ULONG startIndex = fragRing->BeginIndex;
    ULONG i;

    EMAC_LOG("RX queue start\n");
    RtlZeroMemory(rx->Desc, (SIZE_T)rx->NumDesc * sizeof(EMAC_DESC_SLOT));
    for (i = 0; i < rx->NumDesc; i++) {
        rx->Desc[i].Desc.Next = rx->DescBus + ((i + 1) % rx->NumDesc) * EMAC_DESC_STRIDE;
    }
    KeMemoryBarrier();

    A->RxQueue = rx;

    /* Give the hardware every buffer the OS has already provided. */
    {
        NET_RING_FRAGMENT_ITERATOR fi = NetRingGetPostFragments(rx->Rings);
        while (NetFragmentIteratorHasAny(&fi)) {
            RxPostDescriptor(rx, NetFragmentIteratorGetIndex(&fi));
            NetFragmentIteratorAdvance(&fi);
        }
        NetFragmentIteratorSet(&fi);
    }

    WdfSpinLockAcquire(A->Lock);
    EMAC_WR(A, EMAC_RX_DESC_LIST, rx->DescBus + startIndex * EMAC_DESC_STRIDE);
    EMAC_OR(A, EMAC_RX_CTL1,
            EMAC_RX_CTL1_RX_MD | EMAC_RX_CTL1_RX_RUNT | EMAC_RX_CTL1_RX_ERR |
            EMAC_RX_CTL1_RX_DMA_EN | EMAC_RX_CTL1_RX_DMA_START);
    EMAC_OR(A, EMAC_RX_CTL0, EMAC_RX_CTL0_RX_EN);
    A->RxRunning = TRUE;
    WdfSpinLockRelease(A->Lock);
}

static void EvtRxQueueStop(NETPACKETQUEUE Queue)
{
    EMAC_RXQUEUE *rx = EmacGetRxQueueContext(Queue);

    EmacSetIntMask(rx->Adapter, 0, EMAC_INT_RX_MASK);
    RxDisableHardware(rx->Adapter);
    rx->Adapter->RxQueue = NULL;
    KeFlushQueuedDpcs();
}

NTSTATUS EmacEvtCreateRxQueue(NETADAPTER NetAdapter, NETRXQUEUE_INIT *Init)
{
    EMAC_ADAPTER *A = EmacGetAdapterContext(NetAdapter);
    WDF_OBJECT_ATTRIBUTES attrs;
    NET_PACKET_QUEUE_CONFIG cfg;
    NETPACKETQUEUE queue;
    EMAC_RXQUEUE *rx;
    NET_EXTENSION_QUERY q;
    NET_RING *fragRing;
    WDF_COMMON_BUFFER_CONFIG cbCfg;
    PHYSICAL_ADDRESS pa;
    NTSTATUS status;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, EMAC_RXQUEUE);
    attrs.EvtDestroyCallback = EvtRxQueueDestroy;
    NET_PACKET_QUEUE_CONFIG_INIT(&cfg, EvtRxQueueAdvance, EvtRxQueueSetNotificationEnabled, EvtRxQueueCancel);
    cfg.EvtStart = EvtRxQueueStart;
    cfg.EvtStop = EvtRxQueueStop;

    EMAC_LOG("EvtCreateRxQueue\n");
    status = NetRxQueueCreate(Init, &attrs, &cfg, &queue);
    if (!NT_SUCCESS(status)) { EMAC_LOG("NetRxQueueCreate failed 0x%08lx\n", status); return status; }

    rx = EmacGetRxQueueContext(queue);
    rx->Adapter = A;
    rx->Queue = queue;
    rx->Rings = NetRxQueueGetRingCollection(queue);

    NET_EXTENSION_QUERY_INIT(&q, NET_FRAGMENT_EXTENSION_LOGICAL_ADDRESS_NAME,
                             NET_FRAGMENT_EXTENSION_LOGICAL_ADDRESS_VERSION_1, NetExtensionTypeFragment);
    NetRxQueueGetExtension(queue, &q, &rx->LogicalAddressExtension);

    fragRing = NetRingCollectionGetFragmentRing(rx->Rings);
    rx->NumDesc = fragRing->NumberOfElements;
    EMAC_LOG("RX queue: %lu fragments\n", rx->NumDesc);

    WDF_COMMON_BUFFER_CONFIG_INIT(&cbCfg, FILE_128_BYTE_ALIGNMENT);
    status = WdfCommonBufferCreateWithConfig(A->DmaEnabler, (SIZE_T)rx->NumDesc * sizeof(EMAC_DESC_SLOT),
                                             &cbCfg, WDF_NO_OBJECT_ATTRIBUTES, &rx->DescBuffer);
    if (!NT_SUCCESS(status)) { EMAC_LOG("RX common buffer failed 0x%08lx\n", status); return status; }

    rx->Desc = (EMAC_DESC_SLOT *)WdfCommonBufferGetAlignedVirtualAddress(rx->DescBuffer);
    pa = WdfCommonBufferGetAlignedLogicalAddress(rx->DescBuffer);
    if ((pa.QuadPart >> 32) != 0) {
        EMAC_LOG("RX descriptors above 4 GB (0x%I64x): 32-bit DMA mask not honored\n", pa.QuadPart);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    rx->DescBus = pa.LowPart;
    return STATUS_SUCCESS;
}
