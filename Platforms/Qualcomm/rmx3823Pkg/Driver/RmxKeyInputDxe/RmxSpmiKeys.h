/** @file
  Hardware keys of the Xiaomi Pad 6S Pro through the SPMI PMIC arbiter, as pure register-pointer code (host-testable).

  Facts, all from the board devicetree and mainline Linux (drivers/input/misc/pm8941-pwrkey.c, drivers/pinctrl/qcom/pinctrl-spmi-gpio.c):
    power key   PMIC SID 0, PON peripheral PID 0x13 (reg 0x1300), PON_RT_STS at +0x10, PON_GEN3_KPDPWR_N_SET = bit 7, set = pressed
    volume down PMIC SID 0, same register, PON_GEN3_RESIN_N_SET = bit 6, set = pressed (the board's linux,code 114)
    volume up   PM8550 (SID 1) GPIO6, peripheral PID 0x88 + 5 = 0x8d, RT_STS at +0x10 bit 0; gpio-keys flags 1 = active low
  The arbiter layout (version 7, "core" 0xc400000, "cnfg" 0xc42d000, "obsrvr" 0xc440000, EE 0) is the one the kernel side already reads
  on this hardware (BootResetCore.h): an APID is found by its PPID (SID << 8 | PID) in the APID map, and a register byte is read with an
  extended-read command on the observer channel of that APID.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#ifndef RMX_SPMI_KEYS_H_
#define RMX_SPMI_KEYS_H_

#include <stdint.h>

enum {
  RK_VERSION_V7_MIN = 0x70000000u, RK_VERSION_V8_MIN = 0x80000000u,
  RK_FEATURES = 0x04, RK_FEATURES_APIDS_MASK = 0x7ffu,
  RK_APID_MAP = 0x2000,
  RK_OBS_EE_STRIDE = 0x8000, RK_OBS_APID_STRIDE = 0x20,
  RK_CMD = 0x00, RK_STATUS = 0x08, RK_RDATA0 = 0x18,
  RK_OP_EXT_READL = 1,
  RK_STATUS_DONE = 1u << 0, RK_STATUS_ERRORS = 0xeu,
  RK_POLLS = 100000,
  RK_PPID_PON = 0x013, RK_PPID_GPIO6 = 0x18d, RK_REG_RT_STS = 0x10,
  RK_PON_RESIN = 1u << 6, RK_PON_KPDPWR = 1u << 7,
  RK_KEY_POWER = 1u << 0, RK_KEY_VOLDOWN = 1u << 1, RK_KEY_VOLUP = 1u << 2
};

struct rk_arbiter {
  volatile uint32_t *core;   // 0x3000 bytes
  volatile uint32_t *obs;    // observer channels of this EE and above (EE 0 starts at the base)
  uint8_t ee;
};

// 0 and *apid on success. 1 unknown layout, 2 no APIDs, 3 PPID not mapped.
static inline int rk_find_apid (const struct rk_arbiter *a, uint32_t ppid, int *apid)
{
  uint32_t version = a->core[0], apids = a->core[RK_FEATURES / 4] & RK_FEATURES_APIDS_MASK;
  *apid = -1;
  if (version < RK_VERSION_V7_MIN || version >= RK_VERSION_V8_MIN) return 1;
  if (!apids || apids > 1024) return 2;
  for (uint32_t i = 0; i < apids; ++i) {
    uint32_t map = a->core[(RK_APID_MAP + 4 * i) / 4];
    if (map && ((map >> 8) & 0xfff) == ppid) { *apid = (int) i; break; }
  }
  return *apid < 0 ? 3 : 0;
}

// One register byte through the observer channel. 0 ok, 1 timeout, 2 the arbiter reported an error.
static inline int rk_read8 (const struct rk_arbiter *a, int apid, uint8_t reg, uint8_t *value)
{
  volatile uint32_t *ch = a->obs + (RK_OBS_EE_STRIDE * a->ee + RK_OBS_APID_STRIDE * (uint32_t) apid) / 4;
  ch[RK_CMD / 4] = ((uint32_t) RK_OP_EXT_READL << 27) | ((uint32_t) reg << 4);
  for (unsigned n = 0; n < RK_POLLS; ++n) {
    uint32_t s = ch[RK_STATUS / 4];
    if (s & RK_STATUS_DONE) {
      if (s & RK_STATUS_ERRORS) return 2;
      *value = (uint8_t) ch[RK_RDATA0 / 4];
      return 0;
    }
  }
  return 1;
}

struct rk_keys { int pon, gpio; };   // APIDs, -1 = not available

static inline int rk_open (const struct rk_arbiter *a, struct rk_keys *k)
{
  int rc = rk_find_apid (a, RK_PPID_PON, &k->pon);
  if (rc) return rc;
  if (rk_find_apid (a, RK_PPID_GPIO6, &k->gpio)) k->gpio = -1;   // the power and volume-down keys still work without it
  return 0;
}

// Pressed keys as RK_KEY_* bits. 0 ok; non-zero when a read failed (nothing is reported pressed then).
static inline int rk_sample (const struct rk_arbiter *a, const struct rk_keys *k, uint8_t *pressed)
{
  uint8_t v = 0;
  *pressed = 0;
  int rc = rk_read8 (a, k->pon, RK_REG_RT_STS, &v);
  if (rc) return rc;
  if (v & RK_PON_KPDPWR) *pressed |= RK_KEY_POWER;
  if (v & RK_PON_RESIN) *pressed |= RK_KEY_VOLDOWN;
  if (k->gpio >= 0) {
    rc = rk_read8 (a, k->gpio, RK_REG_RT_STS, &v);
    if (rc) { *pressed = 0; return rc; }
    if (!(v & 1)) *pressed |= RK_KEY_VOLUP;       // active low
  }
  return 0;
}

// Debounce and auto-repeat. A key counts as pressed after two identical samples. Volume keys repeat while held: first after
// `delay` polls, then every `period` polls. step() returns the keys to report now (new presses plus repeats).
struct rk_filter { uint8_t last, stable; uint16_t held[3]; };

static inline uint8_t rk_step (struct rk_filter *f, uint8_t sample, uint16_t delay, uint16_t period)
{
  uint8_t out = 0;
  if (sample == f->last) {
    uint8_t fresh = (uint8_t) (sample & ~f->stable);
    f->stable = sample;
    out = fresh;
    for (unsigned i = 0; i < 3; ++i) {
      uint8_t bit = (uint8_t) (1u << i);
      if (!(sample & bit)) { f->held[i] = 0; continue; }
      if (fresh & bit) { f->held[i] = 0; continue; }
      ++f->held[i];
      if (bit != RK_KEY_POWER && f->held[i] >= delay && (f->held[i] - delay) % period == 0) out |= bit;
    }
  }
  f->last = sample;
  return out;
}

#endif
