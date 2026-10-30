/*
 * Orange Pi Zero3 (Allwinner H616/H618) EMAC0 - Windows 11 ARM64 NetAdapterCx (KMDF) driver.
 *
 * Structure follows raspberrypi/windows-drivers bcmgenet (MIT) and Microsoft's RtEthSample
 * (MIT). Hardware sequences come from the UEFI EmacDxe driver for this board.
 *
 * What ACPI already did before D0Entry runs (see acpi/DSDT_minimal.dsl, power resource EPWR):
 * EMAC0 bus clock gated on, reset released, syscon RGMII/TX-delay set, PI pins muxed.
 */
#include "emac.h"

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD EmacEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE EmacEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE EmacEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY EmacEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT EmacEvtD0Exit;
EVT_WDF_TIMER EmacEvtLinkTimer;

/* ================================================================== MAC address */

/* Identical to EmacGenerateMac() in EmacDxe.c so UEFI/PXE and Windows agree. */
static void EmacGenerateMac(EMAC_ADAPTER *A, UCHAR Mac[EMAC_MAC_LEN])
{
    ULONG sid[4];
    ULONG i;

    for (i = 0; i < 4; i++) {
        sid[i] = READ_REGISTER_ULONG((volatile ULONG *)(A->Sid + i * 4));
    }

    if ((sid[0] | sid[1] | sid[2] | sid[3]) == 0) {
        static const UCHAR fallback[EMAC_MAC_LEN] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x03 };
        RtlCopyMemory(Mac, fallback, EMAC_MAC_LEN);
        return;
    }

    if ((sid[3] & 0xFFFFFF) == 0) {
        sid[3] |= 0x800000;
    }

    Mac[0] = 0x02;
    Mac[1] = (UCHAR)(sid[0] >> 0);
    Mac[2] = (UCHAR)(sid[3] >> 24);
    Mac[3] = (UCHAR)(sid[3] >> 16);
    Mac[4] = (UCHAR)(sid[3] >> 8);
    Mac[5] = (UCHAR)(sid[3] >> 0);
}

static void EmacWriteAddrSlot(EMAC_ADAPTER *A, ULONG Slot, const UCHAR *Mac, BOOLEAN Enable)
{
    ULONG lo = (ULONG)Mac[0] | ((ULONG)Mac[1] << 8) | ((ULONG)Mac[2] << 16) | ((ULONG)Mac[3] << 24);
    ULONG hi = (ULONG)Mac[4] | ((ULONG)Mac[5] << 8);

    if (Enable) {
        hi |= EMAC_ADDR_HIGH_ENABLE;
    }
    EMAC_WR(A, EMAC_ADDR_LOW(Slot), lo);
    EMAC_WR(A, EMAC_ADDR_HIGH(Slot), hi);
}

/* ================================================================== filters / link */

/* Caller holds A->Lock. Logic mirrors sun8i_dwmac_set_filter() in Linux (register facts only). */
void EmacMacProgramFilters(EMAC_ADAPTER *A)
{
    ULONG flt = EMAC_FLT_CTL;
    ULONG used = 1;                     /* slot 0 is the station address */
    ULONG slot;

    if (A->PacketFilter & NetPacketFilterFlagPromiscuous) {
        flt = EMAC_FLT_RXALL;
    } else if (A->PacketFilter & NetPacketFilterFlagAllMulticast) {
        flt |= EMAC_FLT_ALLMULTI;
    } else if ((A->PacketFilter & NetPacketFilterFlagMulticast) && A->McastCount != 0) {
        if (A->McastCount <= EMAC_MAX_MULTICAST) {
            for (ULONG i = 0; i < A->McastCount; i++) {
                EmacWriteAddrSlot(A, used++, A->Mcast[i].Address, TRUE);
            }
        } else {
            flt |= EMAC_FLT_ALLMULTI;   /* too many addresses for the perfect filter */
        }
    }

    for (slot = used; slot < EMAC_ADDR_SLOTS; slot++) {
        EMAC_WR(A, EMAC_ADDR_HIGH(slot), 0);
    }
    EMAC_WR(A, EMAC_RX_FRM_FLT, flt);
}

