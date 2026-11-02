/* Pong adaptation: NothingOSS qti_battery_charger.c standard property IDs; no Xiaomi extensions or charger writes. */
/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "ShengBattmgr.h"

/* NothingOSS qti_battery_charger.c: TEMP=12 (deci-C), CHG_COUNTER=14,
 * CYCLE_COUNT=15, FULL_DESIGN=16, FULL=17, TTF=19, TTE=20.
 * USB_ADAP_TYPE=7 is POWER_SUPPLY_USB_TYPE; USB_TYPE=6 is not that enum.
 * No Xiaomi BOARD_GET properties are requested on this single-pack board. */
static const struct {uint8_t opcode, property, slow;} kFields[BmFields] = {
    [BmStatus] = {SHENG_BATTMGR_OP_BATTERY_GET, 0, 0},
    [BmPresent] = {SHENG_BATTMGR_OP_BATTERY_GET, 2, 0},
    [BmCapacity] = {SHENG_BATTMGR_OP_BATTERY_GET, 4, 0},
    [BmVoltage] = {SHENG_BATTMGR_OP_BATTERY_GET, 7, 0},
    [BmCurrent] = {SHENG_BATTMGR_OP_BATTERY_GET, 9, 0},
    [BmTemperature] = {SHENG_BATTMGR_OP_BATTERY_GET, 12, 0},
    [BmCharge] = {SHENG_BATTMGR_OP_BATTERY_GET, 14, 0},
    [BmUsbOnline] = {SHENG_BATTMGR_OP_USB_GET, 0, 0},
    [BmUsbType] = {SHENG_BATTMGR_OP_USB_GET, 6, 0},
    [BmUsbVoltage] = {SHENG_BATTMGR_OP_USB_GET, 1, 0},
    [BmUsbCurrent] = {SHENG_BATTMGR_OP_USB_GET, 3, 0},
    [BmTimeToEmpty] = {SHENG_BATTMGR_OP_BATTERY_GET, 20, 0},
    [BmTimeToFull] = {SHENG_BATTMGR_OP_BATTERY_GET, 19, 0},
    /* the fuel gauges: remaining and full charge of each (mAh) */
    [BmGauge1Remaining] = {SHENG_BATTMGR_OP_BOARD_GET, 0x8a, 0},
    [BmGauge1Full] = {SHENG_BATTMGR_OP_BOARD_GET, 0x8b, 0},
    [BmGauge2Remaining] = {SHENG_BATTMGR_OP_BOARD_GET, 0xa6, 0},
    [BmGauge2Full] = {SHENG_BATTMGR_OP_BOARD_GET, 0xa7, 0},
    [BmFull] = {SHENG_BATTMGR_OP_BATTERY_GET, 17, 1},
    [BmDesign] = {SHENG_BATTMGR_OP_BATTERY_GET, 16, 1},
    [BmCycles] = {SHENG_BATTMGR_OP_BATTERY_GET, 15, 1},
    [BmUsbAdapter] = {SHENG_BATTMGR_OP_USB_GET, 7, 0},
    [BmUsbLimit] = {SHENG_BATTMGR_OP_USB_GET, 5, 0},
    [BmUsbMax] = {SHENG_BATTMGR_OP_USB_GET, 4, 0},
    [BmChargeType] = {SHENG_BATTMGR_OP_BATTERY_GET, 3, 0},
    [BmThermalLevel] = {SHENG_BATTMGR_OP_BATTERY_GET, 10, 0},
    [BmChargeCurrent] = {SHENG_BATTMGR_OP_BATTERY_GET, 12, 0},
    [BmRealType] = {SHENG_BATTMGR_OP_BOARD_GET, 0x07, 0},
    [BmPowerMax] = {SHENG_BATTMGR_OP_BOARD_GET, 0x33, 0},
    [BmPdVerified] = {SHENG_BATTMGR_OP_BOARD_GET, 0x18, 0},
    [BmAuthentic] = {SHENG_BATTMGR_OP_BOARD_GET, 0x03, 1},
    [BmSlaveAuthentic] = {SHENG_BATTMGR_OP_BOARD_GET, 0xa5, 1},
    [BmInputSuspend] = {SHENG_BATTMGR_OP_BOARD_GET, 0x2a, 0},
};
#define NO_CYCLE 0xff

static void put32(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);}
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static void header(uint8_t *out, uint32_t opcode)
{
    put32(out, SHENG_BATTMGR_OWNER);
    put32(out + 4, SHENG_BATTMGR_TYPE_REQUEST);
    put32(out + 8, opcode);
}

