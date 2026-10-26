/** @file
  Differentiated System Description Table Fields (DSDT)

  Copyright (c) 2014-2018, ARM Ltd. All rights reserved.<BR>
  Copyright (c) 2019-2020, Bingxing Wang. All rights reserved.<BR>
  Portion retrieved from Surface RT 2 DSDT dump

  This program and the accompanying materials are licensed and made available under 
  the terms and conditions of the BSD License which accompanies this distribution.  
  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include "Platform.h"

DefinitionBlock("DsdtTable.aml", "DSDT", 1, "NVIDIA", "TEGRA210", EFI_ACPI_ARM_OEM_REVISION) 
{
    Scope(_SB) 
    {

        Device(CPU0) 
        {
            Name(_HID, "ACPI0007")
            Name(_UID, 0)
        }

        Device(CPU1) 
        {
            Name(_HID, "ACPI0007")
            Name(_UID, 1)
        }

        Device(CPU2) 
        {
            Name(_HID, "ACPI0007")
            Name(_UID, 2)
        }

        Device(CPU3) 
        {
            Name(_HID, "ACPI0007")
            Name(_UID, 3)
        }

        // Helper functions
        Method (GCRS, 3, NotSerialized)
        {
            Name (RSRC, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite,
                    0x00000000,         // Address Base
                    0x00000000,         // Address Length
                    _Y00)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive, ,, _Y01)
                {
                    0x00000000,
                }
            })
            CreateDWordField (RSRC, \_SB.GCRS._Y00._BAS, MBAS)  // _BAS: Base Address
            CreateDWordField (RSRC, \_SB.GCRS._Y00._LEN, MBLE)  // _LEN: Length
            CreateWordField (RSRC, \_SB.GCRS._Y01._INT, INTN)  // _INT: Interrupts
            MBAS = Arg0
            MBLE = Arg1
            INTN = Arg2
            Return (RSRC)
        }

        Method (CRSD, 2, NotSerialized)
        {
            Name (RSRC, ResourceTemplate ()
            {
                FixedDMA (0x0000, 0x0000, Width32bit, _Y07)
                FixedDMA (0x0001, 0x0001, Width32bit, _Y08)
            })
            CreateWordField (RSRC, One, REQ1)
            CreateWordField (RSRC, \_SB.CRSD._Y07._TYP, CHN1)  // _TYP: Type
            CreateWordField (RSRC, \_SB.CRSD._Y08._DMA, REQ2)  // _DMA: Direct Memory Access
            CreateWordField (RSRC, \_SB.CRSD._Y08._TYP, CHN2)  // _TYP: Type
            REQ1 = Arg0
            REQ2 = (Arg0 | 0x20)
            CHN1 = Arg1
            CHN2 = Arg1++
            Return (RSRC)
        }

        Method (CRS3, 5, NotSerialized)
        {
            Local0 = GCRS (Arg0, Arg1, Arg2)
            Local1 = CRSD (Arg3, Arg4)
            Return (ConcatenateResTemplate (Local0, Local1))
        }

        // UART 2: Right Joy-Con
        Device (UAR2)
        {
            Method (_HID, 0, NotSerialized)
            {
                Return ("NVDA0100")
            }

            Name (_UID, One)
            Method (_CRS, 0, NotSerialized)
            {
                Return (CRS3 (0x70006040, 0x40, 0x45, 0x09, 0x02))
            }

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

        // Tegra210 XUSB xHCI host controller.
        //
        // Linux runtime evidence identifies three MMIO windows at 0x70090000,
        // 0x70098000 and 0x70099000, with GIC SPI 39/40/49.  ACPI describes
        // ARM GIC SPIs as global interrupt numbers, so these become GSI
        // 71/72/81 (SPI + 32).  This is an enumeration-only step: the DXE
        // audit driver remains read-only and no XUSB register writes are
        // introduced here.
        Device (XHC0)
        {
            Name (_HID, "PNP0D10")
            Name (_CID, "PNP0D10")
            Name (_UID, Zero)
            Name (_CCA, Zero)

            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x70090000, 0x00008000)
                Memory32Fixed (ReadWrite, 0x70098000, 0x00001000)
                Memory32Fixed (ReadWrite, 0x70099000, 0x00001000)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Shared)
                {
                    0x00000047, // GIC SPI 39
                    0x00000048, // GIC SPI 40
                    0x00000051  // GIC SPI 49
                }
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

        // R367: Tegra210 GPIO controller (Windows driver tegra210gpio.sys,
        // GpioClx). MMIO 0x6000D000, 8 banks x 32 pins; one GIC SPI per bank
        // in bank order: SPI 32, 33, 34, 35, 55, 87, 89, 125 (Linux
        // tegra210 gpio node) = GSI 64, 65, 66, 67, 87, 119, 121, 157.
        // GPIO number = port * 8 + bit (PX6 = 190, PX7 = 191).
        Device (GPI0)
        {
            Name (_HID, "SWOA0101")
            Name (_UID, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x6000D000, 0x00001000)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive)
                {
                    0x00000040, 0x00000041, 0x00000042, 0x00000043,
                    0x00000057, 0x00000077, 0x00000079, 0x0000009D
                }
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

        // R368: hardware buttons for Windows (inbox HID button driver).
        // R369: R368 used PNP0C40, which no inbox INF on Windows 10 matches
        // (problem 28); hidinterrupt.inf binds ACPI0011 with this _DSD.
        // Volume up = PX6 (GPIO 190), volume down = PX7 (GPIO 191), both
        // active low with pull-ups (L4T gpio-keys). The power button is on
        // the MAX77620 PMIC (and maybe PX5); it is not listed here yet.
        // GpioInt must be edge ActiveBoth so press and release are both seen.
        Device (BTNS)
        {
            Name (_HID, "ACPI0011")    // R369: ACPI 6.0 Generic Buttons Device (hidinterrupt.inf)
            Name (_UID, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                GpioInt (Edge, ActiveBoth, Exclusive, PullUp, 0x0000, "\\_SB.GPI0", 0x00, ResourceConsumer, ,)
                {
                    190     // PX6: volume up
                }
                GpioInt (Edge, ActiveBoth, Exclusive, PullUp, 0x0000, "\\_SB.GPI0", 0x00, ResourceConsumer, ,)
                {
                    191     // PX7: volume down
                }
            })

            // Map GpioInt index -> HID usage (Windows 10 button array _DSD).
            Name (_DSD, Package (0x02)
            {
                ToUUID ("fa6bd625-9ce8-470d-a2c7-b3ca36c4282e"),
                Package (0x03)
                {
                    Package (0x05) { Zero, One, Zero, One, 0x0D },          // Portable Device Control collection
                    Package (0x05) { One, Zero, One, 0x0C, 0xE9 },          // GpioInt 0: Volume Increment
                    Package (0x05) { One, One, One, 0x0C, 0xEA }            // GpioInt 1: Volume Decrement
                }
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

        // R370: Tegra210 I2C controllers for the Windows driver tegra210i2c.sys
        // (SpbCx, ACPI\SWOA0201). Only the buses whose clock/reset UEFI
        // already enables (PmicDxe: I2C1 and I2C5). Interrupts are from the
        // L4T DT (GIC SPI 38 and 53 = GSI 70 and 85); driver v1 polls.
        //   I2C1 0x7000C000, 100 kHz: BM92T 0x18, MAX17050 0x36, TMP451 0x4C, BQ24193 0x6B
        //   I2C5 0x7000D000, 400 kHz: MAX77621 CPU 0x1B / GPU 0x1C, MAX77620 PMIC 0x3C
        Device (I2C1)
        {
            Name (_HID, "SWOA0201")
            Name (_UID, One)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x7000C000, 0x00000100)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 0x00000046 }
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

        Device (I2C5)
        {
            Name (_HID, "SWOA0201")
            Name (_UID, 0x05)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x7000D000, 0x00000100)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 0x00000055 }
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

        // R371: system battery for switchbattery.sys (ACPI\SWOA0301, battc).
        // Connection 0 = MAX17050 fuel gauge, 1 = BQ24193 charger, both on I2C1.
        Device (BAT0)
        {
            Name (_HID, "SWOA0301")
            Name (_UID, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                I2cSerialBusV2 (0x0036, ControllerInitiated, 100000, AddressingMode7Bit, "\\_SB.I2C1", 0x00, ResourceConsumer, , Exclusive, )
                I2cSerialBusV2 (0x006B, ControllerInitiated, 100000, AddressingMode7Bit, "\\_SB.I2C1", 0x00, ResourceConsumer, , Exclusive, )
                // R378: connection 2 = BM92T USB-C PD controller, for VBUS / charge
                // role switching on hot-plug (switchbattery.sys v4).
                I2cSerialBusV2 (0x0018, ControllerInitiated, 100000, AddressingMode7Bit, "\\_SB.I2C1", 0x00, ResourceConsumer, , Exclusive, )
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

        // R372: fan control for switchfan.sys (ACPI\SWOA0401). PWM controller
        // (the driver only writes PWM_CSR_1 = fan) and the TMP451 sensor on I2C1.
        Device (FAN0)
        {
            Name (_HID, "SWOA0401")
            Name (_UID, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x7000A000, 0x00000100)
                I2cSerialBusV2 (0x004C, ControllerInitiated, 100000, AddressingMode7Bit, "\\_SB.I2C1", 0x00, ResourceConsumer, , Exclusive, )
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

        // R373: I2C3 (GEN3_I2C) for the touch panel. UEFI R373 powers the
        // panel (LDO6 2.9 V, reset PJ7 high) and enables the I2C3 clock.
        Device (I2C3)
        {
            Name (_HID, "SWOA0201")
            Name (_UID, 0x03)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x7000C500, 0x00000100)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 0x0000007C }
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

        // R373: STM FTM4 touch controller (ACPI\SWOA0501), 0x49 on I2C3,
        // interrupt PX1 (GPIO 185) level low.
        Device (TCH0)
        {
            Name (_HID, "SWOA0501")
            Name (_UID, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                I2cSerialBusV2 (0x0049, ControllerInitiated, 400000, AddressingMode7Bit, "\\_SB.I2C3", 0x00, ResourceConsumer, , Exclusive, )
                GpioInt (Level, ActiveLow, Exclusive, PullUp, 0x0000, "\\_SB.GPI0", 0x00, ResourceConsumer, , ) { 185 }
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

        // R374: power button for switchpwrbtn.sys (ACPI\SWOA0601). The button
        // drives MAX77620 EN0; the driver polls ONOFFSTAT/ONOFFIRQ on I2C5.
        Device (PWRB)
        {
            Name (_HID, "SWOA0601")
            Name (_UID, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                I2cSerialBusV2 (0x003C, ControllerInitiated, 400000, AddressingMode7Bit, "\\_SB.I2C5", 0x00, ResourceConsumer, , Exclusive, )
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

        // R375: CPU clock control for switchcpu.sys (ACPI\SWOA0701). CAR window
        // (PLLX, CCLK burst policy), MAX77621 VDD_CPU on I2C5, TMP451 on I2C1
        // (shared with the fan driver) for the thermal guard.
        Device (CPUC)
        {
            Name (_HID, "SWOA0701")
            Name (_UID, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x60006000, 0x00001000)
                I2cSerialBusV2 (0x001B, ControllerInitiated, 400000, AddressingMode7Bit, "\\_SB.I2C5", 0x00, ResourceConsumer, , Exclusive, )
                I2cSerialBusV2 (0x004C, ControllerInitiated, 100000, AddressingMode7Bit, "\\_SB.I2C1", 0x00, ResourceConsumer, , Shared, )
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

        // R377: real-time clock for switchrtc.sys (ACPI\SWOA0801). MAX77620 RTC
        // block at I2C5 0x68. The driver only reads it and keeps its own offset,
        // so the time Horizon keeps in the RTC is left alone.
        Device (RTC0)
        {
            Name (_HID, "SWOA0801")
            Name (_UID, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                I2cSerialBusV2 (0x0068, ControllerInitiated, 400000, AddressingMode7Bit, "\\_SB.I2C5", 0x00, ResourceConsumer, , Exclusive, )
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }

    } // Scope(_SB)
}