/* Caller holds A->Lock. TX/RX are paused while speed/duplex change. */
void EmacMacApplyLink(EMAC_ADAPTER *A)
{
    ULONG ctl0;
    BOOLEAN tx = A->TxRunning;
    BOOLEAN rx = A->RxRunning;

    if (tx) EMAC_AND(A, EMAC_TX_CTL0, ~EMAC_TX_CTL0_TX_EN);
    if (rx) EMAC_AND(A, EMAC_RX_CTL0, ~EMAC_RX_CTL0_RX_EN);

    ctl0 = EMAC_RD(A, EMAC_CTL0) & ~(EMAC_CTL0_FULL_DUPLEX | EMAC_CTL0_SPEED_MASK);
    if (A->FullDuplex) {
        ctl0 |= EMAC_CTL0_FULL_DUPLEX;
    }
    switch (A->LinkSpeedMbps) {
    case 10:   ctl0 |= EMAC_CTL0_SPEED_10;   break;
    case 100:  ctl0 |= EMAC_CTL0_SPEED_100;  break;
    default:   ctl0 |= EMAC_CTL0_SPEED_1000; break;
    }
    EMAC_WR(A, EMAC_CTL0, ctl0);

    if (rx) EMAC_OR(A, EMAC_RX_CTL0, EMAC_RX_CTL0_RX_EN);
    if (tx) EMAC_OR(A, EMAC_TX_CTL0, EMAC_TX_CTL0_TX_EN);
}

void EmacEvtLinkTimer(WDFTIMER Timer)
{
    EMAC_ADAPTER *A = EmacGetTimerContext(Timer)->Adapter;
    BOOLEAN up = FALSE, full = FALSE, changed;
    ULONG speed = 0;
    NTSTATUS status;
    NET_ADAPTER_LINK_STATE state;

    status = EmacPhyReadLink(A, &up, &speed, &full);
    if (!NT_SUCCESS(status)) {
        up = FALSE;
    }

    WdfSpinLockAcquire(A->Lock);
    changed = (up != A->LinkUp) || (up && (speed != A->LinkSpeedMbps || full != A->FullDuplex));
    if (changed) {
        A->LinkUp = up;
        if (up) {
            A->LinkSpeedMbps = speed;
            A->FullDuplex = full;
            EmacMacApplyLink(A);
        }
    }
    WdfSpinLockRelease(A->Lock);

    if (!changed) {
        return;
    }

    if (up) {
        NET_ADAPTER_LINK_STATE_INIT(&state, (UINT64)speed * 1000000ULL, MediaConnectStateConnected,
                                    full ? MediaDuplexStateFull : MediaDuplexStateHalf,
                                    NetAdapterPauseFunctionTypeUnsupported,
                                    NetAdapterAutoNegotiationFlagXmitLinkSpeedAutoNegotiated |
                                    NetAdapterAutoNegotiationFlagRcvLinkSpeedautoNegotiated |
                                    NetAdapterAutoNegotiationFlagDuplexAutoNegotiated);
        EMAC_LOG("link up %lu Mbps %s duplex\n", speed, full ? "full" : "half");
    } else {
        NET_ADAPTER_LINK_STATE_INIT_DISCONNECTED(&state);
        EMAC_LOG("link down\n");
    }
    NetAdapterSetLinkState(A->NetAdapter, &state);
}

/* Called by NetAdapterCx when the packet filter or multicast list changes. */
static void EmacEvtReceiveFilter(NETADAPTER NetAdapter, NETRECEIVEFILTER Filter)
{
    EMAC_ADAPTER *A = EmacGetAdapterContext(NetAdapter);
    SIZE_T count = NetReceiveFilterGetMulticastAddressCount(Filter);
    const NET_ADAPTER_LINK_LAYER_ADDRESS *list = NetReceiveFilterGetMulticastAddressList(Filter);

    WdfSpinLockAcquire(A->Lock);
    A->PacketFilter = NetReceiveFilterGetPacketFilter(Filter);
    if (count > EMAC_MAX_MULTICAST) {
        count = EMAC_MAX_MULTICAST + 1;     /* forces the all-multicast fallback */
    }
    A->McastCount = (ULONG)count;
    if (count != 0 && count <= EMAC_MAX_MULTICAST) {
        RtlCopyMemory(A->Mcast, list, count * sizeof(A->Mcast[0]));
    }
    EmacMacProgramFilters(A);
    WdfSpinLockRelease(A->Lock);
}