void sheng_battmgr_init(ShengBattmgr *b)
{
    for (unsigned i = 0; i < sizeof *b; ++i) ((uint8_t *)b)[i] = 0;
    b->cursor = NO_CYCLE;
    b->last_timeout = -1;
    b->sdp_limit_ua = 0;  /* a configured USB 3 port's 900 mA; the firmware's current limiting backs off if the port sags */
    b->limit_port = -1;
}

uint32_t sheng_battmgr_enable_request(uint8_t out[SHENG_BATTMGR_ENABLE_BYTES])
{
    header(out, SHENG_BATTMGR_OP_REQUEST_NOTIFICATION);
    /* Nothing's BC_SET_NOTIFY_REQ uses MSG_TYPE_NOTIFY, unlike property
     * requests. TYPE_REQUEST can leave the firmware subscription inactive. */
    put32(out + 4, SHENG_BATTMGR_TYPE_NOTIFY);
    put32(out + 12, 0);    /* battery 0 */
    put32(out + 16, 0);    /* power state */
    put32(out + 20, 1);    /* low capacity notification threshold, percent */
    put32(out + 24, 100);  /* high */
    return SHENG_BATTMGR_ENABLE_BYTES;
}

static void skip_to_wanted(ShengBattmgr *b)
{
    // Nothing's single battery does not implement Xiaomi BOARD_GET fields.
    while(b->cursor<BmFields &&
       (kFields[b->cursor].opcode==SHENG_BATTMGR_OP_BOARD_GET ||
        b->cursor==BmChargeCurrent ||
        (kFields[b->cursor].slow&&!b->slow_due)))b->cursor++;
}

static void advance(ShengBattmgr *b)
{
    b->in_flight = 0;
    b->cursor++;
    skip_to_wanted(b);
}

void sheng_battmgr_refresh(ShengBattmgr *b, int slow)
{
    if (b->cursor != NO_CYCLE) {b->refresh_wanted = 1 + (slow != 0); return;}
    b->slow_due = slow || !(b->valid & (1u << BmFull));
    b->cursor = 0;
    b->fresh = 0;
    b->in_flight = 0;
    skip_to_wanted(b);
}

uint32_t sheng_battmgr_next(ShengBattmgr *b, uint8_t out[SHENG_BATTMGR_REQUEST_BYTES])
{
    if (b->in_flight || b->limit_in_flight) return 0;
    if (b->limit_due) {
        header(out, SHENG_BATTMGR_OP_USB_SET);
        put32(out + 12, 0);
        put32(out + 16, 5);  /* input current limit */
        put32(out + 20, b->limit_due);
        b->limit_in_flight = b->limit_due;
        b->limit_due = 0;
        b->limit_tries++;
        return SHENG_BATTMGR_REQUEST_BYTES;
    }
    if (b->cursor == NO_CYCLE || b->cursor >= BmFields) return 0;
    header(out, kFields[b->cursor].opcode);
    put32(out + 12, 0);
    put32(out + 16, kFields[b->cursor].property);
    put32(out + 20, 0);
    b->in_flight = 1;
    return SHENG_BATTMGR_REQUEST_BYTES;
}

/* The input current limit a port type gets from the host side: a standard port (SDP 1) the configured value, a charging downstream port
 * (CDP 3) its 1.5 A; others (dedicated chargers, Type-C current, PD contracts) are the firmware's own. */
static uint32_t wanted_limit(const ShengBattmgr *b, int port)
{
    (void)b;(void)port;return 0; /* Charging limits remain firmware-owned. */
}

/* After a cycle: a set is due when the port wants a higher limit than the firmware reports, at most three tries per port type. */
static void check_limit(ShengBattmgr *b)
{
    const int port = (b->valid >> BmUsbAdapter) & 1 ? b->value[BmUsbAdapter] : (b->valid >> BmUsbType) & 1 ? b->value[BmUsbType] : 0;
    if (port != b->limit_port) {b->limit_port = port; b->limit_tries = 0;}
    const uint32_t want = wanted_limit(b, port);
    if (!want || b->limit_tries >= 3) return;
    if ((b->valid >> BmUsbLimit) & 1 && b->value[BmUsbLimit] >= (int32_t)want - 100000) return;  /* 100 mA under it: input limiting */
    b->limit_due = want;
}

