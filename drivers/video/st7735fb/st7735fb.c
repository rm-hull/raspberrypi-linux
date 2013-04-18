/*
 * linux/drivers/video/st7735fb.c -- FB driver for ST7735 LCD controller
 * as found in 1.8" TFT LCD modules available from Adafruit, SainSmart,
 * and other manufacturers.
 * Layout is based on skeletonfb.c by James Simmons and Geert Uytterhoeven.
 *
 * Copyright (C) 2011, Matt Porter
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of this archive for
 * more details.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/fb.h>
#include <linux/gpio.h>
#include <linux/spi/spi.h>
#include <linux/delay.h>
#include <linux/uaccess.h>

#include "st7735fb.h"


#if ( CONFIG_FB_ST7735_PANEL_TYPE_RED_TAB == 1 )
# define ST7735_COLSTART 0
# define ST7735_ROWSTART 0
#else
# define ST7735_COLSTART 2
# define ST7735_ROWSTART 1
#endif

/*
   The main image transfer SPI clock speed is set up by the st7735fb_map
   module when it opens our spi_device, but the st7735_cfg_script[] init
   sequence will be limited to this rate:
*/
#define ST7735_SPI_INITCMD_MAX_SPEED	2000000

static struct st7735_function st7735_cfg_script[] = {
	{ ST7735_START, ST7735_START},
	{ ST7735_CMD, ST7735_SWRESET},
	{ ST7735_DELAY, 150},
	{ ST7735_CMD, ST7735_SLPOUT},
	{ ST7735_DELAY, 500},
	{ ST7735_CMD, ST7735_FRMCTR1},
	{ ST7735_DATA, 0x01},
	{ ST7735_DATA, 0x2c},
	{ ST7735_DATA, 0x2d},
	{ ST7735_CMD, ST7735_FRMCTR2},
	{ ST7735_DATA, 0x01},
	{ ST7735_DATA, 0x2c},
	{ ST7735_DATA, 0x2d},
	{ ST7735_CMD, ST7735_FRMCTR3},
	{ ST7735_DATA, 0x01},
	{ ST7735_DATA, 0x2c},
	{ ST7735_DATA, 0x2d},
	{ ST7735_DATA, 0x01},
	{ ST7735_DATA, 0x2c},
	{ ST7735_DATA, 0x2d},
	{ ST7735_CMD, ST7735_INVCTR},
	{ ST7735_DATA, 0x07},
	{ ST7735_CMD, ST7735_PWCTR1},
	{ ST7735_DATA, 0xa2},
	{ ST7735_DATA, 0x02},
	{ ST7735_DATA, 0x84},
	{ ST7735_CMD, ST7735_PWCTR2},
	{ ST7735_DATA, 0xc5},
	{ ST7735_CMD, ST7735_PWCTR3},
	{ ST7735_DATA, 0x0a},
	{ ST7735_DATA, 0x00},
	{ ST7735_CMD, ST7735_PWCTR4},
	{ ST7735_DATA, 0x8a},
	{ ST7735_DATA, 0x2a},
	{ ST7735_CMD, ST7735_PWCTR5},
	{ ST7735_DATA, 0x8a},
	{ ST7735_DATA, 0xee},
	{ ST7735_CMD, ST7735_VMCTR1},
	{ ST7735_DATA, 0x0e},
	{ ST7735_CMD, ST7735_INVOFF},
	{ ST7735_CMD, ST7735_MADCTL},
#if ( CONFIG_FB_ST7735_RGB_ORDER_REVERSED == 1 )
	{ ST7735_DATA, 0xc0},
#else
	{ ST7735_DATA, 0xc8},
#endif
	{ ST7735_CMD, ST7735_COLMOD},
	{ ST7735_DATA, 0x05},
#if 0 /* set_addr_win will set these, so no need to set them at init */
	{ ST7735_CMD, ST7735_CASET},
	{ ST7735_DATA, 0x00},
	{ ST7735_DATA, 0x00 + ST7735_COLSTART},
	{ ST7735_DATA, 0x00},
	{ ST7735_DATA, WIDTH - 1 + ST7735_COLSTART},
	{ ST7735_CMD, ST7735_RASET},
	{ ST7735_DATA, 0x00},
	{ ST7735_DATA, 0x00 + ST7735_ROWSTART},
	{ ST7735_DATA, 0x00},
	{ ST7735_DATA, HEIGHT - 1 + ST7735_ROWSTART},
#endif
	{ ST7735_CMD, ST7735_GMCTRP1},
	{ ST7735_DATA, 0x02},
	{ ST7735_DATA, 0x1c},
	{ ST7735_DATA, 0x07},
	{ ST7735_DATA, 0x12},
	{ ST7735_DATA, 0x37},
	{ ST7735_DATA, 0x32},
	{ ST7735_DATA, 0x29},
	{ ST7735_DATA, 0x2d},
	{ ST7735_DATA, 0x29},
	{ ST7735_DATA, 0x25},
	{ ST7735_DATA, 0x2b},
	{ ST7735_DATA, 0x39},
	{ ST7735_DATA, 0x00},
	{ ST7735_DATA, 0x01},
	{ ST7735_DATA, 0x03},
	{ ST7735_DATA, 0x10},
	{ ST7735_CMD, ST7735_GMCTRN1},
	{ ST7735_DATA, 0x03},
	{ ST7735_DATA, 0x1d},
	{ ST7735_DATA, 0x07},
	{ ST7735_DATA, 0x06},
	{ ST7735_DATA, 0x2e},
	{ ST7735_DATA, 0x2c},
	{ ST7735_DATA, 0x29},
	{ ST7735_DATA, 0x2d},
	{ ST7735_DATA, 0x2e},
	{ ST7735_DATA, 0x2e},
	{ ST7735_DATA, 0x37},
	{ ST7735_DATA, 0x3f},
	{ ST7735_DATA, 0x00},
	{ ST7735_DATA, 0x00},
	{ ST7735_DATA, 0x02},
	{ ST7735_DATA, 0x10},
#if 0 /* init_display will turn on the display after clearing it */
	{ ST7735_CMD, ST7735_DISPON},
	{ ST7735_DELAY, 100},
#endif
	{ ST7735_CMD, ST7735_NORON},
	{ ST7735_DELAY, 10},
	{ ST7735_END, ST7735_END},
};