/* ================================================================== adapter */

static NTSTATUS EmacAdapterStart(EMAC_ADAPTER *A)
{
    NET_ADAPTER_LINK_STATE link;
    NET_ADAPTER_LINK_LAYER_CAPABILITIES llc;
    NET_ADAPTER_DMA_CAPABILITIES rxDma;
    NET_ADAPTER_TX_CAPABILITIES txCaps;
    NET_ADAPTER_RX_CAPABILITIES rxCaps;
    NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES rfCaps;

    NET_ADAPTER_LINK_STATE_INIT_DISCONNECTED(&link);
    NetAdapterSetLinkState(A->NetAdapter, &link);

    NET_ADAPTER_LINK_LAYER_CAPABILITIES_INIT(&llc, EMAC_MAX_LINK_SPEED, EMAC_MAX_LINK_SPEED);
    NetAdapterSetLinkLayerCapabilities(A->NetAdapter, &llc);
    NetAdapterSetLinkLayerMtuSize(A->NetAdapter, EMAC_MTU);

    /* TX copies into driver-owned slots, so it declares no DMA mapping of its own. */
    NET_ADAPTER_TX_CAPABILITIES_INIT(&txCaps, 1);
    txCaps.MaximumNumberOfFragments = EMAC_TX_MAX_FRAGMENTS;
    txCaps.FragmentRingNumberOfElementsHint = EMAC_TX_RING_HINT;

    NET_ADAPTER_DMA_CAPABILITIES_INIT(&rxDma, A->DmaEnabler);
    /* The MAC only takes 32-bit buffer addresses. The default is "no limit", which would let the
     * framework allocate receive buffers above 4 GB (the Zero3 can have 4 GB of RAM). */
    rxDma.MaximumPhysicalAddress.QuadPart = 0xFFFFFFFFLL;
    NET_ADAPTER_RX_CAPABILITIES_INIT_SYSTEM_MANAGED_DMA(&rxCaps, &rxDma, EMAC_RX_BUFFER_MAX, 1);
    rxCaps.FragmentBufferAlignment = EMAC_RX_BUFFER_ALIGN;
    rxCaps.FragmentRingNumberOfElementsHint = EMAC_RX_RING_HINT;
    NetAdapterSetDataPathCapabilities(A->NetAdapter, &txCaps, &rxCaps);

    NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES_INIT(&rfCaps, EmacEvtReceiveFilter);
    rfCaps.SupportedPacketFilters = EMAC_SUPPORTED_FILTERS;
    rfCaps.MaximumMulticastAddresses = EMAC_MAX_MULTICAST;
    NetAdapterSetReceiveFilterCapabilities(A->NetAdapter, &rfCaps);

    return NetAdapterStart(A->NetAdapter);
}

/* ================================================================== PnP / power */

