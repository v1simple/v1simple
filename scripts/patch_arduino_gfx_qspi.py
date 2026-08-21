"""Apply the v1simple Arduino_GFX QSPI full-frame flush patch (pre-build).

Upstream ``Arduino_ESP32QSPI::writePixels`` (GFX Library for Arduino 1.6.7)
packs one chunk of pixels into ``_buffer32`` and then blocks on
``POLL_START()``/``POLL_END()`` before packing the next, so the CPU byte-swap
and the QSPI DMA transfer never overlap. It also reads the source framebuffer
two 16-bit pixels at a time.

Both cost real time on the full-canvas flush. A 640x172 push is 110,080
pixels / 220,160 bytes; at ESP32QSPI_FREQUENCY=80 MHz over four lanes the wire
time is ~5.5 ms, but the measured flush subphase runs 33-57 ms
(``displayFlushSubphaseMax_us`` in the bench perf CSVs). The remainder is the
pack loop against the PSRAM framebuffer plus per-transaction driver overhead.
Raising ESP32QSPI_MAX_PIXELS_AT_ONCE 1024 -> 4096 (commit 683f944) cut 108
transactions to 27 and moved max flush ~57 -> ~47 ms, which puts per-transaction
overhead near 123 us and leaves ~44 ms in the pack-and-transfer body.

This matters to the schema-47 notification-to-display-pipeline-completion
measurement because that interval ends only after the triggered display
pipeline returns. A full-frame flush performed by that pipeline is therefore
inside the measured interval. The schema-46 dispatch-time observation used
different semantics and cannot be compared with it.

The patch makes two changes to writePixels and nothing else:

  1. Ping-pong across ``_buffer32`` and ``_2nd_buffer32`` so chunk N+1 is packed
     while chunk N is on the wire. ``_2nd_buffer`` is already allocated in
     ``begin()`` (which returns false if the allocation fails, so it is never
     null here) and is otherwise never read anywhere in the class. Each
     in-flight chunk gets its own transaction descriptor because
     ``spi_device_polling_end()`` reads back the transaction it was started
     with -- the shared ``_spi_tran_ext`` cannot be reused mid-transfer. It stays
     the zeroed template instead; every other method in the class sets
     flags/cmd/addr/tx_buffer/length before starting, so leaving it unmutated
     is safe.

  2. Pack 32 bits per iteration when the source pointer is 4-byte aligned,
     which is the full-canvas flush case. ``MSB_32_16_16_SET`` byte-swaps each
     16-bit pixel and stores the pair in order; on a little-endian word holding
     both pixels that is exactly
     ``((v & 0x00FF00FF) << 8) | ((v >> 8) & 0x00FF00FF)``. Unaligned callers
     (row-sliced bitmaps from ``Arduino_TFT::draw16bitRGBBitmap``) keep the
     original 16-bit loop, because Xtensa would trap-and-emulate a wide
     unaligned load and lose more than the pairing saves.

Byte order on the wire is unchanged on both paths, and the odd-pixel tail is
handled exactly as upstream.

Fail-closed: if the vendored source matches neither the upstream function nor
the patched marker, the build stops -- do not build with an unknown QSPI
databus.
"""

Import("env")  # noqa: F821  (SCons construction environment)

from pathlib import Path

MARKER = "v1simple-gfx-qspi-flush-patch-v1"

UPSTREAM = """void Arduino_ESP32QSPI::writePixels(uint16_t *data, uint32_t len)
{

  CS_LOW();
  uint32_t l, l2;
  uint16_t p1, p2;
  bool first_send = true;
  while (len)
  {
    l = (len > ESP32QSPI_MAX_PIXELS_AT_ONCE) ? ESP32QSPI_MAX_PIXELS_AT_ONCE : len;

    if (first_send)
    {
      _spi_tran_ext.base.flags = SPI_TRANS_MODE_QIO;
      _spi_tran_ext.base.cmd = 0x32;
      _spi_tran_ext.base.addr = 0x003C00;
      first_send = false;
    }
    else
    {
      _spi_tran_ext.base.flags = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD |
                                 SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
    }
    l2 = l >> 1;
    for (uint32_t i = 0; i < l2; ++i)
    {
      p1 = *data++;
      p2 = *data++;
      MSB_32_16_16_SET(_buffer32[i], p1, p2);
    }
    if (l & 1)
    {
      p1 = *data++;
      MSB_16_SET(_buffer16[l - 1], p1);
    }

    _spi_tran_ext.base.tx_buffer = _buffer32;
    _spi_tran_ext.base.length = l << 4;

    POLL_START();
    POLL_END();

    len -= l;
  }
  CS_HIGH();
}"""