/* Ends the cycle when the cursor ran past the last field; starts the remembered one. Returns 1 when a cycle ended. */
static int finish(ShengBattmgr *b)
{
    if (b->cursor == NO_CYCLE || b->cursor < BmFields) return 0;
    b->cursor = NO_CYCLE;
    b->cycles_done++;
    b->completed = b->fresh;
    check_limit(b);
    if (b->refresh_wanted) {
        const int slow = b->refresh_wanted == 2;
        b->refresh_wanted = 0;
        sheng_battmgr_refresh(b, slow);
    }
    return 1;
}

void sheng_battmgr_timeout(ShengBattmgr *b)
{
    if (b->limit_in_flight) {b->limit_in_flight = 0; b->limit_result = -60; b->timeouts++; return;}
    if (b->cursor == NO_CYCLE || !b->in_flight) return;
    b->timeouts++;
    b->last_timeout = b->cursor;
    b->valid &= ~(1u << b->cursor);
    advance(b);
    finish(b);
}

int sheng_battmgr_receive(ShengBattmgr *b, const uint8_t *data, uint32_t length)
{
    if (length < 12 || le32(data) != SHENG_BATTMGR_OWNER) {b->ignored++; return BmIgnored;}
    const uint32_t type = le32(data + 4), opcode = le32(data + 8);
    if (opcode == SHENG_BATTMGR_OP_NOTIFICATION) {
        if(type != SHENG_BATTMGR_TYPE_NOTIFY || length < 16) {b->ignored++; return BmIgnored;}
        b->notifications++;
        sheng_battmgr_refresh(b, 0);
        return BmNotified;
    }
    if (opcode == SHENG_BATTMGR_OP_REQUEST_NOTIFICATION &&
        (type == SHENG_BATTMGR_TYPE_REQUEST || type == SHENG_BATTMGR_TYPE_NOTIFY)) {
        b->enabled = 1; return BmEnabled;
    }
    if (type != SHENG_BATTMGR_TYPE_REQUEST) {b->ignored++; return BmIgnored;}
    if (opcode == SHENG_BATTMGR_OP_USB_SET) {
        if (!b->limit_in_flight || length < 24) {b->ignored++; return BmIgnored;}
        b->limit_result = (int32_t)le32(data + 20);
        if (!b->limit_result) b->limit_sets++;
        b->limit_in_flight = 0;
        sheng_battmgr_refresh(b, 0);  /* read the limit back */
        return BmAnswer;
    }
    if (b->cursor == NO_CYCLE || b->cursor >= BmFields || !b->in_flight || length < 24) {b->ignored++; return BmIgnored;}
    const uint32_t property = le32(data + 12), value = le32(data + 16), result = le32(data + 20);
    if (opcode != kFields[b->cursor].opcode || property != kFields[b->cursor].property) {b->ignored++; return BmIgnored;}
    b->responses++;
    if (result) {
        b->errors++;
        b->valid &= ~(1u << b->cursor);
    } else {
        const int32_t signed_value=(int32_t)value;
        if(b->cursor==BmTemperature && (signed_value < -400 || signed_value > 900)) {
            b->errors++;b->valid &= ~(1u << b->cursor);
        } else {
            b->value[b->cursor]=b->cursor==BmTemperature?signed_value*10:signed_value;
            b->valid |= 1u << b->cursor;
            b->fresh |= 1u << b->cursor;
        }
    }
    advance(b);
    return finish(b) ? BmCycleDone : BmAnswer;
}

/* POWER_SUPPLY_USB_TYPE (unknown 0, SDP 1, DCP 2, CDP 3, ACA 4, C 5, PD 6, PD_DRP 7, PD_PPS 8) to the kIOPSFamilyCode values (the
 * USB family's error space, 0xe0004000 + n: host 0, dedicated charging port 4, downstream charging port 5, unknown 7, Type-C 9, PD 10). */
static uint32_t family(int type)
{
    switch (type) {
    case 1: return 0xe0004000u;
    case 2: return 0xe0004004u;
    case 3: return 0xe0004005u;
    case 5: return 0xe0004009u;
    case 6: case 7: case 8: return 0xe000400au;
    default: return 0xe0004007u;
    }
}

