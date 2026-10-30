/*
 * MDIO + Motorcomm YT8531 handling. Sequence taken from EmacDxe.c
 * (EmacMdioAt, EmacConfigureYt8531, EmacPhyStartAutoneg, EmacReadNegotiatedMode).
 */
#include "emac.h"

#define MDIO_POLL_ITERATIONS    1000
#define MDIO_POLL_STALL_US      10

/* Caller holds A->Lock or is the only user (D0Entry before the timer starts). */
static NTSTATUS EmacMdioWait(EMAC_ADAPTER *A)
{
    for (ULONG i = 0; i < MDIO_POLL_ITERATIONS; i++) {
        if ((EMAC_RD(A, EMAC_MII_CMD) & EMAC_MDIO_BUSY) == 0) {
            return STATUS_SUCCESS;
        }
        KeStallExecutionProcessor(MDIO_POLL_STALL_US);
    }
    return STATUS_IO_TIMEOUT;
}

NTSTATUS EmacMdioRead(EMAC_ADAPTER *A, UCHAR Reg, USHORT *Value)
{
    NTSTATUS status = EmacMdioWait(A);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    EMAC_WR(A, EMAC_MII_CMD,
            EMAC_MDIO_REG(Reg) | EMAC_MDIO_PHY(A->PhyAddress) | EMAC_MDIO_DIV_128 | EMAC_MDIO_BUSY);

    status = EmacMdioWait(A);
    if (NT_SUCCESS(status)) {
        *Value = (USHORT)EMAC_RD(A, EMAC_MII_DATA);
    }
    return status;
}

NTSTATUS EmacMdioWrite(EMAC_ADAPTER *A, UCHAR Reg, USHORT Value)
{
    NTSTATUS status = EmacMdioWait(A);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    EMAC_WR(A, EMAC_MII_DATA, Value);
    EMAC_WR(A, EMAC_MII_CMD,
            EMAC_MDIO_REG(Reg) | EMAC_MDIO_PHY(A->PhyAddress) | EMAC_MDIO_DIV_128 |
            EMAC_MDIO_WRITE | EMAC_MDIO_BUSY);

    return EmacMdioWait(A);
}

/* Locked wrappers */
static NTSTATUS PhyRead(EMAC_ADAPTER *A, UCHAR Reg, USHORT *Value)
{
    NTSTATUS status;
    WdfSpinLockAcquire(A->Lock);
    status = EmacMdioRead(A, Reg, Value);
    WdfSpinLockRelease(A->Lock);
    return status;
}

static NTSTATUS PhyWrite(EMAC_ADAPTER *A, UCHAR Reg, USHORT Value)
{
    NTSTATUS status;
    WdfSpinLockAcquire(A->Lock);
    status = EmacMdioWrite(A, Reg, Value);
    WdfSpinLockRelease(A->Lock);
    return status;
}

/* YT8531 extended registers: write address to 0x1E, access data through 0x1F. */
static NTSTATUS YtExtRead(EMAC_ADAPTER *A, USHORT ExtReg, USHORT *Value)
{
    NTSTATUS status;
    WdfSpinLockAcquire(A->Lock);
    status = EmacMdioWrite(A, YT_EXT_ADDR, ExtReg);
    if (NT_SUCCESS(status)) {
        status = EmacMdioRead(A, YT_EXT_DATA, Value);
    }
    WdfSpinLockRelease(A->Lock);
    return status;
}

static NTSTATUS YtExtWrite(EMAC_ADAPTER *A, USHORT ExtReg, USHORT Value)
{
    NTSTATUS status;
    WdfSpinLockAcquire(A->Lock);
    status = EmacMdioWrite(A, YT_EXT_ADDR, ExtReg);
    if (NT_SUCCESS(status)) {
        status = EmacMdioWrite(A, YT_EXT_DATA, Value);
    }
    WdfSpinLockRelease(A->Lock);
    return status;
}

