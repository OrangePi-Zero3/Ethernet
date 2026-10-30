# Orange Pi Zero3 – Windows 11 22H2 ARM64 Ethernet (EMAC0) – design note, status, test guide

Status: **written but never built or run.** The authoring environment had no WDK, no MSVC and no
hardware. What was actually checked: the ASL compiles with `iasl -we` (0 errors, 0 warnings, AML
included), and the C sources pass `gcc -fsyntax-only` against hand-written stand-in headers (this catches
typos and wrong argument counts against *my idea* of the API, not the real WDK). Treat first build and
first boot as bring-up, not as a verification of a finished driver.

## 1. Layout

```
acpi/DSDT_minimal.dsl         patched DSDT (EMAC power resource + device), compiles with iasl
acpi/DSDT_minimal.aml         compiled output
acpi/DSDT_minimal.emac.patch  diff against Silicium-ACPI/SoCs/Allwinner/H616/DSDT_minimal.dsl
driver/                       KMDF + NetAdapterCx 2.1 driver (C): driver.c phy.c datapath.c + headers
driver/emac.inx               INF template (hardware ID ACPI\AWEM0001)
driver/zero3emac.vcxproj      hand-written project, untested
```

## 2. Decisions

**Driver model: KMDF + NetAdapterCx 2.1 (non-preview).** UMDF NetAdapterCx needs Windows 11 24H2, so it is
out for 22H2 (build 22621). NetAdapterCx 2.1 is what bcmgenet's `cx21` variant targets for builds ≥ 19536;
check the exact version against the WDK 22621 headers. NDIS miniport (netvmini, imxnetmini) stays the fallback.

**Who brings up clock/reset/pinmux: ACPI (Option A).** EmacDxe's ExitBootServices handler stops DMA,
asserts the MAC reset and gates the bus clock, and Windows has no CCU/pinctrl/syscon driver on this SoC.
The EMAC device therefore has a power resource (`EPWR`, `_PR0`) whose `_ON` repeats EmacDxe's entry
sequence with `OperationRegion(SystemMemory)` accesses; `_OFF` asserts reset and gates the clock. The
driver only programs the MAC and the PHY. Trade-off: this relies on Windows ACPI accepting
SystemMemory regions over CCU/PIO/SYS_CFG (allowed, nothing else claims them) and on the AML being
correct, which is only checked by the compiler so far. Option B (extra `Memory32Fixed` windows and
poking CCU/PIO from the driver) is the fallback if `_ON` misbehaves. Option C (modify EmacDxe to leave
the clock running) was not done.

**DMA.** `_CCA = 0` (as the other devices in this DSDT). 32-bit WDF DMA enabler
(`WdfDmaProfileScatterGather`): the MAC takes 32-bit descriptor and buffer addresses and the 4 GB Zero3 has
RAM above 4 GB. Descriptor rings and TX slots are WDF common buffers (always parented to the DMA enabler,
never given a `ParentObject`; deleted in the queue destroy callback). RX uses system-managed DMA buffers with
`MaximumPhysicalAddress = 0xFFFFFFFF`; every address is range-checked before it goes into a descriptor
(no silent truncation). One descriptor per ring element, 128-byte stride as in EmacDxe.

**TX: copy into driver slots, no framework DMA mapping.** The first run with traffic hit
`SYSTEM_PTE_MISUSE (0xDA, 0x302)` inside `NetAdapterCx!NxDmaAdapter::CleanupNetPacket` (NetAdapterCx
unmapping an MDL chain it had mapped for a HAL-remapped TX packet). The TX queue now declares no DMA
(`NET_ADAPTER_TX_CAPABILITIES_INIT`), asks for the fragments' virtual addresses, copies each packet (up to
16 fragments, at most 2048 bytes) into one slot of a driver-owned common buffer and sends it with a single
FIRST|LAST descriptor: the path EmacDxe and U-Boot use. Costs a memcpy per packet; irrelevant at 1 Gbps on
this SoC.

**Interrupts.** One SPI, GSIV 46 (SPI 14 + 32). RX/TX sources are armed only while the queue wants a
notification, and are masked in the ISR. If interrupts misbehave the quickest bring-up fallback is a
polling DPC; not implemented.

**MAC address.** Same SID-derived algorithm as EmacDxe (`EmacGenerateMac`), so UEFI/PXE and Windows agree;
overridable with the `NetworkAddress` keyword. SID words are read via a third resource in `_CRS`.

**PHY.** YT8531 at MDIO address 1 (INF keyword `PhyAddress`), init sequence from EmacDxe, plus an
unconditional PHY reset (EmacDxe skips it when the link is already up). Link polled once a second.

## 3. Where each hardware fact comes from

| Fact | Source |
|---|---|
| MAC base 0x05020000, size 0x10000, SPI 14 | Linux `sun50i-h616.dtsi`; EmacDxe.h for the base |
| GSIV = SPI + 32 | cross-check: EHC1 (SPI 28 in the dtsi) appears as 60 and SDC0 (SPI 35) as 67 in `DSDT_minimal.dsl` |
| EMAC_BGR 0x03001000+0x97C, gate bit0, reset bit16 | Mu-Silicium `ClockDefinitions.h` |
| SYS_CFG 0x03000030 value 0x00011C06 | EmacDxe.h/.c (`EPHY_SHUTDOWN\|EPIT\|ETCS=2\|7<<10`) |
| PI0..PI16 (not PI6), mux 2, drive 3; bank I at 0x0300B120 | EmacDxe `EmacConfigurePins`, GpioDxe.h (bank stride 0x24, CFG +0, DRV +0x14) |
| Register offsets, descriptor layout, speed bits | EmacDxe.h, cross-checked with U-Boot `sun8i_emac.c` and Linux `dwmac-sun8i.c` |
| Interrupt bit positions, RX_FRM_FLT bits, 8 address slots | Linux `dwmac-sun8i.c` only |
| YT8531 sequence | EmacDxe.c |
| NetAdapterCx structure, INF | bcmgenet, RtEthSample (both MIT); `netringiterator.h` copied from RtEthSample (MIT) |