void sheng_battmgr_snapshot(const ShengBattmgr *b, ShengBatterySnapshot *s)
{
#define HAS(f) ((b->valid >> (f)) & 1u)
#define V(f) (b->value[(f)])
    const int status = HAS(BmStatus) ? V(BmStatus) : 0;  /* POWER_SUPPLY_STATUS: 1 charging, 2 discharging, 3 not charging, 4 full */
    s->present = HAS(BmPresent) ? V(BmPresent) != 0 : HAS(BmCapacity);
    // ONLINE is the charger's policy state, and can be zero when a full
    // battery stops drawing. A fresh, measured VBUS still proves physical
    // external power. Never infer its presence from stale voltage or type.
    const int voltage_known = (b->completed & (1u << BmUsbVoltage)) && HAS(BmUsbVoltage) &&
        V(BmUsbVoltage) >= 0 && V(BmUsbVoltage) <= 30000000;
    const int online_known = (b->completed & (1u << BmUsbOnline)) && HAS(BmUsbOnline);
    const int vbus = voltage_known && V(BmUsbVoltage) >= 4000000;
    // ONLINE=0 can mean a full pack has stopped drawing; without a
    // fresh voltage reply it is not evidence of physical disconnection.
    s->source_known = voltage_known || (online_known && V(BmUsbOnline) != 0);
    s->status_known = (b->completed & (1u << BmStatus)) && HAS(BmStatus);
    s->external = (online_known && V(BmUsbOnline) != 0) || vbus;
    const int gauges = HAS(BmGauge1Remaining) && HAS(BmGauge1Full) && HAS(BmGauge2Remaining) && HAS(BmGauge2Full);
    const int64_t remaining = gauges ? (int64_t)(uint32_t)V(BmGauge1Remaining) + (uint32_t)V(BmGauge2Remaining) : 0;
    const int64_t full = gauges ? (int64_t)(uint32_t)V(BmGauge1Full) + (uint32_t)V(BmGauge2Full) : 0;
    int percent = full > 0 ? (int)((remaining * 100 + full / 2) / full) : HAS(BmCapacity) ? (V(BmCapacity) + 50) / 100 : 0;
    s->percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    s->charge_known = full > 0 || HAS(BmCapacity);
    s->charging = s->external && status == 1;
    // Rounded capacity alone is not a completed charge indication. The
    // PMIC may still be charging at displayed 100%; preserve its status.
    s->fully_charged = s->external && status == 4;
    s->critical = s->present && !s->external && s->percent <= 2;
    s->voltage_mv = HAS(BmVoltage) ? V(BmVoltage) / 1000 : 0;
    s->current_ma = HAS(BmCurrent) ? -V(BmCurrent) / 1000 : 0;
    /* -40 to 90 C; the firmware answers a fixed out-of-range value when it has no reading */
    s->temperature_valid = HAS(BmTemperature) && V(BmTemperature) >= -4000 && V(BmTemperature) <= 9000;
    s->temperature = s->temperature_valid ? V(BmTemperature) : 0;
    s->raw_mah = HAS(BmCharge) ? V(BmCharge) / 1000 : 0;
    s->full_mah = HAS(BmFull) ? V(BmFull) / 1000 : 0;
    s->design_mah = HAS(BmDesign) ? V(BmDesign) / 1000 : 0;
    s->cycles = HAS(BmCycles) ? V(BmCycles) : 0;
    /* The gauge reports times in seconds, and a negative or huge value when it has none. */
    s->time_to_empty_min = HAS(BmTimeToEmpty) && V(BmTimeToEmpty) > 0 && V(BmTimeToEmpty) < 100 * 3600 ? V(BmTimeToEmpty) / 60 : -1;
    s->time_to_full_min = HAS(BmTimeToFull) && V(BmTimeToFull) > 0 && V(BmTimeToFull) < 100 * 3600 ? V(BmTimeToFull) / 60 : -1;
    if (s->external) {
        s->adapter_family = family(HAS(BmUsbAdapter) ? V(BmUsbAdapter) : 0);
        s->adapter_mv = HAS(BmUsbVoltage) ? V(BmUsbVoltage) / 1000 : 0;
        // AdapterDetails describes the available source, not the battery's
        // instantaneous draw (which is zero when the pack is full).
        s->adapter_ma = HAS(BmUsbMax) ? V(BmUsbMax) / 1000 : 0;
        if (HAS(BmUsbLimit) && V(BmUsbLimit) >= 0 &&
            (!s->adapter_ma || V(BmUsbLimit) / 1000 < s->adapter_ma))
            s->adapter_ma = V(BmUsbLimit) / 1000;
        if (s->adapter_mv < 0) s->adapter_mv = 0;
        if (s->adapter_ma < 0) s->adapter_ma = -s->adapter_ma;
        s->adapter_watts = (int)(((int64_t)s->adapter_mv * s->adapter_ma + 500000) / 1000000);
    } else {
        s->adapter_family = 0;
        s->adapter_mv = s->adapter_ma = s->adapter_watts = 0;
    }
#undef HAS
#undef V
}