static NTSTATUS YtConfigure(EMAC_ADAPTER *A)
{
    NTSTATUS status;
    USHORT chip, rgmii, synce;

    status = YtExtRead(A, YT_CHIP_CONFIG, &chip);
    if (!NT_SUCCESS(status)) return status;
    status = YtExtRead(A, YT_RGMII_CONFIG1, &rgmii);
    if (!NT_SUCCESS(status)) return status;
    status = YtExtRead(A, YT_SYNCE_CONFIG, &synce);
    if (!NT_SUCCESS(status)) return status;

    /* RX delay is applied inside the PHY (board uses rgmii-rxid), not on RXC. */
    chip &= (USHORT)~YT_RXC_DELAY_ENABLE;
    status = YtExtWrite(A, YT_CHIP_CONFIG, chip);
    if (!NT_SUCCESS(status)) return status;

    rgmii = (USHORT)((rgmii & ~YT_RX_DELAY_MASK) | YT_RX_DELAY_1950_PS);
    status = YtExtWrite(A, YT_RGMII_CONFIG1, rgmii);
    if (!NT_SUCCESS(status)) return status;

    synce = (USHORT)((synce & ~(YT_SYNCE_ENABLE | YT_CLK_125MHZ | YT_CLK_SOURCE_MASK)) |
                     YT_SYNCE_ENABLE | YT_CLK_125MHZ);
    return YtExtWrite(A, YT_SYNCE_CONFIG, synce);
}

static void Sleep1ms(void)
{
    LARGE_INTEGER t;
    t.QuadPart = -10000;        /* 1 ms */
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

NTSTATUS EmacPhyInitialize(EMAC_ADAPTER *A)
{
    NTSTATUS status;
    USHORT id1 = 0, id2 = 0, v = 0;
    ULONG i;

    A->PhyReady = FALSE;

    status = PhyRead(A, MII_PHYSID1, &id1);
    if (!NT_SUCCESS(status)) return status;
    status = PhyRead(A, MII_PHYSID2, &id2);
    if (!NT_SUCCESS(status)) return status;

    EMAC_LOG("PHY @%lu ID %04x:%04x\n", A->PhyAddress, id1, id2);
    if ((((ULONG)id1 << 16) | id2) != YT8531_PHY_ID) {
        EMAC_LOG("expected YT8531 (%08lx); wrong PHY address or board?\n", YT8531_PHY_ID);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    /* Unlike EmacDxe we always reset: the MAC side was just power-cycled by ACPI. */
    status = PhyWrite(A, MII_BMCR, BMCR_RESET);
    if (!NT_SUCCESS(status)) return status;
    for (i = 0; i < 500; i++) {
        if (NT_SUCCESS(PhyRead(A, MII_BMCR, &v)) && (v & BMCR_RESET) == 0) {
            break;
        }
        Sleep1ms();
    }
    if (i == 500) {
        EMAC_LOG("PHY reset did not complete\n");
        return STATUS_IO_TIMEOUT;
    }

    status = YtConfigure(A);
    if (!NT_SUCCESS(status)) return status;

    status = PhyWrite(A, MII_ADVERTISE,
                      ADVERTISE_CSMA | ADVERTISE_10HALF | ADVERTISE_10FULL |
                      ADVERTISE_100HALF | ADVERTISE_100FULL);
    if (!NT_SUCCESS(status)) return status;
    status = PhyWrite(A, MII_CTRL1000, ADVERTISE_1000HALF | ADVERTISE_1000FULL);
    if (!NT_SUCCESS(status)) return status;
    status = PhyWrite(A, MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);
    if (!NT_SUCCESS(status)) return status;

    A->PhyReady = TRUE;
    return STATUS_SUCCESS;
}

/* Safe at DISPATCH_LEVEL (used from the link timer). */
NTSTATUS EmacPhyReadLink(EMAC_ADAPTER *A, BOOLEAN *Up, ULONG *SpeedMbps, BOOLEAN *Full)
{
    NTSTATUS status;
    USHORT bmsr = 0, st = 0;

    *Up = FALSE;

    /* BMSR link bit latches low; read twice like EmacDxe. */
    status = PhyRead(A, MII_BMSR, &bmsr);
    if (!NT_SUCCESS(status)) return status;
    status = PhyRead(A, MII_BMSR, &bmsr);
    if (!NT_SUCCESS(status)) return status;

    if ((bmsr & (BMSR_LSTATUS | BMSR_ANEGCOMPLETE)) != (BMSR_LSTATUS | BMSR_ANEGCOMPLETE)) {
        return STATUS_SUCCESS;
    }

    status = PhyRead(A, MII_YT_PHY_STATUS, &st);
    if (!NT_SUCCESS(status)) return status;

    switch (st & YT_STATUS_SPEED_MASK) {
    case YT_STATUS_SPEED_10:   *SpeedMbps = 10;   break;
    case YT_STATUS_SPEED_100:  *SpeedMbps = 100;  break;
    case YT_STATUS_SPEED_1000: *SpeedMbps = 1000; break;
    default:
        EMAC_LOG("unsupported/invalid YT8531 status %04x\n", st);
        return STATUS_NOT_SUPPORTED;
    }

    *Full = (st & YT_STATUS_DUPLEX) != 0;
    *Up = TRUE;
    return STATUS_SUCCESS;
}