NTSTATUS EmacEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    EMAC_ADAPTER *A = EmacGetDeviceContext(Device)->Adapter;
    ULONG count = WdfCmResourceListGetCount(Translated);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR irqRaw = NULL, irqTr = NULL;
    WDF_INTERRUPT_CONFIG irqCfg;
    WDF_OBJECT_ATTRIBUTES irqAttrs;
    NTSTATUS status;
    UCHAR mac[EMAC_MAC_LEN];
    static const UNICODE_STRING phyKey = RTL_CONSTANT_STRING(L"PhyAddress");
    ULONG phyAddr;

    for (ULONG i = 0; i < count; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(Raw, i);
        PCM_PARTIAL_RESOURCE_DESCRIPTOR t = WdfCmResourceListGetDescriptor(Translated, i);

        if (t->Type == CmResourceTypeMemory) {
            if (t->u.Memory.Length == EMAC_REG_SIZE && A->Regs == NULL) {
                A->Regs = MmMapIoSpaceEx(t->u.Memory.Start, EMAC_REG_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
            } else if (t->u.Memory.Length == SID_REG_SIZE && A->Sid == NULL) {
                A->Sid = MmMapIoSpaceEx(t->u.Memory.Start, SID_REG_SIZE, PAGE_READONLY | PAGE_NOCACHE);
            }
        } else if (t->Type == CmResourceTypeInterrupt && irqTr == NULL) {
            irqRaw = r;
            irqTr = t;
        }
    }

    if (A->Regs == NULL || A->Sid == NULL || irqTr == NULL) {
        EMAC_LOG("missing resources: regs=%p sid=%p irq=%p\n", A->Regs, A->Sid, irqTr);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    EMAC_LOG("mapped regs=%p sid=%p irq vector=%lu\n", A->Regs, A->Sid, irqTr->u.Interrupt.Vector);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&irqAttrs, EMAC_INTERRUPT_CONTEXT);
    WDF_INTERRUPT_CONFIG_INIT(&irqCfg, EmacEvtInterruptIsr, EmacEvtInterruptDpc);
    irqCfg.InterruptRaw = irqRaw;
    irqCfg.InterruptTranslated = irqTr;
    status = WdfInterruptCreate(Device, &irqCfg, &irqAttrs, &A->Interrupt);
    if (!NT_SUCCESS(status)) return status;
    EmacGetInterruptContext(A->Interrupt)->Adapter = A;

    A->PhyAddress = EMAC_DEFAULT_PHY_ADDRESS;
    if (NT_SUCCESS(NetConfigurationQueryUlong(A->Config, NET_CONFIGURATION_QUERY_ULONG_NO_FLAGS,
                                              &phyKey, &phyAddr)) && phyAddr < 32) {
        A->PhyAddress = phyAddr;
    }

    EmacGenerateMac(A, mac);
    A->PermanentAddress.Length = EMAC_MAC_LEN;
    RtlCopyMemory(A->PermanentAddress.Address, mac, EMAC_MAC_LEN);

    status = NetConfigurationQueryLinkLayerAddress(A->Config, &A->CurrentAddress);
    if (!NT_SUCCESS(status) || A->CurrentAddress.Length != EMAC_MAC_LEN ||
        (A->CurrentAddress.Address[0] & 1) != 0) {
        A->CurrentAddress = A->PermanentAddress;
    }
    NetAdapterSetPermanentLinkLayerAddress(A->NetAdapter, &A->PermanentAddress);
    NetAdapterSetCurrentLinkLayerAddress(A->NetAdapter, &A->CurrentAddress);
    EMAC_LOG("MAC %02x:%02x:%02x:%02x:%02x:%02x, PHY address %lu\n",
             A->CurrentAddress.Address[0], A->CurrentAddress.Address[1], A->CurrentAddress.Address[2],
             A->CurrentAddress.Address[3], A->CurrentAddress.Address[4], A->CurrentAddress.Address[5],
             A->PhyAddress);

    if (!A->AdapterStarted) {
        status = EmacAdapterStart(A);
        EMAC_LOG("NetAdapterStart -> 0x%08lx\n", status);
        if (!NT_SUCCESS(status)) return status;
        A->AdapterStarted = TRUE;
    }
    return STATUS_SUCCESS;
}

NTSTATUS EmacEvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    EMAC_ADAPTER *A = EmacGetDeviceContext(Device)->Adapter;

    UNREFERENCED_PARAMETER(Translated);

    EMAC_LOG("ReleaseHardware\n");
    if (A->Regs != NULL) {
        MmUnmapIoSpace((PVOID)A->Regs, EMAC_REG_SIZE);
        A->Regs = NULL;
    }
    if (A->Sid != NULL) {
        MmUnmapIoSpace((PVOID)A->Sid, SID_REG_SIZE);
        A->Sid = NULL;
    }
    A->Interrupt = NULL;
    return STATUS_SUCCESS;
}

