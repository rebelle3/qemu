/*
 * Samsung S5L8702 LCD interface controller (iPod Classic 6G).
 *
 * The controller forwards commands (LCD_WCMD) and data/pixels
 * (LCD_WDATA FIFO at +0x40, also the target of PL080 DMA) to the
 * attached panel.  The iPod 6G panels come in two flavours:
 *  - "cmdset16" (types 2/3): ILI9326-style 16-bit register writes;
 *    window regs 0x210-0x213, GRAM address 0x200/0x201, GRAM write
 *    command 0x202,
 *  - "cmdset8" (types 0/1): MIPI-DCS style; column/row address set
 *    0x2A/0x2B with four byte parameters, memory write 0x2C.
 * Both are implemented; the GPIO strap selects which one Rockbox uses.
 *
 * Pixels are RGB565, streamed after the GRAM-write command and routed
 * into a shadow framebuffer displayed via the QEMU console.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "ui/console.h"

#define TYPE_S5L8702_LCD "s5l8702-lcd"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8702LcdState, S5L8702_LCD)

#define LCD_WIDTH  320
#define LCD_HEIGHT 240

#define REG_CON      0x00
#define REG_WCMD     0x04
#define REG_RCMD     0x0c
#define REG_RDATA    0x10
#define REG_DBUFF    0x14
#define REG_INTCON   0x18
#define REG_STATUS   0x1c
#define REG_PHTIME   0x20
#define REG_RST_TIME 0x24
#define REG_DRV_RST  0x28
#define REG_WDATA    0x40   /* ..0x5c */

/* 16-bit command set registers */
#define R16_HORIZ_GRAM_ADDR  0x200
#define R16_VERT_GRAM_ADDR   0x201
#define R16_WRITE_GRAM       0x202
#define R16_HORIZ_START      0x210
#define R16_HORIZ_END        0x211
#define R16_VERT_START       0x212
#define R16_VERT_END         0x213

/* 8-bit command set */
#define R8_CASET   0x2a
#define R8_RASET   0x2b
#define R8_RAMWR   0x2c

struct S5L8702LcdState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    QemuConsole *con;

    uint32_t lcd_con;
    uint32_t intcon;
    uint32_t phtime;
    uint32_t rst_time;
    uint32_t drv_rst;

    /* panel state machine */
    uint32_t cur_cmd;
    int param_idx;
    uint8_t params[4];
    bool pixel_mode;

    /* window and GRAM pointer */
    uint16_t win_xs, win_xe, win_ys, win_ye;
    uint16_t gram_x, gram_y;

    /* readback (display ID etc.) */
    uint8_t rd_buf[4];
    int rd_idx;
    bool rd_ready;

    uint16_t fb[LCD_WIDTH * LCD_HEIGHT];
    bool dirty;
};

static void s5l8702_lcd_put_pixel(S5L8702LcdState *s, uint16_t pix)
{
    if (getenv("IPOD6G_DEBUG")) {
        static int px;
        if ((px++ % 76800) == 0) {
            fprintf(stderr, "PIXDBG n=%d at(%d,%d)\n", px, s->gram_x, s->gram_y);
        }
    }
    if (s->gram_x < LCD_WIDTH && s->gram_y < LCD_HEIGHT) {
        s->fb[s->gram_y * LCD_WIDTH + s->gram_x] = pix;
    }
    if (s->gram_x >= s->win_xe) {
        s->gram_x = s->win_xs;
        if (s->gram_y >= s->win_ye) {
            s->gram_y = s->win_ys;
        } else {
            s->gram_y++;
        }
    } else {
        s->gram_x++;
    }
    s->dirty = true;
}

static void s5l8702_lcd_command(S5L8702LcdState *s, uint32_t cmd)
{
    if (getenv("IPOD6G_DEBUG")) {
        fprintf(stderr, "LCDDBG cmd=%02x win=(%d,%d)-(%d,%d)\n",
                cmd, s->win_xs, s->win_ys, s->win_xe, s->win_ye);
    }
    s->cur_cmd = cmd;
    s->param_idx = 0;
    s->pixel_mode = false;

    switch (cmd) {
    case R16_WRITE_GRAM:
    case R8_RAMWR:
        s->gram_x = s->win_xs;
        s->gram_y = s->win_ys;
        s->pixel_mode = true;
        break;
    default:
        break;
    }
}