static struct fb_fix_screeninfo st7735fb_fix __devinitdata = {
	.id =		"ST7735", 
	.type =		FB_TYPE_PACKED_PIXELS,
	.visual =	FB_VISUAL_TRUECOLOR,
	.xpanstep =	0,
	.ypanstep =	0,
	.ywrapstep =	0, 
	.line_length =	WIDTH*BPP/8,
	.accel =	FB_ACCEL_NONE,
};

static struct fb_var_screeninfo st7735fb_var __devinitdata = {
	.xres =			WIDTH,
	.yres =			HEIGHT,
	.xres_virtual =		WIDTH,
	.yres_virtual =		HEIGHT,
	.bits_per_pixel =	BPP,
	.nonstd	=		1,
};

/**
 * spi_write_at_speed - SPI synchronous write with given SPI clock speed
 * @spi: device to which data will be written
 * @speed_hz: SPI clock speed for this message
 * @buf: data buffer
 * @len: data buffer size
 * Context: can sleep
 *
 * This writes the buffer and returns zero or a negative error code.
 * Callable only from contexts that can sleep.
 */
static inline int
spi_write_at_speed(struct spi_device *spi, u32 speed_hz,
			const void *buf, size_t len)
{
	struct spi_transfer	t = {
			.tx_buf		= buf,
			.len		= len,
			.speed_hz	= speed_hz,
		};
	struct spi_message	m;

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	return spi_sync(spi, &m);
}

static void st7735_write_data(struct st7735fb_par *par, int initcmd, u8 data)
{
	int ret = 0;
	int speed = initcmd ? ST7735_SPI_INITCMD_MAX_SPEED : 0;

	/* Set data mode */
	gpio_set_value(par->dc, 1);

	ret = spi_write_at_speed(par->spi, speed, &data, 1);
	if (ret < 0)
		pr_err("%s: write data %02x failed with status %d\n",
			par->info->fix.id, data, ret);
}

static int st7735_write_data_buf(struct st7735fb_par *par,
					u8 *txbuf, int size)
{
	/* Set data mode */
	gpio_set_value(par->dc, 1);

	/* Write entire buffer (*/
	return spi_write(par->spi, txbuf, size);
}

static void st7735_write_cmd(struct st7735fb_par *par, int initcmd, u8 data)
{
	int ret = 0;
	int speed = initcmd ? ST7735_SPI_INITCMD_MAX_SPEED : 0;

	/* Set command mode */
	gpio_set_value(par->dc, 0);

	ret = spi_write_at_speed(par->spi, speed, &data, 1);
	if (ret < 0)
		pr_err("%s: write command %02x failed with status %d\n",
			par->info->fix.id, data, ret);
}

