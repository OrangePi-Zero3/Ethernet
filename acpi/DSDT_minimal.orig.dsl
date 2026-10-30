DefinitionBlock ("", "DSDT", 2, "ALWINR", "H616    ", 0x00000616)
{
    Scope (_SB)
    {
        Device (CPU0)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x00)
            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

        Device (CPU1)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x01)
            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

        Device (CPU2)
        {
            Name (_HID, "ACPI0007" /* Processor Device */)
            Name (_UID, 0x02)
            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

        Device (CPU3)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0x03)
            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

        Device (EHC1)
        {
            Name (_HID, EisaId ("PNP0D20"))
            Name (_UID, 0x01)
            Name (_CCA, Zero)
            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x05200000, 0x00000100)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 60 }
            })
        }

        Device (EHC2)
        {
            Name (_HID, EisaId ("PNP0D20"))
            Name (_UID, 0x02)
            Name (_CCA, Zero)
            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x05310000, 0x00000100)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 62 }
            })
        }

        Device (EHC3)
        {
            Name (_HID, EisaId ("PNP0D20"))
            Name (_UID, 0x03)
            Name (_CCA, Zero)
            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x05311000, 0x00000100)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 64 }
            })
        }

        // never got to bring this up lmao
        Device (SDC0)
        {
            Name (_HID, "AWMC0001")
            Name (_UID, 0x00)
            Name (_CCA, Zero)
            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x04020000, 0x00001000)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 67 }
            })
        }
    }
}