static void s5l8702_lcd_data(S5L8702LcdState *s, uint32_t val)
{
    if (s->pixel_mode) {
        s5l8702_lcd_put_pixel(s, val & 0xffff);
        return;
    }
    switch (s->cur_cmd) {
    /* 16-bit command set: one data word per register */
    case R16_HORIZ_GRAM_ADDR:
        s->gram_x = val & 0x1ff;
        break;
    case R16_VERT_GRAM_ADDR:
        s->gram_y = val & 0x1ff;
        break;
    case R16_HORIZ_START:
        s->win_xs = val & 0x1ff;
        break;
    case R16_HORIZ_END:
        s->win_xe = val & 0x1ff;
        break;
    case R16_VERT_START:
        s->win_ys = val & 0x1ff;
        break;
    case R16_VERT_END:
        s->win_ye = val & 0x1ff;
        break;

    /* 8-bit command set: four byte-wide parameters */
    case R8_CASET:
    case R8_RASET:
        if (s->param_idx < 4) {
            s->params[s->param_idx++] = val & 0xff;
        }
        if (s->param_idx == 4) {
            uint16_t start = (s->params[0] << 8) | s->params[1];
            uint16_t end = (s->params[2] << 8) | s->params[3];

            if (s->cur_cmd == R8_CASET) {
                s->win_xs = start;
                s->win_xe = end;
            } else {
                s->win_ys = start;
                s->win_ye = end;
            }
        }
        break;
    default:
        /* parameters of unmodelled panel commands: ignore */
        break;
    }
}

static uint64_t s5l8702_lcd_read(void *opaque, hwaddr offset, unsigned size)
{
    S5L8702LcdState *s = opaque;

    switch (offset) {
    case REG_CON:
        return s->lcd_con;
    case REG_STATUS:
        /* bit1: ready for config/cmd/data; bit4: FIFO full (never);
         * bit0: read data ready */
        return 0x2 | (s->rd_ready ? 0x1 : 0);
    case REG_DBUFF:
        s->rd_ready = false;
        /* panel returns bits shifted left by one (driver does >> 1) */
        return (uint32_t)s->rd_buf[s->rd_idx++ & 3] << 1;
    case REG_INTCON:
        return s->intcon;
    case REG_PHTIME:
        return s->phtime;
    default:
        return 0;
    }
}