static void st7735_run_cfg_script(struct st7735fb_par *par)
{
	int i = 0;
	int end_script = 0;

	do {
		switch (st7735_cfg_script[i].cmd)
		{
		case ST7735_START:
			break;
		case ST7735_CMD:
			st7735_write_cmd(par, 1,
				st7735_cfg_script[i].data & 0xff);
			break;
		case ST7735_DATA:
			st7735_write_data(par, 1,
				st7735_cfg_script[i].data & 0xff);
			break;
		case ST7735_DELAY:
			mdelay(st7735_cfg_script[i].data);
			break;
		case ST7735_END:
			end_script = 1;
		}
		i++;
	} while (!end_script);
}

static void st7735_set_addr_win(struct st7735fb_par *par,
				int xs, int ys, int xe, int ye)
{
	unsigned char buf[4] = { 0, 0, 0, 0 };

	if (par->addr_win.xs != xs || par->addr_win.xe != xe) {
	    par->addr_win.xs = xs;
	    par->addr_win.xe = xe;
	    pr_debug("ST7735FB - set_addr_win xs=%d xe=%d\n", xs, xe);
	    st7735_write_cmd(par, 0, ST7735_CASET);
	    buf[1] = xs + ST7735_COLSTART;
	    buf[3] = xe + ST7735_COLSTART;
	    st7735_write_data_buf(par, buf, 4);
	}
	if (par->addr_win.ys != ys || par->addr_win.ye != ye) {
	    par->addr_win.ys = ys;
	    par->addr_win.ye = ye;
	    pr_debug("ST7735FB - set_addr_win ys=%d ye=%d\n", ys, ye);
	    st7735_write_cmd(par, 0, ST7735_RASET);
	    buf[1] = ys + ST7735_ROWSTART;
	    buf[3] = ye + ST7735_ROWSTART;
	    st7735_write_data_buf(par, buf, 4);
	}
}

static void st7735_reset(struct st7735fb_par *par)
{
	/* Reset controller */
	gpio_set_value(par->rst, 0);
	udelay(10);
	gpio_set_value(par->rst, 1);
	mdelay(120);
}

static void st7735fb_update_display(struct st7735fb_par *par,
		const struct fb_fillrect *dirty_rect)
{
	int ret = 0;
	u8 *vmem = par->info->screen_base;
	int i;
	u16 *vmem16;
	u16 *smem16;
	int dx, dy, dw, dh;
	unsigned int vmem_start;
	unsigned int write_nbytes;

	if (dirty_rect) {
	    dx = dirty_rect->dx;
	    dy = dirty_rect->dy;
	    dw = dirty_rect->width;
	    dh = dirty_rect->height;
	} else {
	    dx = dy = 0;
	    dw = WIDTH;
	    dh = HEIGHT;
	}

	/* Load spi_writebuf with image data from the dirty_rect subwindow */
	vmem_start = (WIDTH*dy + dx)*BPP/8;
	vmem16 = (u16 *)((u8 *)vmem + vmem_start);
	smem16 = (u16 *)par->spi_writebuf;
	for (i=0; i<dh; i++) {
		int x;
#ifdef __LITTLE_ENDIAN
		for (x=0; x<dw; x++)
		    smem16[x] = swab16(vmem16[x]);
#else
		memcpy(smem16, vmem16, dw*2);
#endif
		smem16 += dw*BPP/16;
		vmem16 += WIDTH*BPP/16;
	}

	/* Set the RAM write target window */
	st7735_set_addr_win(par, dx, dy, dx+dw-1, dy+dh-1);

	/* Internal RAM write command */
	st7735_write_cmd(par, 0, ST7735_RAMWR);

	/* Blast spi_writebuf to ST7735 internal display RAM target window */
	write_nbytes = dw*dh*BPP/8;
	ret = st7735_write_data_buf(par, par->spi_writebuf, write_nbytes);
	if (ret < 0)
		pr_err("%s: spi_write failed to update display buffer\n",
			par->info->fix.id);
	pr_debug("%s: spi_write_data_buf(%d) returned %d\n",
			par->info->fix.id, write_nbytes, ret);
}