static NTSTATUS EmacHardwareReset(EMAC_ADAPTER *A)
{
    ULONG i;

    EMAC_AND(A, EMAC_RX_CTL0, ~EMAC_RX_CTL0_RX_EN);
    EMAC_AND(A, EMAC_TX_CTL0, ~EMAC_TX_CTL0_TX_EN);
    EMAC_AND(A, EMAC_TX_CTL1, ~EMAC_TX_CTL1_TX_DMA_EN);
    EMAC_AND(A, EMAC_RX_CTL1, ~EMAC_RX_CTL1_RX_DMA_EN);

    EMAC_WR(A, EMAC_CTL1, EMAC_CTL1_SOFT_RST);
    for (i = 0; i < 10000; i++) {           /* 100 ms like EMAC_RESET_TIMEOUT_US */
        if ((EMAC_RD(A, EMAC_CTL1) & EMAC_CTL1_SOFT_RST) == 0) {
            break;
        }
        KeStallExecutionProcessor(10);
    }
    if (i == 10000) {
        EMAC_LOG("MAC soft reset timed out (clock/reset from ACPI not applied?)\n");
        return STATUS_IO_TIMEOUT;
    }

    EMAC_WR(A, EMAC_CTL1, EMAC_CTL1_BURST_LEN << EMAC_CTL1_BURST_SHIFT);
    return STATUS_SUCCESS;
}

NTSTATUS EmacEvtD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    EMAC_ADAPTER *A = EmacGetDeviceContext(Device)->Adapter;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(PreviousState);

    status = EmacHardwareReset(A);
    if (!NT_SUCCESS(status)) return status;

    /* Interrupts off until the queues arm them. */
    WdfInterruptAcquireLock(A->Interrupt);
    A->IntEnShadow = 0;
    EMAC_WR(A, EMAC_INT_EN, 0);
    EMAC_WR(A, EMAC_INT_STA, EMAC_INT_ALL);
    WdfInterruptReleaseLock(A->Interrupt);

    WdfSpinLockAcquire(A->Lock);
    EmacWriteAddrSlot(A, 0, A->CurrentAddress.Address, FALSE);
    EmacMacProgramFilters(A);
    A->LinkUp = FALSE;
    A->TxRunning = FALSE;
    A->RxRunning = FALSE;
    WdfSpinLockRelease(A->Lock);

    status = EmacPhyInitialize(A);
    if (!NT_SUCCESS(status)) {
        EMAC_LOG("PHY init failed: 0x%08lx\n", status);
        return status;
    }

    WdfTimerStart(A->LinkTimer, 0);
    EMAC_LOG("D0Entry ok\n");
    return STATUS_SUCCESS;
}

NTSTATUS EmacEvtD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    EMAC_ADAPTER *A = EmacGetDeviceContext(Device)->Adapter;

    EMAC_LOG("D0Exit, target state %d\n", (int)TargetState);

    WdfTimerStop(A->LinkTimer, TRUE);
    if (A->Interrupt != NULL && A->Regs != NULL) {
        WdfInterruptAcquireLock(A->Interrupt);
        A->IntEnShadow = 0;
        EMAC_WR(A, EMAC_INT_EN, 0);
        WdfInterruptReleaseLock(A->Interrupt);
    }
    if (A->Regs != NULL) {
        EMAC_AND(A, EMAC_RX_CTL0, ~EMAC_RX_CTL0_RX_EN);
        EMAC_AND(A, EMAC_TX_CTL0, ~EMAC_TX_CTL0_TX_EN);
    }
    A->PhyReady = FALSE;
    return STATUS_SUCCESS;
}

/* ================================================================== DeviceAdd / DriverEntry */

