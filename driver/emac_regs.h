/*
 * Allwinner H616/H618 EMAC0 register definitions.
 *
 * Provenance tags:
 *   [DXE]   Mu-Silicium .../AllwinnerPkg/Drivers/EmacDxe/EmacDxe.h (proven on this board in UEFI)
 *   [UBOOT] u-boot drivers/net/sun8i_emac.c (register facts only)
 *   [LINUX] linux drivers/net/ethernet/stmicro/stmmac/dwmac-sun8i.c (register facts only)
 *   [UNV]   not verified against the H616 user manual / hardware
 */
#pragma once

#define EMAC_REG_SIZE           0x10000     /* Linux dtsi: reg = <0x05020000 0x10000>   */
#define SID_REG_SIZE            0x10        /* four 32-bit words at 0x03006200  [DXE]   */

#ifndef BIT
#define BIT(n)                  (1UL << (n))
#endif

/* ---- MAC registers (offsets from 0x05020000) ------------------------------------- */
#define EMAC_CTL0               0x00        /* [DXE][LINUX] BASIC_CTL0 */
#define EMAC_CTL1               0x04        /* [DXE][LINUX] BASIC_CTL1 */
#define EMAC_INT_STA            0x08        /* [LINUX][UBOOT] write 1 to clear   */
#define EMAC_INT_EN             0x0C        /* [LINUX][UBOOT] */
#define EMAC_TX_CTL0            0x10        /* [DXE] */
#define EMAC_TX_CTL1            0x14        /* [DXE] */
#define EMAC_TX_FLOW_CTL        0x1C        /* [LINUX] */
#define EMAC_TX_DESC_LIST       0x20        /* [DXE] EMAC_TX_DMA_DESC */
#define EMAC_RX_CTL0            0x24        /* [DXE] */
#define EMAC_RX_CTL1            0x28        /* [DXE] */
#define EMAC_RX_DESC_LIST       0x34        /* [DXE] EMAC_RX_DMA_DESC */
#define EMAC_RX_FRM_FLT         0x38        /* [LINUX] */
#define EMAC_MII_CMD            0x48        /* [DXE] */
#define EMAC_MII_DATA           0x4C        /* [DXE] */
#define EMAC_ADDR_HIGH(n)       (0x50 + (n) * 8)   /* [LINUX] slot 0 = station address [DXE] */
#define EMAC_ADDR_LOW(n)        (0x54 + (n) * 8)
#define EMAC_TX_DMA_STA         0xB0        /* [LINUX][UBOOT] */
#define EMAC_RX_DMA_STA         0xC0

#define EMAC_ADDR_SLOTS         8           /* [LINUX] unicast_filter_entries = 8 */

/* CTL0 */
#define EMAC_CTL0_FULL_DUPLEX   BIT(0)      /* [DXE] */
#define EMAC_CTL0_SPEED_MASK    (3UL << 2)  /* [UBOOT] */
#define EMAC_CTL0_SPEED_1000    (0UL << 2)  /* [DXE][UBOOT][LINUX] */
#define EMAC_CTL0_SPEED_100     (3UL << 2)  /* [DXE][UBOOT][LINUX] */
#define EMAC_CTL0_SPEED_10      (2UL << 2)  /* [DXE][UBOOT][LINUX] */

/* CTL1 */
#define EMAC_CTL1_SOFT_RST      BIT(0)      /* [DXE] self-clearing */
#define EMAC_CTL1_BURST_SHIFT   24          /* [DXE] value used: 8 */
#define EMAC_CTL1_BURST_LEN     8

/* TX / RX control */
#define EMAC_TX_CTL0_TX_EN      BIT(31)     /* [DXE] */
#define EMAC_TX_CTL1_TX_MD      BIT(1)      /* [DXE] store and forward */
#define EMAC_TX_CTL1_TX_DMA_EN  BIT(30)     /* [DXE] */
#define EMAC_TX_CTL1_TX_DMA_START BIT(31)   /* [DXE] */
#define EMAC_RX_CTL0_RX_EN      BIT(31)     /* [DXE] */
#define EMAC_RX_CTL1_RX_MD      BIT(1)      /* [DXE] store and forward */
#define EMAC_RX_CTL1_RX_RUNT    BIT(2)      /* [DXE] accept runt frames */
#define EMAC_RX_CTL1_RX_ERR     BIT(3)      /* [DXE] accept errored frames (driver drops them) */
#define EMAC_RX_CTL1_RX_DMA_EN  BIT(30)     /* [DXE] */
#define EMAC_RX_CTL1_RX_DMA_START BIT(31)   /* [DXE] */

/* RX frame filter (EMAC_RX_FRM_FLT) [LINUX] [UNV on hardware: not used by EmacDxe] */
#define EMAC_FLT_RXALL          BIT(0)
#define EMAC_FLT_CTL            BIT(13)
#define EMAC_FLT_ALLMULTI       BIT(16)
#define EMAC_ADDR_HIGH_ENABLE   BIT(31)     /* set on slots 1..7 */