static void st7735fb_deferred_io(struct fb_info *info,
				struct list_head *pagelist)
{
	struct page *page;
	int npages = info->fix.smem_len / PAGE_SIZE;
	int page_low = npages;
	int page_high = -1;
	struct fb_fillrect rect;
	int i;

	for ( i=0; i<npages; i++ ) {
		struct st7735fb_par *par = info->par;
		if (test_and_clear_bit(i, &par->deferred_pages_mask)) {
			if ( page_high == -1 )
				page_low = i;
			page_high = i;
		}
	}

	list_for_each_entry(page, pagelist, lru) {
		if ( page_low > page->index )
		    page_low = page->index;
		if ( page_high < page->index )
		    page_high = page->index;
	}

	if (page_high == -1) {
		pr_debug("ST7735FB - deferred_io no pages? full update.\n");
		page_low = 0;
		page_high = npages - 1;
	}

	rect.dx = 0;
	rect.width = WIDTH;

	rect.dy = page_low * PAGE_SIZE / (WIDTH*BPP/8);
	rect.height = (page_high - page_low + 1) * PAGE_SIZE / (WIDTH*BPP/8);

	pr_debug("ST7735FB - deferred_io page_low=%d page_high=%d dy=%u h=%u\n",
			page_low, page_high, rect.dy, rect.height);

	st7735fb_update_display(info->par, &rect);
}


/* Mark page-size areas as dirty by setting corresponding bit(s) in the
 * deferred_pages_mask bitmap, then (re-)schedule the deferred_io workqueue
 * task. */
static void st7735fb_deferred_io_touch(struct fb_info *info,
				const struct fb_fillrect *rect)
{
	struct fb_deferred_io *fbdefio = info->fbdefio;

	if (!fbdefio)
		return;

	if (rect) {
		struct st7735fb_par *par = info->par;
		unsigned long offset;
		int index_lo, index_hi, i;

		offset = rect->dy * info->fix.line_length;
		index_lo = offset >> PAGE_SHIFT;
		offset = (rect->dy + rect->height - 1) * info->fix.line_length;
		index_hi = offset >> PAGE_SHIFT;

		for ( i=index_lo; i<=index_hi; i++ )
			set_bit(i, &par->deferred_pages_mask);
	}

	schedule_delayed_work(&info->deferred_work, fbdefio->delay);
}


static int st7735fb_blank(int blank_mode, struct fb_info *info)
{
	struct st7735fb_par *par = info->par;

	/*
	 * Without a way to control the backlight, just setting ST7735_DISPOFF
	 * for blanking leaves the screen in full-brightness white.  Instead,
	 * just paint the screen black.
	 */
#if 0
	int cmd = ST7735_DISPOFF;

	if (blank_mode == FB_BLANK_UNBLANK)
		cmd = ST7735_DISPON;

	st7735_write_cmd(par, 1, cmd);
	mdelay(100);
#else
	if (blank_mode == FB_BLANK_UNBLANK) {
		/* do nothing */
	} else {
		/* paint the screen black */
		memset(par->info->screen_base, 0, info->fix.smem_len);
		st7735fb_update_display(par, NULL);
	}
#endif

	return 0;
}


static int st7735fb_init_display(struct st7735fb_par *par)
{
	st7735_reset(par);

	st7735_run_cfg_script(par);

	if ( 1 ) { /* fill display mem with a gradient pattern */
		u16 *vmem16 = (u16 *)par->info->screen_base;
		int x, y;
		for ( y=0; y<HEIGHT; y++ )
			for ( x=0; x<WIDTH; x++ ) {
				u16 pixel;
				pixel = ((WIDTH-x)>>2) << 11
				      | (x>>2) << 5
				      | (y>>3) << 0;
				*vmem16++ = pixel;
			}
	}

	/* update the display with the display mem contents, then turn it on */
	st7735fb_update_display(par, NULL);

	st7735_write_cmd(par, 1, ST7735_DISPON);
	mdelay(100);

	return 0;
}

void st7735fb_fillrect(struct fb_info *info, const struct fb_fillrect *rect)
{
	pr_debug("ST7735FB - fillrect dx=%d dy=%d w=%d h=%d color=0x%x\n",
		rect->dx, rect->dy, rect->width, rect->height, rect->color);

	sys_fillrect(info, rect);

	st7735fb_deferred_io_touch(info, rect);
}