NTSTATUS EmacEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES devAttrs, adapterAttrs, lockAttrs, timerAttrs;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_DMA_ENABLER_CONFIG dmaCfg;
    WDF_TIMER_CONFIG timerCfg;
    WDFDEVICE device;
    WDFTIMER timer;
    NETADAPTER_INIT *adapterInit;
    NET_ADAPTER_DATAPATH_CALLBACKS dp;
    NETADAPTER netAdapter;
    EMAC_ADAPTER *A;

    UNREFERENCED_PARAMETER(Driver);

    status = NetDeviceInitConfig(DeviceInit);
    if (!NT_SUCCESS(status)) EMAC_LOG("NetDeviceInitConfig failed 0x%08lx\n", status);
    if (!NT_SUCCESS(status)) return status;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&devAttrs, EMAC_DEVICE_CONTEXT);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = EmacEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = EmacEvtReleaseHardware;
    pnp.EvtDeviceD0Entry = EmacEvtD0Entry;
    pnp.EvtDeviceD0Exit = EmacEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    status = WdfDeviceCreate(&DeviceInit, &devAttrs, &device);
    if (!NT_SUCCESS(status)) EMAC_LOG("WdfDeviceCreate failed 0x%08lx\n", status);
    if (!NT_SUCCESS(status)) return status;

    WdfDeviceSetAlignmentRequirement(device, FILE_LONG_ALIGNMENT);

    /*
     * 32-bit DMA only: the MAC takes 32-bit descriptor and buffer addresses, and the 4 GB
     * Zero3 has RAM above 4 GB. The DMA enabler bounces anything higher.
     */
    WDF_DMA_ENABLER_CONFIG_INIT(&dmaCfg, WdfDmaProfileScatterGather, 2048);
    dmaCfg.Flags |= WDF_DMA_ENABLER_CONFIG_REQUIRE_SINGLE_TRANSFER;
    dmaCfg.WdmDmaVersionOverride = 3;

    adapterInit = NetAdapterInitAllocate(device);
    if (adapterInit == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    NET_ADAPTER_DATAPATH_CALLBACKS_INIT(&dp, EmacEvtCreateTxQueue, EmacEvtCreateRxQueue);
    NetAdapterInitSetDatapathCallbacks(adapterInit, &dp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&adapterAttrs, EMAC_ADAPTER);
    status = NetAdapterCreate(adapterInit, &adapterAttrs, &netAdapter);
    if (!NT_SUCCESS(status)) EMAC_LOG("NetAdapterCreate failed 0x%08lx\n", status);
    NetAdapterInitFree(adapterInit);
    if (!NT_SUCCESS(status)) return status;

    A = EmacGetAdapterContext(netAdapter);
    A->Device = device;
    A->NetAdapter = netAdapter;
    EmacGetDeviceContext(device)->Adapter = A;

    status = WdfDmaEnablerCreate(device, &dmaCfg, WDF_NO_OBJECT_ATTRIBUTES, &A->DmaEnabler);
    if (!NT_SUCCESS(status)) EMAC_LOG("WdfDmaEnablerCreate failed 0x%08lx\n", status);
    if (!NT_SUCCESS(status)) return status;

    status = NetAdapterOpenConfiguration(netAdapter, WDF_NO_OBJECT_ATTRIBUTES, &A->Config);
    if (!NT_SUCCESS(status)) EMAC_LOG("NetAdapterOpenConfiguration failed 0x%08lx\n", status);
    if (!NT_SUCCESS(status)) return status;

    WDF_OBJECT_ATTRIBUTES_INIT(&lockAttrs);
    lockAttrs.ParentObject = device;
    status = WdfSpinLockCreate(&lockAttrs, &A->Lock);
    if (!NT_SUCCESS(status)) EMAC_LOG("WdfSpinLockCreate failed 0x%08lx\n", status);
    if (!NT_SUCCESS(status)) return status;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&timerAttrs, EMAC_TIMER_CONTEXT);
    timerAttrs.ParentObject = device;
    WDF_TIMER_CONFIG_INIT_PERIODIC(&timerCfg, EmacEvtLinkTimer, EMAC_LINK_POLL_MS);
    status = WdfTimerCreate(&timerCfg, &timerAttrs, &timer);
    if (!NT_SUCCESS(status)) EMAC_LOG("WdfTimerCreate failed 0x%08lx\n", status);
    if (!NT_SUCCESS(status)) return status;
    EmacGetTimerContext(timer)->Adapter = A;
    A->LinkTimer = timer;

    return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG cfg;

    WDF_DRIVER_CONFIG_INIT(&cfg, EmacEvtDeviceAdd);
    cfg.DriverPoolTag = EMAC_POOL_TAG;
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}
