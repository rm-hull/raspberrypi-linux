#include <linux/module.h>
#include <linux/spi/spi.h>
#include "st7735fb.h"


const char this_driver_name[] = "common_tft18";

static struct st7735fb_platform_data st7735fb_data;

/*
   The RST and DC gpio pin numbers must either be configured as CONFIG_'s
   or (if the CONFIG_'s are left as -1) then they provided as module
   params "rst_gpio" and "dc_gpio".
*/

static int rst_gpio = CONFIG_FB_ST7735_MAP_RST_GPIO;
module_param(rst_gpio, int, 0444);
MODULE_PARM_DESC(rst_gpio, "ST7735 RST gpio pin number");

static int dc_gpio = CONFIG_FB_ST7735_MAP_DC_GPIO;
module_param(dc_gpio, int, 0444);
MODULE_PARM_DESC(dc_gpio, "ST7735 D/C gpio pin number");

static unsigned int spi_bus_num = CONFIG_FB_ST7735_MAP_SPI_BUS_NUM;
module_param(spi_bus_num, uint, 0444);
MODULE_PARM_DESC(spi_bus_num, "SPI bus number");

static unsigned int spi_bus_cs = CONFIG_FB_ST7735_MAP_SPI_BUS_CS;
module_param(spi_bus_cs, uint, 0444);
MODULE_PARM_DESC(spi_bus_cs, "SPI bus chipselect");

static unsigned int spi_speed_hz = CONFIG_FB_ST7735_MAP_SPI_BUS_SPEED;
module_param(spi_speed_hz, uint, 0444);
MODULE_PARM_DESC(spi_speed_hz, "SPI bus clock speed (Hz)");

static unsigned int spi_mode = CONFIG_FB_ST7735_MAP_SPI_BUS_MODE;
module_param(spi_mode, uint, 0444);
MODULE_PARM_DESC(spi_mode, "SPI bus mode (0, 1, 2, 3)");


static struct spi_device *st7735fb_mapped_spi_device;


static int __init add_st7735fb_device_to_bus(void)
{
	struct spi_master *spi_master;
	struct spi_device *spi_device;
	struct device *pdev;
	char buff[64];
	int status = 0;

	if (rst_gpio < 0 || dc_gpio < 0) {
		printk(KERN_ALERT "params rst_gpio and dc_gpio must be set!\n");
		return -1;
	}
	st7735fb_data.rst_gpio = rst_gpio;
	st7735fb_data.dc_gpio = dc_gpio;

	spi_master = spi_busnum_to_master(spi_bus_num);
	if (!spi_master) {
		printk(KERN_ALERT "spi_busnum_to_master(%d) returned NULL\n",
			spi_bus_num);
		printk(KERN_ALERT "SPI bus initialised?\n");
		return -1;
	}

	spi_device = spi_alloc_device(spi_master);
	if (!spi_device) {
		put_device(&spi_master->dev);
		printk(KERN_ALERT "spi_alloc_device() failed\n");
		return -1;
	}

	/* specify a chip select line */
	spi_device->chip_select = spi_bus_cs;

	/* Check whether this SPI bus.cs is already claimed */
	snprintf(buff, sizeof(buff), "%s.%u", 
			dev_name(&spi_device->master->dev),
			spi_device->chip_select);

	pdev = bus_find_device_by_name(spi_device->dev.bus, NULL, buff);
 	if (pdev) {
		/* We are not going to use this spi_device, so free it */ 
		spi_dev_put(spi_device);
		
		/* 
		 * There is already a device configured for this bus.cs combination.
		 * It's okay if it's us. This happens if we previously loaded then 
                 * unloaded our driver. 
                 * If it is not us, we complain and fail.
		 */
		if (pdev->driver && pdev->driver->name && 
				strcmp(this_driver_name, pdev->driver->name)) {
			printk(KERN_ALERT 
				"Driver [%s] already registered for %s\n",
				pdev->driver->name, buff);
			status = -1;
		} 
	} else {
		spi_device->dev.platform_data = &st7735fb_data;
		spi_device->max_speed_hz = spi_speed_hz;
		switch (spi_mode) {
			case 0: spi_device->mode = SPI_MODE_0;
				break;
			case 1: spi_device->mode = SPI_MODE_1;
				break;
			case 2: spi_device->mode = SPI_MODE_2;
				break;
			case 3: spi_device->mode = SPI_MODE_3;
				break;
		}
		spi_device->bits_per_word = 8;
		spi_device->irq = -1;
		spi_device->controller_state = NULL;
		spi_device->controller_data = NULL;
		strlcpy(spi_device->modalias, this_driver_name, SPI_NAME_SIZE);
		status = spi_add_device(spi_device);
		
		if (status < 0) {	
			spi_dev_put(spi_device);
			printk(KERN_ALERT "spi_add_device() failed: %d\n", 
				status);		
		}				

		st7735fb_mapped_spi_device = spi_device;
	}

	put_device(&spi_master->dev);

	return status;
}
module_init(add_st7735fb_device_to_bus);

static void __exit st7735fb_unmap(void)
{
	BUG_ON(!st7735fb_mapped_spi_device);
	device_del(&st7735fb_mapped_spi_device->dev);
	spi_dev_put(st7735fb_mapped_spi_device);
	st7735fb_mapped_spi_device = NULL;
}
module_exit(st7735fb_unmap);

MODULE_AUTHOR("Neil Greatorex");
MODULE_AUTHOR("Kamal Mostafa <kamal@whence.com>");
MODULE_DESCRIPTION("Bind SPI to st7735fb");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.4");