void st7735fb_copyarea(struct fb_info *info, const struct fb_copyarea *area) 
{
	const struct fb_fillrect *rect = (const struct fb_fillrect *)area;

	pr_debug("ST7735FB - copyarea dx=%d dy=%d w=%d h=%d\n",
		rect->dx, rect->dy, rect->width, rect->height);

	sys_copyarea(info, area);

	st7735fb_deferred_io_touch(info, rect);
}

void st7735fb_imageblit(struct fb_info *info, const struct fb_image *image) 
{
	const struct fb_fillrect *rect = (const struct fb_fillrect *)image;

	pr_debug("ST7735FB - imageblit dx=%d dy=%d w=%d h=%d\n",
		rect->dx, rect->dy, rect->width, rect->height);

	sys_imageblit(info, image);

	st7735fb_deferred_io_touch(info, rect);
}

static ssize_t st7735fb_write(struct fb_info *info, const char __user *buf,
		size_t count, loff_t *ppos)
{
	unsigned long p = *ppos;
	void *dst;
	int err = 0;
	unsigned long total_size;

	pr_debug("ST7735FB - write(count=%zd,ppos=%llu)\n", count, *ppos);

	if (info->state != FBINFO_STATE_RUNNING)
		return -EPERM;

	total_size = info->fix.smem_len;

	if (p > total_size)
		return -EFBIG;

	if (count > total_size) {
		err = -EFBIG;
		count = total_size;
	}

	if (count + p > total_size) {
		if (!err)
			err = -ENOSPC;

		count = total_size - p;
	}

	dst = (void __force *) (info->screen_base + p);

	if (copy_from_user(dst, buf, count))
		err = -EFAULT;

	if  (!err)
		*ppos += count;

	st7735fb_deferred_io_touch(info, NULL); /* TODO: pass dirty rect */

	return (err) ? err : count;
}

static int st7735fb_setcolreg(unsigned regno, unsigned red, unsigned green,
				unsigned blue, unsigned transp,
				struct fb_info *info)
{
	struct st7735fb_par *par = info->par;

	if (regno >= ARRAY_SIZE(par->pseudo_palette))
		return -EINVAL;

	par->pseudo_palette[regno] =
		((red   & 0xf800)) |
		((green & 0xfc00) >>  5) |
		((blue  & 0xf800) >> 11);

	return 0;
}


static struct fb_ops st7735fb_ops = {
	.owner		= THIS_MODULE,
	.fb_read	= fb_sys_read,
	.fb_write	= st7735fb_write,
	.fb_blank	= st7735fb_blank,
	.fb_fillrect	= st7735fb_fillrect,
	.fb_copyarea	= st7735fb_copyarea,
	.fb_imageblit	= st7735fb_imageblit,
	.fb_setcolreg	= st7735fb_setcolreg,
};

static struct fb_deferred_io st7735fb_defio = {
	.delay		= HZ/30,
	.deferred_io	= st7735fb_deferred_io,
};


static int __devinit st7735fb_probe (struct spi_device *spi)
{
	int chip = spi_get_device_id(spi)->driver_data;
	struct st7735fb_platform_data *pdata = spi->dev.platform_data;
	int vmem_size = WIDTH*HEIGHT*BPP/8;
	u8 *vmem = NULL;
	u8 *spi_writebuf = NULL;
	struct fb_info *info;
	struct st7735fb_par *par;
	int retval = -EINVAL;

	pr_debug("ST7735FB - loading\n");

	if (chip != ST7735_DISPLAY_COMMON_TFT18) {
		pr_err("%s: only the %s device is supported\n", DRVNAME,
			to_spi_driver(spi->dev.driver)->id_table->name);
		return -EINVAL;
	}

	if (!pdata) {
		pr_err("%s: platform data required for rst and dc info\n",
			DRVNAME);
		return -EINVAL;
	}

	/* Request GPIOs and initialize to default values */
	retval = gpio_request_one(pdata->rst_gpio, GPIOF_OUT_INIT_HIGH,
			"ST7735 Reset Pin");
	if (retval) {
		pr_err("%s: could not acquire rst_gpio %d\n",
			DRVNAME, pdata->rst_gpio);
		return retval;
	}

	retval = gpio_request_one(pdata->dc_gpio, GPIOF_OUT_INIT_LOW,
			"ST7735 Data/Command Pin");
	if (retval) {
		gpio_free(pdata->dc_gpio);
		pr_err("%s: could not acquire dc_gpio %d\n",
			DRVNAME, pdata->dc_gpio);
		return retval;
	}

	retval = -ENOMEM;
	vmem = vzalloc(vmem_size);
	if (!vmem)
		goto alloc_fail;

	/* Allocate spi write buffer */
	spi_writebuf = vzalloc(vmem_size);
	if (!spi_writebuf)
		goto alloc_fail;

	info = framebuffer_alloc(sizeof(struct st7735fb_par), &spi->dev);
	if (!info)
		goto alloc_fail;

	info->screen_base = (u8 __force __iomem *)vmem;
	info->fbops = &st7735fb_ops;
	info->fix = st7735fb_fix;
	info->fix.smem_len = vmem_size;
	info->var = st7735fb_var;
	/* Choose any packed pixel format as long as it's RGB565 */
	info->var.red.offset = 11;
	info->var.red.length = 5;
	info->var.green.offset = 5;
	info->var.green.length = 6;
	info->var.blue.offset = 0;
	info->var.blue.length = 5;
	info->var.transp.offset = 0;
	info->var.transp.length = 0;
	info->flags = FBINFO_FLAG_DEFAULT | FBINFO_VIRTFB;
	info->fbdefio = &st7735fb_defio;
	fb_deferred_io_init(info);

	par = info->par;
	par->info = info;
	par->spi = spi;
	par->rst = pdata->rst_gpio;
	par->dc = pdata->dc_gpio;
	par->spi_writebuf = spi_writebuf;
	par->addr_win.xs = par->addr_win.xe =
		par->addr_win.ys = par->addr_win.ye = -1;

	info->pseudo_palette = par->pseudo_palette;

	spi_set_drvdata(spi, info);

	retval = st7735fb_init_display(par);
	if (retval < 0)
		goto init_fail;

	/* register framebuffer *after* initializing device! */
	retval = register_framebuffer(info);
	if (retval < 0)
		goto fbreg_fail;

	pr_info("fb%d: %s frame buffer device,\n"
		"\tusing %d KiB of video memory\n"
		"\trst_gpio=%d dc_gpio=%d\n",
		info->node, info->fix.id, vmem_size, par->rst, par->dc);

	return 0;


	/* TODO: release gpios on fail */
fbreg_fail:
	framebuffer_release(info);

init_fail:
	spi_set_drvdata(spi, NULL);

alloc_fail:
	if (spi_writebuf)
		vfree(spi_writebuf);
	if (vmem)
		vfree(vmem);

	gpio_free(pdata->dc_gpio);
	gpio_free(pdata->rst_gpio);

	return retval;
}

static int __devexit st7735fb_remove(struct spi_device *spi)
{
	struct fb_info *info = spi_get_drvdata(spi);

	if (info) {
		struct st7735fb_par *par = info->par;
		unregister_framebuffer(info);
		fb_deferred_io_cleanup(info);
		vfree(info->screen_base);	
		framebuffer_release(info);
		vfree(par->spi_writebuf);
		gpio_free(par->dc);
		gpio_free(par->rst);
	}

	spi_set_drvdata(spi, NULL);

	return 0;
}

static const struct spi_device_id st7735fb_ids[] = {
	{ "common_tft18", ST7735_DISPLAY_COMMON_TFT18 },
	{ },
};

MODULE_DEVICE_TABLE(spi, st7735fb_ids);

static struct spi_driver st7735fb_driver = {
	.driver = {
		.name   = "st7735fb",
		.owner  = THIS_MODULE,
	},
	.id_table = st7735fb_ids,
	.probe  = st7735fb_probe,
	.remove = __devexit_p(st7735fb_remove),
};


static struct work_struct deferred_register_work;

static void deferred_register( struct work_struct *work )
{
	pr_debug("ST7735FB - deferred_register\n");
	spi_register_driver(&st7735fb_driver);
}

static int __init st7735fb_init(void)
{
	pr_debug("ST7735FB - init\n");
	INIT_WORK(&deferred_register_work, deferred_register);
	return schedule_work(&deferred_register_work);
}

static void __exit st7735fb_exit(void)
{
	pr_debug("ST7735FB - exit\n");
	spi_unregister_driver(&st7735fb_driver);
}

/* ------------------------------------------------------------------------- */

module_init(st7735fb_init);
module_exit(st7735fb_exit);

MODULE_DESCRIPTION("FB driver for ST7735 display controller");
MODULE_AUTHOR("Matt Porter");
MODULE_AUTHOR("Neil Greatorex");
MODULE_AUTHOR("Kamal Mostafa <kamal@whence.com>");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.4");
