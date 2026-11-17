// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// bl33_stub.S - BL33 entry shim for hekate's L4T launcher (SWITCH_HEKATE flavour).
//
// bl33.bin = this 4 KiB stub + TEGRA210_EFI.fd (see Tools/make-bl33.py).
// hekate loads bl33.bin at 0xAA000000 (BL33_LOAD_BASE in bootloader/l4t/l4t.c); BL31 then
// enters it with pc = 0xAA000000 and spsr = SPSR_EL2T (EL2, SP_EL0, D/A/I/F all clear).
//
// hekate contract, read from l4t.c (master 45cd81e) - NOT run on hardware:
//   * For every L4T launch (even with uart_port=0) hekate evaluates
//         *(u32 *)(0xAA000000 + *(u32 *)(0xAA000000 + 0x10))  ==  0xEDFE0DD0 (FDT magic)
//     so the u32 at +0x10 MUST be a small offset that stays inside this image, otherwise hekate
//     dereferences a wild pointer before it ever launches. The word it points to must not be
//     the FDT magic (we are not a DTB; hekate would patch UART strings into it).
//
// What the stub does: mask D/A/I/F (the entry SPSR leaves them unmasked and no vectors are
// installed yet), select SP_ELx, and branch to the FD at stub + 0x1000. The FD's first
// instruction is a PC-relative branch into PrePi's _ModuleEntryPoint, which sets up its own
// stack (ArmPlatformPeiBootAction does `msr SPSel, #1` as well; both are harmless).
//
// Build: aarch64-linux-gnu-as, ld -Ttext=0xAA000000, objcopy -O binary (make-bl33.py does it).

	.text
	.global	_start
_start:
	b	stub_main			// +0x00  entry point (pc = BL33_LOAD_BASE)

	.org	0x10
	.word	stub_tag - _start		// +0x10  offset hekate dereferences (= 0x20)

	.org	0x20
stub_tag:
	.ascii	"WINX"				// +0x20  must differ from the FDT magic
	.ascii	"HKT1"				// +0x24  stub layout tag, verified by make-bl33.py

	.org	0x40
stub_main:
	msr	daifset, #0xf			// mask Debug, SError, IRQ, FIQ
	msr	spsel, #1			// use SP_ELx
	isb
	b	pwm_fix				// R418, then the FD

// R418: backlight PWM clock. hekate itself runs PWM0 from CLK_M / 3 (CLK_SOURCE_PWM =
// 0xC0000004, bdk/soc/clock.c _clock_pwm: 6.4 MHz, 25 kHz PWM), but by the time BL31 enters
// us the source is 0x80000004 (bits 31:30 = 2 = CLK_32K, Linux mux_pllp_pllc_clk32_clkm):
// 32.768 kHz / 3 / 256 = ~43 Hz backlight PWM, a visible flicker until UsbPowerAuditDxe
// (R389) put the source back much later. Put CLK_M back here, at the first instruction of
// bl33, without resetting the PWM (same write as R389). Only when the PWM clock is enabled
// and out of reset. The values found are kept at pwm_save for UsbPowerAuditDxe to print.
// No unconditional B before the final one (make-bl33.py follows the first B to the FD).
pwm_fix:
	ldr	x0, =0x60006000			// CAR
	ldr	w1, [x0, #0x110]		// CLK_SOURCE_PWM
	ldr	w2, [x0, #0x010]		// CLK_OUT_ENB_L
	ldr	w3, [x0, #0x004]		// RST_DEVICES_L
	adr	x9, pwm_save
	str	w1, [x9, #4]			// +4  source found
	ubfx	w4, w2, #17, #1			// PWM clock enabled
	ubfx	w5, w3, #17, #1			// PWM in reset
	orr	w6, w4, w5, lsl #1
	str	w6, [x9, #12]			// +12 bit0 clk on, bit1 in reset
	mov	w7, #0
	cmp	w6, #1				// on and out of reset
	b.ne	1f
	ldr	x8, =0x7000A000			// PWM
	ldr	w7, [x8]			// CSR_0 (backlight)
	ldr	w10, =0xC0000004		// CLK_M, divisor 3 (hekate, coreboot chain)
	cmp	w1, w10
	b.eq	1f
	str	w10, [x0, #0x110]
	dsb	sy
1:
	str	w7, [x9, #8]			// +8  PWM CSR_0
	ldr	w1, [x0, #0x110]
	str	w1, [x9, #16]			// +16 source now
	ldr	w11, =0x314D5750		// "PWM1"
	str	w11, [x9]			// +0  valid tag, written last
	dsb	sy
	b	fd_entry			// UEFI FD at stub + 0x1000 (= 0xAA001000)
	.ltorg

	.org	0x800
pwm_save:					// 0xAA000800, read by UsbPowerAuditDxe (R418)
	.word	0, 0, 0, 0, 0

	.org	0x1000
fd_entry:
