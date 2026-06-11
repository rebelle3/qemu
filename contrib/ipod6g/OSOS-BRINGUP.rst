iPod Classic 6G — booting the original Apple firmware (osos)
===========================================================

This documents the state of bringing up Apple's original firmware
("osos", extracted and decrypted from an IPSW) on the ``ipod6g`` QEMU
machine, and the exact point where it currently stops.

The proprietary ``osos`` and NOR images are **never** committed; they
are supplied at run time::

    qemu-system-arm -M ipod6g \
        -kernel osos_decrypted.bin \
        -drive file=ipod.img,format=raw,if=ide \
        -drive file=nor_full.bin,format=raw,if=mtd,readonly=on \
        -serial mon:stdio -display none

The machine recognises the Apple IM3 magic (``8702``) on ``-kernel``,
strips the 0x800 header and enters the firmware body at the DRAM base
(0x08000000) — i.e. it injects the decrypted OS directly, bypassing the
encrypted boot chain.

Why the authentic boot chain cannot be replayed
-----------------------------------------------

Real boot: bootrom -> Apple NOR bootloader ("ONB", in NOR @ 0x8000) ->
ONB loads osos from the disk firmware partition -> osos.

The ONB in the NOR is an IM3 image encrypted with the **device-unique
key** (Rockbox's ``im3_crypt``/``im3_sign`` select ``HWKEYAES_UKEY`` =
key index 2, "device unique key"), fused into that specific SoC's UID.
That key is not present in any file we have and is not reproducible in
emulation, so the ONB cannot be decrypted. Hence osos must be injected
directly (as above) and the ONB's hardware/handoff role has to be
stood in for.

How far osos gets
-----------------

With the direct ``-kernel`` injection osos **runs**: it sets up its
ARM mode stacks, relocates/decompresses itself into IRAM, configures
the S5L8702 timers, and services ~1 kHz timer IRQs through the PL192
VIC (verified with ``-d int``). It is RTXC RTOS based ("RTXC v3.2b for
ARM" banner at the reset vector).

It then **deliberately halts** (it is not a crash and not an unhandled
CPU exception — ``-d int`` shows only IRQs): a generic fatal handler at
runtime address ~0x08322b10 disables the watchdog (writes 0 to
0x3c800000) and spins on ``b .``.

The exact stop: a missing hardware-version handoff
--------------------------------------------------

The first fatal handler reached is the decision at runtime 0x083286d4::

    bl   0x80c11f8      ; getter -> hardware version
    bl   0x827a188      ; predicate: (version >> 16) == 0x13 ? 1 : 0
    cmp  r0, #0
    bne  continue       ; predicate true  -> boot continues
    bl   <fatal/halt>   ; predicate false -> HALT

i.e. osos requires ``(hw_version >> 16) == 0x13`` — that is
``0x13xxxx``, which matches the SysCfg **HwVr = 0x130000** read from the
NOR (System > Debug > View SysCfg shows ``Hardware version (HwVr):
130000``).

The version is read from a platform-info struct (default at
0x0896d6fc, pointer cached at 0x089116fc), field **+0x84**. At the halt
that whole struct is **all zero**, so the version reads 0,
``0 >> 16 != 0x13``, and osos halts.

Proven cause and effect
-----------------------

Writing 0x00130000 into the struct field (0x0896d780) and the getter's
cache (0x08911c30) at run time — after osos's bss-clear, via a GDB
hardware breakpoint, software breakpoints get clobbered by osos's
self-decompression — makes the predicate pass and osos **advances past
this gate to the next fatal-handler caller**. So the diagnosis is
confirmed: osos is missing handoff state that the Apple ONB normally
establishes, starting with the board/hardware version.

The field lives inside the loaded osos image but is zeroed by osos's
own startup, so it cannot simply be pre-seeded by the kernel loader; it
has to be provided after osos initialises, or produced by whatever
init osos expects to populate the struct (not yet located — osos does
not reference the SysCfg ``SCfg``/``HwVr`` tags directly, so it obtains
the value via a handoff/detection path rather than parsing SysCfg).

Remaining work to reach the Apple UI
------------------------------------

1. Replicate the ONB handoff: populate osos's platform struct (hardware
   version 0x130000 first; the struct is ~160 bytes, other fields TBD)
   at the right point in osos init.
2. Work through the subsequent fatal-handler gates the same way (each is
   another expected-state check).
3. Provide the storage/resources osos needs once it is past early init
   (NAND/flash controller, Apple disk layout with OS resources), plus
   Apple-specific LCD init, USB, and the crypto engine.

This is a substantial multi-stage effort. The toolchain used here:
``gdb-multiarch`` on the QEMU gdbstub (use ``hbreak`` — osos rewrites
its own code, so software breakpoints do not survive), ``-d int`` for
exception tracing, and ``-d in_asm,exec`` for the path to a halt
(search the trace for the fatal-handler address and read backwards).
