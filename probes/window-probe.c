// SPDX-License-Identifier: BSD-2-Clause-Patent
// Own diagnostic kernel, not XNU. Only writes allocated RAM and the GOP buffer.
#include <stdint.h>
#include <stddef.h>
#include "DiagFont.h"

#ifndef PROBE_MEMSIZE_ACTUAL_OFFSET
#define PROBE_MEMSIZE_ACTUAL_OFFSET 0x478
#endif

static const uint64_t font[] = GLYPH_FONT;
static volatile uint32_t *fb;
static uint64_t stride, width, height;

static void flush(const volatile void *base, uint64_t bytes) {
  uint64_t ctr;
  __asm__ volatile("mrs %0,ctr_el0" : "=r"(ctr));
  uint64_t line = 4ULL << ((ctr >> 16) & 15);
  uint64_t start = (uint64_t)(uintptr_t)base & ~(line - 1);
  uint64_t end = (uint64_t)(uintptr_t)base + bytes;
  __asm__ volatile("dsb sy" ::: "memory");
  for (; start < end; start += line)
    __asm__ volatile("dc civac,%0" :: "r"(start) : "memory");
  __asm__ volatile("dsb sy" ::: "memory");
}

static void text(const char *s, unsigned row) {
  unsigned x = 18, y = 180 + row * 48, scale = 3;
  for (unsigned dy = 0; dy < 42 && y + dy < height; dy++)
    for (unsigned dx = 0; dx < width; dx++) fb[(y + dy) * stride + dx] = 0;
  while (*s) {
    unsigned ch = (unsigned char)*s++;
    if (ch < 32 || ch > 126 || x + 18 > width || y + 42 > height) continue;
    uint64_t glyph = font[ch - 32];
    for (unsigned gy = 0; gy < 14; gy++)
      for (unsigned gx = 0; gx < 6; gx++) {
        uint64_t bits = gy < 6 ? (glyph >> 32) >> (gy * 5) :
                        gy < 12 ? (glyph & 0xffffffffULL) >> ((gy - 6) * 5) : 0;
        uint32_t colour = gy < 12 && gx < 5 && ((bits >> gx) & 1) ? 0x00ffffff : 0;
        for (unsigned dy = 0; dy < scale; dy++)
          for (unsigned dx = 0; dx < scale; dx++)
            fb[(y + gy * scale + dy) * stride + x + gx * scale + dx] = colour;
      }
    x += 18;
  }
  if (y < height) flush(fb + y * stride, (height - y < 42 ? height - y : 42) * stride * 4);
}

static void value(const char *label, uint64_t number, unsigned row) {
  char line[64]; unsigned i = 0;
  while (label[i] && i < 40) { line[i] = label[i]; i++; }
  line[i++] = ' '; line[i++] = '0'; line[i++] = 'x';
  for (int n = 15; n >= 0; n--) line[i++] = "0123456789ABCDEF"[(number >> (n * 4)) & 15];
  line[i] = 0; text(line, row);
}

static void finish(unsigned failure) {
  text(failure ? "PONG WINDOW FAIL" : "PONG WINDOW PASS", 0);
  if (failure) value("ERROR", failure, 1);
  else text("256 MiB / EL1 / MMU OFF", 1);
  for (unsigned y = 64; y < 96 && y < height; y++)
    for (unsigned x = 0; x < width; x++)
      fb[y * stride + x] = failure ? 0x00df2020 : 0x0012d4a6;
  flush(fb + 64 * stride, 32 * stride * 4);
  for (;;) __asm__ volatile("wfe");
}

void probe_exception(uint64_t esr, uint64_t far, uint64_t elr, const uint64_t *a) {
  fb = (volatile uint32_t *)(uintptr_t)a[5];
  stride = a[7] / 4; width = a[8]; height = a[9];
  if (fb && width && width <= 8192 && height >= 512 && height <= 8192 &&
      stride >= width && stride <= 16384 && a[10] == 32) {
    text("PONG EXCEPTION", 0);
    value("ESR", esr, 1); value("FAR", far, 2); value("ELR", elr, 3);
    for (unsigned y = 64; y < 96; y++)
      for (unsigned x = 0; x < width; x++) fb[y * stride + x] = 0x00aa33dd;
    flush(fb + 64 * stride, 32 * stride * 4);
  }
  for (;;) __asm__ volatile("wfe");
}

