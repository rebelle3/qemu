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

The handoff mechanism (solved)
------------------------------

osos populates that platform struct from a **"SysInfo" handoff** the
Apple bootloader leaves at a fixed IRAM address. The population routine
(runtime ~0x08323aec) reads the word at **0x2203ff18**, requires it to
equal the magic **0x53797349 ('SysI')**, then ``memcpy``\s a 0x120-byte
block — pointed to by the word at 0x2203ff1c — into the platform struct
at 0x0896d6fc. The hardware version lands at struct +0x84.

The kernel loader now builds this handoff for an injected osos
(``ipod6g_load_kernel``): it writes the platform block (version
0x130000 at +0x84) and the ``SysI`` header into the top of IRAM, which
osos leaves untouched. With it in place osos's getter returns 0x130000,
the predicate passes, and osos **boots past the version gate** to the
next fatal-handler caller (runtime ~0x0800f37c) — verified natively,
no debugger. So the first and most fundamental piece of the ONB handoff
is reconstructed and shipped.

Remaining work to reach the Apple UI
------------------------------------

1. The next gate (~0x0800f37c -> check 0x08013870) is a deeper
   subsystem call chain that returns an error, not a single handoff
   value; work through it and the gates after it the same way.
2. The platform block almost certainly needs more than the version
   (it is 0x120 bytes; only +0x84 is populated today) — fill fields as
   later gates demand them.
3. Provide the storage/resources osos needs once it is past early init
   (NAND/flash controller, Apple disk layout with OS resources), plus
   Apple-specific LCD init, USB, and the crypto engine.

This is a substantial multi-stage effort. The toolchain used here:
``gdb-multiarch`` on the QEMU gdbstub (use ``hbreak`` — osos rewrites
its own code, so software breakpoints do not survive), ``-d int`` for
exception tracing, and ``-d in_asm,exec`` for the path to a halt
(search the trace for the fatal-handler address and read backwards).
