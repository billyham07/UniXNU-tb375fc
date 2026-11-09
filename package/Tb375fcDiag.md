# TB375FC diagnostic and storage transport

The DEVELOPMENT kernel feeds independent console and msgbuf tails into the
recorder through `Tb375fcDiagGlue.c` and the device patch series. Stage and panic
callbacks preserve text without replacing the existing console.

## Hardware ownership and storage

- The recorder is the only UFS controller owner.
- The experimental IOKit root adapter calls `tb375fc_ufs_read_block()` and shares
  the recorder's lock, transfer list and DMA buffer. It must not take over the
  controller independently.
- Diagnostic writes are restricted to two reserved TBKL slots inside mrdump:
  LUN2 base block 31170552, relative slots 64000 and 64009, nine 4096-byte blocks
  each. The root adapter itself exposes no storage writes.
- Each slot is committed header-last with length and CRC checks. This detects
  incomplete records; it does not promise power-failure atomicity.
- A busy transport returns BUSY; the reader may retry. Failure disables further
  transport use. Panic callbacks do not wait on a held lock.

## Integration

Early initialization follows PE initialization. DMA/MMIO mapping starts only
after `arm_vm_init` returns. A kernel thread polls once a second; phase and panic
callbacks try to flush immediately. Glue feeds the watchdog for a bounded
120-second observation window, never indefinitely.

The buffers retain separate console/msgbuf tails. Logging hooks append only;
UFS I/O does not run under the BSD msgbuf lock. Clear or match slot headers for
each new experiment instead of treating an old record as current evidence.

The earlier diagnostic integration reached root-device selection on hardware.
The new shared READ(10) path and IOKit publication require separate validation;
a successful build is not a successful APFS mount.