void probe_main(const uint8_t *args) {
  const uint64_t *a = (const uint64_t *)args;
  uint64_t sctlr, el, frequency;
  __asm__ volatile("mrs %0,sctlr_el1" : "=r"(sctlr));
  __asm__ volatile("mrs %0,CurrentEL" : "=r"(el));
  __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(frequency));
  fb = (volatile uint32_t *)(uintptr_t)a[5];
  stride = a[7] / 4; width = a[8]; height = a[9];
  // There is no safe way to draw a failure without a valid boot video.
  if (!fb || !width || width > 8192 || height < 512 || height > 8192 ||
      a[7] % 4 || stride < width || stride > 16384 || a[10] != 32)
    for (;;) __asm__ volatile("wfe");
  for (unsigned y = 0; y < height; y++)
    for (unsigned x = 0; x < width; x++) fb[y * stride + x] = 0;
  flush(fb, stride * height * 4);
  text("PONG HANDOFF ENTERED", 0);
  text("STEP 1: WINDOW PARAMETERS", 2);
  value("WINDOW BASE", a[2], 3); value("WINDOW SIZE", a[3], 4);
  text("STEP 2: CPU REGISTERS", 2);
  if (args[0x6c] == '!') __asm__ volatile("brk #0x504");
  value("CNTFRQ", frequency, 5); value("SCTLR", sctlr, 6);
  text("Own probe; iOS 16 XNU not started", 7);
  text("STEP 3: BOOT ARGS", 2);
  if (*(const uint32_t *)args != 0x00020002 || el != 4 || (sctlr & 0x1005)) finish(1);
  if (a[3] != 0x10000000 || a[PROBE_MEMSIZE_ACTUAL_OFFSET / 8] != a[3] ||
      (a[1] & 0x1fffff) != (a[2] & 0x1fffff)) finish(2);
  uint64_t end = a[2] + a[3], used = a[4];
  if (end < a[2] || (uint64_t)(uintptr_t)args < a[2] ||
      used <= (uint64_t)(uintptr_t)args || used >= end - 0x10000) finish(3);
  text("STEP 4: APPLE DEVICE TREE", 2);
  uint64_t dt = a[12] - a[1] + a[2];
  uint32_t dtlen = *(const uint32_t *)(args + 0x68);
  if (dt < a[2] || dt >= used || !dtlen || dtlen > used - dt ||
      !*(const uint32_t *)(uintptr_t)dt) finish(4);
  // Check zero-fill of the final data page, matching the returned kernel's span.
  text("STEP 5: ZERO FILL", 2);
  volatile uint64_t *zero = (volatile uint64_t *)(uintptr_t)(a[2] + PROBE_KERNEL_SPAN - 0x1000);
  for (unsigned i = 0; i < 512; i++) if (zero[i]) finish(5);
  // Two patterns at every page in the unused window; exclude our high stack.
  uint64_t start = (used + 0xfff) & ~0xfffULL, limit = end - 0x10000;
  for (unsigned pass = 0; pass < 2; pass++) {
    uint64_t pattern = pass ? 0xaa55aa55aa55aa55ULL : 0x55aa55aa55aa55aaULL;
    text(pass ? "STEP 8: WRITE PATTERN 2" : "STEP 6: WRITE PATTERN 1", 2);
    for (uint64_t p = start; p < limit; p += 0x1000) {
      if (!(p & 0xffffff)) value("WRITE ADDRESS", p, 8);
      *(volatile uint64_t *)(uintptr_t)p = p ^ pattern;
      __asm__ volatile("dc civac,%0" :: "r"(p) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
    text(pass ? "STEP 9: READ PATTERN 2" : "STEP 7: READ PATTERN 1", 2);
    for (uint64_t p = start; p < limit; p += 0x1000) {
      if (!(p & 0xffffff)) value("READ ADDRESS", p, 8);
      __asm__ volatile("dc ivac,%0\n\tdsb sy" :: "r"(p) : "memory");
      if (*(volatile uint64_t *)(uintptr_t)p != (p ^ pattern)) finish(6);
    }
  }
  value("PAGES CHECKED", (limit - start) / 0x1000, 8);
  finish(0);
}
