# test-xip/platforms — reused from ../test/platforms

Board sources (startup/semihost/board/crt for the unified `board` platform and
the hosted `board-hosted` variant) come from `../test/platforms`. The active
board is `-DSCL_BOARD=nucleo|weact` (default `weact`); only board boot/init
differs between the two. The XIP apps reference them via the `scl-test-app`
helper exactly as `test/` does — no duplication.
