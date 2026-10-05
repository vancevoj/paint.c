# Lane W4-SAVECFG attribution notes

No Paint.NET 4.x or 5.x code was read or used, and nothing came from the
3.36 source either. The behavior comes from docs/inventory/MENUS.md (Save
Configuration dialog: "computing (p%)" progress, error states, the
standard error dialog) and FILES.md (DDS options); the wording of the size
label is paint.c's own (ADR-013).

## Library interfaces used as documented
- libjpeg-turbo: struct jpeg_progress_mgr (progress_monitor, pass_counter,
  pass_limit, completed_passes, total_passes) as described in its
  libjpeg.txt, "Progress monitoring".
- libwebp: WebPPicture.progress_hook and user_data, VP8_ENC_ERROR_USER_ABORT
  (webp/encode.h).
- libjxl: JxlEncoderSetParallelRunner with a runner that returns a nonzero
  JxlParallelRetCode to stop the encode (jxl/parallel_runner.h).

No dependency was added.