/* Interrupts [LINUX]. [UNV] on this SoC: EmacDxe never enables interrupts. */
#define EMAC_INT_TX             BIT(0)
#define EMAC_INT_TX_DMA_STOP    BIT(1)
#define EMAC_INT_TX_BUF_UA      BIT(2)
#define EMAC_INT_TX_TIMEOUT     BIT(3)
#define EMAC_INT_TX_UNDERFLOW   BIT(4)
#define EMAC_INT_TX_EARLY       BIT(5)
#define EMAC_INT_RX             BIT(8)
#define EMAC_INT_RX_BUF_UA      BIT(9)
#define EMAC_INT_RX_DMA_STOP    BIT(10)
#define EMAC_INT_RX_TIMEOUT     BIT(11)
#define EMAC_INT_RX_OVERFLOW    BIT(12)
#define EMAC_INT_RX_EARLY       BIT(13)
#define EMAC_INT_RGMII_STA      BIT(16)
#define EMAC_INT_ALL            0x1FFFFFFUL /* [LINUX] clear-all value */

#define EMAC_INT_TX_MASK        (EMAC_INT_TX)
#define EMAC_INT_RX_MASK        (EMAC_INT_RX | EMAC_INT_RX_BUF_UA | EMAC_INT_RX_OVERFLOW)

/* MDIO [DXE] */
#define EMAC_MDIO_BUSY          BIT(0)
#define EMAC_MDIO_WRITE         BIT(1)
#define EMAC_MDIO_DIV_128       (3UL << 20)
#define EMAC_MDIO_REG(r)        ((ULONG)(r) << 4)
#define EMAC_MDIO_PHY(p)        ((ULONG)(p) << 12)

/* ---- DMA descriptor [DXE] -------------------------------------------------------- */
/*
 * Four words, chained through Next. EmacDxe pads each descriptor to 128 bytes
 * (EMAC_DESC_STRIDE) because it manages cache lines by hand; the same stride is
 * kept here until it is proven that a smaller one works.
 */
#define EMAC_DESC_STRIDE        128
#define EMAC_DESC_OWN           BIT(31)     /* set = owned by the DMA engine */
#define EMAC_DESC_LAST          BIT(30)     /* TX control word */
#define EMAC_DESC_FIRST         BIT(29)     /* TX control word */
#define EMAC_DESC_CHAIN         BIT(24)     /* TX control word ("CHAIN_SECOND") */
#define EMAC_RX_STATUS_LEN(s)   (((s) >> 16) & 0x3FFF)   /* includes the 4-byte FCS */
#define EMAC_RX_STATUS_ERR_MASK 0x400068DBUL
#define EMAC_RX_SIZE_MAX        2044        /* [DXE] EMAC_RX_BUFFER_SIZE, multiple of 4 */
#define EMAC_FCS_SIZE           4
#define EMAC_MIN_RX_FRAME       64          /* [DXE] incl. FCS */

typedef struct _EMAC_DESC {
    ULONG Status;       /* OWN, and on RX completion: length[29:16] + error bits */
    ULONG Control;      /* TX: length | FIRST | LAST | CHAIN. RX: buffer size     */
    ULONG BufferAddress;/* 32-bit bus address */
    ULONG Next;         /* 32-bit bus address of the next descriptor */
} EMAC_DESC;

typedef struct _EMAC_DESC_SLOT {
    EMAC_DESC Desc;
    UCHAR Pad[EMAC_DESC_STRIDE - sizeof(EMAC_DESC)];
} EMAC_DESC_SLOT;

/* ---- PHY: generic MII [DXE] -------------------------------------------------------- */
#define MII_BMCR                0x00
#define MII_BMSR                0x01
#define MII_PHYSID1             0x02
#define MII_PHYSID2             0x03
#define MII_ADVERTISE           0x04
#define MII_CTRL1000            0x09
#define MII_YT_PHY_STATUS       0x11

#define BMCR_ANRESTART          BIT(9)
#define BMCR_ANENABLE           BIT(12)
#define BMCR_RESET              BIT(15)
#define BMSR_LSTATUS            BIT(2)
#define BMSR_ANEGCOMPLETE       BIT(5)

#define ADVERTISE_CSMA          BIT(0)
#define ADVERTISE_10HALF        BIT(5)
#define ADVERTISE_10FULL        BIT(6)
#define ADVERTISE_100HALF       BIT(7)
#define ADVERTISE_100FULL       BIT(8)
#define ADVERTISE_1000HALF      BIT(8)     /* in CTRL1000 */
#define ADVERTISE_1000FULL      BIT(9)     /* in CTRL1000 */

/* ---- PHY: Motorcomm YT8531 [DXE] ------------------------------------------------- */
#define YT8531_PHY_ID           0x4F51E91BUL
#define YT_EXT_ADDR             0x1E
#define YT_EXT_DATA             0x1F
#define YT_CHIP_CONFIG          0xA001
#define YT_RGMII_CONFIG1        0xA003
#define YT_SYNCE_CONFIG         0xA012
#define YT_RXC_DELAY_ENABLE     BIT(8)
#define YT_RX_DELAY_MASK        (0xFUL << 10)
#define YT_RX_DELAY_1950_PS     (13UL << 10)
#define YT_SYNCE_ENABLE         BIT(6)
#define YT_CLK_125MHZ           BIT(4)
#define YT_CLK_SOURCE_MASK      (7UL << 1)
#define YT_STATUS_SPEED_MASK    ((3UL << 14) | BIT(9))
#define YT_STATUS_SPEED_10      (0UL << 14)
#define YT_STATUS_SPEED_100     (1UL << 14)
#define YT_STATUS_SPEED_1000    (2UL << 14)
#define YT_STATUS_SPEED_2500    BIT(9)
#define YT_STATUS_DUPLEX        BIT(13)