Linux and U-Boot are GPL; only register facts were taken, no code.

## 4. UNVERIFIED (check these first when something fails)

1. **Interrupt bits and behavior.** `EMAC_INT_*` and "write 1 to clear INT_STA" come from Linux only; EmacDxe
   never enables interrupts. Whether INT_STA latches while masked is assumed.
2. **RX filter register.** EmacDxe never touches `RX_FRM_FLT`. The driver writes Linux's values
   (`FLT_CTL`, `RXALL`, `ALLMULTI`) and uses address slots 1..7 for multicast. Whether broadcast always
   passes with these values is assumed. If DHCP/ARP broadcasts do not arrive, start here.
3. **Ring sizes above 32 descriptors** (hints: TX 128, RX 256) and the **128-byte stride** are proven only at 32 in UEFI.
4. **RX/TX cache coherency** under `_CCA = 0` with system-managed DMA buffers. If RX data is stale or
   corrupted, suspect this before the MAC.
5. **RX DMA start semantics.** The driver sets `RX_DMA_START` at queue start and after reposting buffers
   (Linux does the same); EmacDxe only sets `RX_DMA_EN`.
6. **Windows ASL.** The README says the minimal DSDT is built with MS `asl`; this was compiled with Intel
   `iasl` only. Windows may reject constructs `iasl` accepts. Check with MS `asl.exe` and a boot.
7. **Power resource flow.** Assumes Windows evaluates `_ON` before D0Entry and that the PHY supply
   regulator (`dldo1` in the Linux DTS) is still on after ExitBootServices (EmacDxe does no regulator work).
8. **NetAdapterCx API details** (macro signatures, `FragmentBufferAlignment`, `MaximumNumberOfFragments`,
   ring helper semantics) were taken from the sample repos, whose versions differ from each other.
   Expect compile fixes against the real 22621 WDK.
9. **`WdfInterruptCreate` in PrepareHardware** is copied from bcmgenet; the code sets `Interrupt = NULL` in
   ReleaseHardware on the assumption that the framework deletes it.
10. **Flow control, offloads, statistics, WoL, PHY interrupt** are not implemented.

## 5. Firmware side

1. Replace `Silicium-ACPI/SoCs/Allwinner/H616/DSDT_minimal.dsl/.aml` with the files in `acpi/`
   (or apply `DSDT_minimal.emac.patch`). `Zero3Pkg/Include/ACPI.inc` needs no change because it already
   packages that AML. Check that Mu-Silicium's `Silicium-ACPI` submodule points at the repo you edited.
2. Rebuild UEFI and flash. In the UEFI shell or Windows Device Manager the new device should appear as
   `ACPI\AWEM0001`.

## 6. Build and test (Windows machine)

1. Install Visual Studio 2022 + WDK matching build 22621 with the ARM64 build tools.
2. Open/create the project from `driver/` (see the note in `zero3emac.vcxproj`), build **Debug|ARM64**.
3. On the Zero3: `bcdedit /set testsigning on`, reboot, then install:
   `pnputil /add-driver zero3emac.inf /install`.
4. Logs: `DbgPrintEx` with component `DPFLTR_IHVNETWORK_ID`, error level; enable with
   `ed nt!Kd_IHVNETWORK_Mask 0xf` in WinDbg, or read via serial/DBG2 debugging. Messages are prefixed `EmacWin:`.

### Bring-up checklist

| Step | Expect | If not |
|---|---|---|
| Device Manager shows ACPI\AWEM0001 with Code 0 | resources: mem 0x05020000, mem 0x03006200, one IRQ | Code 10/12: `_CRS` or AML problem |
| Log `mapped regs=... sid=...` then `MAC xx:...` | MAC starts with 02: | resource order/size mismatch |
| No `MAC soft reset timed out` | soft reset completes | power resource `_ON` did not run or CCU/reset bits wrong |
| `PHY @1 ID 4f51:e91b` | YT8531 found | wrong MDIO address (set `PhyAddress`), MDIO clock, or PHY supply off |
| `link up 1000 Mbps full duplex` | within ~2 s of cable | PHY config/delays; test 100 Mbps to separate RGMII timing issues |
| `ping` the router, then DHCP | replies | see UNVERIFIED 2, 4, 1 in that order |

## 7. Risks worth knowing

RGMII delay settings (YT8531 RX 1950 ps, MAC TX chain 7) are copied from the working UEFI setup; a 1000 Mbps
link that comes up but loses packets points there. Never validated: suspend/resume, hot unplug, multicast
with more than 7 addresses, jumbo frames (MTU is fixed at 1500), 4 GB bounce-buffer paths.

## 8. Bring-up log (what has actually been observed on hardware)

| Date | Result |
|---|---|
| run 1 | Code 43. Cause: `ParentObject` set on WDF common buffers (WDF rejects it, 0xC020020F). Fixed. |
| run 2 | Adapter started, PHY found (ID 4f51:e91b), both queues started, link up 100 Mbps full duplex. Then bugcheck 0xDA/0x302 in NetAdapterCx TX completion (see TX note above). Run on top of earlier failed starts without a reboot. TX rewritten to copy into driver slots; RX buffers capped to 4 GB. |