static void s5l8702_lcd_write(void *opaque, hwaddr offset,
                              uint64_t val, unsigned size)
{
    S5L8702LcdState *s = opaque;

    if (offset >= REG_WDATA && offset < REG_WDATA + 0x20) {
        s5l8702_lcd_data(s, val);
        return;
    }
    switch (offset) {
    case REG_CON:
        s->lcd_con = val;
        break;
    case REG_WCMD:
        s5l8702_lcd_command(s, val);
        break;
    case REG_RCMD:
        /* select readback source; ID read returns a plausible panel id */
        s->rd_idx = 0;
        s->rd_buf[0] = 0x38;
        s->rd_buf[1] = 0xb3;
        s->rd_buf[2] = 0x71;
        s->rd_buf[3] = 0x00;
        s->rd_ready = false;
        break;
    case REG_RDATA:
        /* writing triggers one read cycle */
        s->rd_ready = true;
        break;
    case REG_INTCON:
        s->intcon = val;
        break;
    case REG_PHTIME:
        s->phtime = val;
        break;
    case REG_RST_TIME:
        s->rst_time = val;
        break;
    case REG_DRV_RST:
        s->drv_rst = val;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps s5l8702_lcd_ops = {
    .read = s5l8702_lcd_read,
    .write = s5l8702_lcd_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void s5l8702_lcd_invalidate(void *opaque)
{
    S5L8702LcdState *s = opaque;

    s->dirty = true;
}

static void s5l8702_lcd_gfx_update(void *opaque)
{
    S5L8702LcdState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint32_t *dest;
    int x, y;

    if (!s->dirty || !surface) {
        return;
    }
    if (surface_width(surface) != LCD_WIDTH ||
        surface_height(surface) != LCD_HEIGHT) {
        return;
    }
    dest = surface_data(surface);
    for (y = 0; y < LCD_HEIGHT; y++) {
        for (x = 0; x < LCD_WIDTH; x++) {
            uint16_t p = s->fb[y * LCD_WIDTH + x];
            uint32_t r = ((p >> 11) & 0x1f) << 3;
            uint32_t g = ((p >> 5) & 0x3f) << 2;
            uint32_t b = (p & 0x1f) << 3;

            /* replicate top bits for full-range colour */
            r |= r >> 5;
            g |= g >> 6;
            b |= b >> 5;
            *dest++ = 0xff000000 | (r << 16) | (g << 8) | b;
        }
    }
    s->dirty = false;
    dpy_gfx_update(s->con, 0, 0, LCD_WIDTH, LCD_HEIGHT);
}

static const GraphicHwOps s5l8702_lcd_gfx_ops = {
    .invalidate = s5l8702_lcd_invalidate,
    .gfx_update = s5l8702_lcd_gfx_update,
};

static void s5l8702_lcd_reset_hold(Object *obj, ResetType type)
{
    S5L8702LcdState *s = S5L8702_LCD(obj);

    s->lcd_con = 0;
    s->intcon = 0;
    s->phtime = 0;
    s->cur_cmd = 0;
    s->param_idx = 0;
    s->pixel_mode = false;
    s->win_xs = 0;
    s->win_xe = LCD_WIDTH - 1;
    s->win_ys = 0;
    s->win_ye = LCD_HEIGHT - 1;
    s->gram_x = 0;
    s->gram_y = 0;
    s->rd_idx = 0;
    s->rd_ready = false;
    memset(s->fb, 0, sizeof(s->fb));
    s->dirty = true;
}

static void s5l8702_lcd_realize(DeviceState *dev, Error **errp)
{
    S5L8702LcdState *s = S5L8702_LCD(dev);

    s->con = graphic_console_init(dev, 0, &s5l8702_lcd_gfx_ops, s);
    qemu_console_resize(s->con, LCD_WIDTH, LCD_HEIGHT);
}

static void s5l8702_lcd_init(Object *obj)
{
    S5L8702LcdState *s = S5L8702_LCD(obj);

    memory_region_init_io(&s->iomem, obj, &s5l8702_lcd_ops, s,
                          "s5l8702-lcd", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_s5l8702_lcd = {
    .name = "s5l8702-lcd",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(lcd_con, S5L8702LcdState),
        VMSTATE_UINT32(intcon, S5L8702LcdState),
        VMSTATE_UINT32(phtime, S5L8702LcdState),
        VMSTATE_UINT32(rst_time, S5L8702LcdState),
        VMSTATE_UINT32(drv_rst, S5L8702LcdState),
        VMSTATE_UINT32(cur_cmd, S5L8702LcdState),
        VMSTATE_INT32(param_idx, S5L8702LcdState),
        VMSTATE_UINT8_ARRAY(params, S5L8702LcdState, 4),
        VMSTATE_BOOL(pixel_mode, S5L8702LcdState),
        VMSTATE_UINT16(win_xs, S5L8702LcdState),
        VMSTATE_UINT16(win_xe, S5L8702LcdState),
        VMSTATE_UINT16(win_ys, S5L8702LcdState),
        VMSTATE_UINT16(win_ye, S5L8702LcdState),
        VMSTATE_UINT16(gram_x, S5L8702LcdState),
        VMSTATE_UINT16(gram_y, S5L8702LcdState),
        VMSTATE_UINT8_ARRAY(rd_buf, S5L8702LcdState, 4),
        VMSTATE_INT32(rd_idx, S5L8702LcdState),
        VMSTATE_BOOL(rd_ready, S5L8702LcdState),
        VMSTATE_UINT16_ARRAY(fb, S5L8702LcdState, LCD_WIDTH * LCD_HEIGHT),
        VMSTATE_END_OF_LIST()
    }
};

static void s5l8702_lcd_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = s5l8702_lcd_realize;
    rc->phases.hold = s5l8702_lcd_reset_hold;
    dc->vmsd = &vmstate_s5l8702_lcd;
}

static const TypeInfo s5l8702_lcd_info = {
    .name = TYPE_S5L8702_LCD,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8702LcdState),
    .instance_init = s5l8702_lcd_init,
    .class_init = s5l8702_lcd_class_init,
};

static void s5l8702_lcd_register_types(void)
{
    type_register_static(&s5l8702_lcd_info);
}

type_init(s5l8702_lcd_register_types)