PATCHED = """void Arduino_ESP32QSPI::writePixels(uint16_t *data, uint32_t len)
{
  // [v1simple-gfx-qspi-flush-patch-v1] Flush cost only -- the bytes placed on
  // the wire are identical to upstream. See scripts/patch_arduino_gfx_qspi.py.
  //
  // Upstream packed a chunk into _buffer32 and then blocked in
  // POLL_START()/POLL_END() before packing the next, so the CPU byte-swap and
  // the DMA transfer never overlapped. Two changes:
  //
  //   1. Ping-pong across _buffer32 and _2nd_buffer32 (allocated in begin(),
  //      otherwise never read) so chunk N+1 is packed while chunk N is on the
  //      wire. Each in-flight chunk needs its own descriptor because
  //      spi_device_polling_end() reads back the transaction it was started
  //      with; _spi_tran_ext stays the zeroed template, which is safe because
  //      every other method sets flags/cmd/addr/tx_buffer/length before
  //      starting rather than inheriting leftovers.
  //   2. Pack 32 bits at a time when the source is 4-byte aligned (the
  //      full-canvas flush). Unaligned callers keep the 16-bit loop; Xtensa
  //      would trap-and-emulate the wide load.
  CS_LOW();
  uint32_t *dma_buffer[2] = {_buffer32, _2nd_buffer32};
  spi_transaction_ext_t tran[2];
  uint32_t l, l2;
  uint16_t p1, p2;
  uint8_t slot = 0;
  bool in_flight = false;
  bool first_send = true;
  while (len)
  {
    l = (len > ESP32QSPI_MAX_PIXELS_AT_ONCE) ? ESP32QSPI_MAX_PIXELS_AT_ONCE : len;

    // Safe to overwrite: dma_buffer[slot] last carried the chunk whose
    // transfer was ended at the top of the previous iteration.
    uint32_t *dst32 = dma_buffer[slot];
    uint16_t *dst16 = (uint16_t *)dst32;
    l2 = l >> 1;
    if ((((uintptr_t)data) & 3u) == 0u)
    {
      const uint32_t *src32 = (const uint32_t *)data;
      for (uint32_t i = 0; i < l2; ++i)
      {
        uint32_t v = src32[i];
        dst32[i] = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
      }
      data += (l2 << 1);
    }
    else
    {
      for (uint32_t i = 0; i < l2; ++i)
      {
        p1 = *data++;
        p2 = *data++;
        MSB_32_16_16_SET(dst32[i], p1, p2);
      }
    }
    if (l & 1)
    {
      p1 = *data++;
      MSB_16_SET(dst16[l - 1], p1);
    }

    if (in_flight)
    {
      spi_device_polling_end(_handle, portMAX_DELAY);
      in_flight = false;
    }

    tran[slot] = _spi_tran_ext;
    if (first_send)
    {
      tran[slot].base.flags = SPI_TRANS_MODE_QIO;
      tran[slot].base.cmd = 0x32;
      tran[slot].base.addr = 0x003C00;
      first_send = false;
    }
    else
    {
      tran[slot].base.flags = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD |
                              SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
    }
    tran[slot].base.tx_buffer = dst32;
    tran[slot].base.length = l << 4;

    spi_device_polling_start(_handle, &tran[slot].base, portMAX_DELAY);
    in_flight = true;

    len -= l;
    slot ^= 1u;
  }
  if (in_flight)
  {
    spi_device_polling_end(_handle, portMAX_DELAY);
  }
  CS_HIGH();
}"""


def _fail(message: str) -> None:
    print(f"[patch_arduino_gfx_qspi] ERROR: {message}")
    env.Exit(1)


def apply_patch() -> None:
    source = (
        Path(env.subst("$PROJECT_LIBDEPS_DIR"))
        / env["PIOENV"]
        / "GFX Library for Arduino"
        / "src"
        / "databus"
        / "Arduino_ESP32QSPI.cpp"
    )
    if not source.exists():
        _fail(
            f"{source} not found. If dependencies have not been installed yet, "
            "run `pio pkg install` (or `pio run` once) and build again -- the "
            "firmware must not be built with an unpatched QSPI databus."
        )

    # newline="" disables universal-newline translation on both read and write:
    # the vendored file ships CRLF and must be handed back byte-for-byte apart
    # from the replaced function. Path.read_text/write_text only accept
    # newline= on 3.13+, so go through open() to stay valid on 3.10.
    with source.open("r", encoding="utf-8", newline="") as handle:
        raw = handle.read()
    crlf = "\r\n" in raw
    text = raw.replace("\r\n", "\n")

    if MARKER in text:
        print("[patch_arduino_gfx_qspi] already applied (v1)")
        return

    if UPSTREAM not in text:
        _fail(
            "vendored Arduino_ESP32QSPI.cpp does not match the expected "
            "upstream writePixels and carries no patch marker. The pinned "
            "GFX Library for Arduino version changed; re-evaluate the patch "
            "before building (see this script's header)."
        )

    text = text.replace(UPSTREAM, PATCHED, 1)
    with source.open("w", encoding="utf-8", newline="") as handle:
        handle.write(text.replace("\n", "\r\n") if crlf else text)
    print("[patch_arduino_gfx_qspi] applied full-frame flush patch")


apply_patch()
