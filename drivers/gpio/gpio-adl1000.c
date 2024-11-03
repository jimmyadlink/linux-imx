// SPDX-License-Identifier: GPL-2.0-only
/*
 *  ADLink 40 bit I/O ports
 *
 *
 *  Derived from drivers/i2c/chips/pca953x.c
 */

#include <linux/bitmap.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/seq_file.h>
#include <linux/reset.h>
#include <linux/slab.h>

#include <asm/unaligned.h>

#include <linux/delay.h>
#define REGMAP_BUCK 0
#define REGMAP_BUCK_RETRY_CNT 3

#define ADL1000_INPUT		0x06
#define ADL1000_OUTPUT		0x0A
//#define ADL1000_INVERT		0x08
#define ADL1000_DIRECTION	0x00

//#define ADL1000_IN_LATCH	0x48
#define ADL1000_PULL_EN		0x20
#define ADL1000_PULL_SEL	0x26
#define ADL1000_INT_MASK	0x10
#define ADL1000_INT_STAT	0x16
#define ADL1000_INT_VER0	0x2e
#define ADL1000_INT_VER1	0x2f

#define ADL1000_NPIN		24


static const struct i2c_device_id adl1000_id[] = {
	{ "adl1000", ADL1000_NPIN , },
	{ }
};
MODULE_DEVICE_TABLE(i2c, adl1000_id);

#define MAX_BANK 6
#define BANK_SZ 8
#define MAX_LINE	(MAX_BANK * BANK_SZ)

#define NBANK(chip) DIV_ROUND_UP(chip->gpio_chip.ngpio, BANK_SZ)


struct adl1000_platform_data {
	/* number of the first GPIO */
	unsigned	gpio_base;

	/* initial polarity inversion setting */
	u32		invert;

	/* interrupt base */
	int		irq_base;

	void		*context;	/* param to setup/teardown */

	int		(*setup)(struct i2c_client *client,
				unsigned gpio, unsigned ngpio,
				void *context);
	void		(*teardown)(struct i2c_client *client,
				unsigned gpio, unsigned ngpio,
				void *context);
	const char	*const *names;
};

struct adl1000_reg_config {
	int direction;
	int output;
	int input;
	//int invert;
};

static const struct adl1000_reg_config adl1000_regs = {
	.direction = ADL1000_DIRECTION,
	.output = ADL1000_OUTPUT,
	.input = ADL1000_INPUT ,
	//.invert = ADL1000_INVERT,
};



struct adl1000_chip {
	unsigned gpio_start;
	struct mutex i2c_lock;
	struct regmap *regmap;


	struct mutex irq_lock;
	DECLARE_BITMAP(irq_mask, MAX_LINE);
	DECLARE_BITMAP(irq_stat, MAX_LINE);
	DECLARE_BITMAP(irq_trig_raise, MAX_LINE);
	DECLARE_BITMAP(irq_trig_fall, MAX_LINE);

	atomic_t wakeup_path;

	struct i2c_client *client;
	struct gpio_chip gpio_chip;
	const char *const *names;
	unsigned long driver_data;
	struct regulator *regulator;

	const struct adl1000_reg_config *regs;

	u8 (*recalc_addr)(struct adl1000_chip *chip, int reg, int off);
};

static bool adl1000_readable_register(struct device *dev, unsigned int reg)
{
	struct adl1000_chip *chip = dev_get_drvdata(dev);
	if(
		(reg>=ADL1000_INPUT && reg<(ADL1000_INPUT+ NBANK(chip))) ||
		(reg>=ADL1000_OUTPUT && reg<(ADL1000_OUTPUT+ NBANK(chip))) ||
//		(reg>=ADL1000_INVERT && reg<(ADL1000_INVERT+ NBANK(chip))) ||
		(reg>=ADL1000_DIRECTION && reg<(ADL1000_DIRECTION+ NBANK(chip))) ||
//		(reg>=ADL1000_IN_LATCH && reg<(ADL1000_IN_LATCH+ NBANK(chip))) ||
		(reg>=ADL1000_PULL_EN && reg<(ADL1000_PULL_EN+ NBANK(chip))) ||
		(reg>=ADL1000_PULL_SEL && reg<(ADL1000_PULL_SEL+ NBANK(chip))) ||
		(reg>=ADL1000_INT_MASK && reg<(ADL1000_INT_MASK+ NBANK(chip))) ||
		(reg>=ADL1000_INT_STAT && reg<(ADL1000_INT_STAT+ NBANK(chip)))
		)
	{
		return true;
	}
	else 
	{
		return false;
	}
}

static bool adl1000_writeable_register(struct device *dev, unsigned int reg)
{
	struct adl1000_chip *chip = dev_get_drvdata(dev);
	if(
		(reg>=ADL1000_OUTPUT && reg<(ADL1000_OUTPUT+ NBANK(chip))) ||
//		(reg>=ADL1000_INVERT && reg<(ADL1000_INVERT+ NBANK(chip))) ||
		(reg>=ADL1000_DIRECTION && reg<(ADL1000_DIRECTION+ NBANK(chip))) ||
//		(reg>=ADL1000_IN_LATCH && reg<(ADL1000_IN_LATCH+ NBANK(chip))) ||
		(reg>=ADL1000_PULL_EN && reg<(ADL1000_PULL_EN+ NBANK(chip))) ||
		(reg>=ADL1000_PULL_SEL && reg<(ADL1000_PULL_SEL+ NBANK(chip))) ||
		(reg>=ADL1000_INT_MASK && reg<(ADL1000_INT_MASK+ NBANK(chip)))
		)
	{
		return true;
	}
	else 
	{
		return false;
	}
}

static bool adl1000_volatile_register(struct device *dev, unsigned int reg)
{
	struct adl1000_chip *chip = dev_get_drvdata(dev);
	if(
		(reg>=ADL1000_INPUT && reg<(ADL1000_INPUT+ NBANK(chip))) ||
		(reg>=ADL1000_INT_STAT && reg<(ADL1000_INT_STAT+ NBANK(chip)))
	)
	{
		return true;
	}
	else 
	{
		return false;
	}
}

static const struct regmap_config adl1000_i2c_regmap = {
	.reg_bits = 8,
	.val_bits = 8,

	.use_single_read = true,
	.use_single_write = true,

	.readable_reg = adl1000_readable_register,
	.writeable_reg = adl1000_writeable_register,
	.volatile_reg = adl1000_volatile_register,

	.disable_locking = true,
	.cache_type = REGCACHE_RBTREE,
	.max_register = 0x7f,
};


static u8 adl1000_recalc_addr(struct adl1000_chip *chip, int reg, int off)
{

	u8 regaddr = reg + (off / BANK_SZ);

	return regaddr;
}

static int adl1000_write_regs(struct adl1000_chip *chip, int reg, unsigned long *val)
{
	u8 regaddr = chip->recalc_addr(chip, reg, 0);
	u8 value[MAX_BANK];
	int i, ret;

	for (i = 0; i < NBANK(chip); i++)
		value[i] = bitmap_get_value8(val, i * BANK_SZ);

#if REGMAP_BUCK
	ret = regmap_bulk_write(chip->regmap, regaddr, value, NBANK(chip));
	if (ret < 0) {
		dev_err(&chip->client->dev, "failed writing register\n");
		return ret;
	}
#else
	for (i = 0; i < NBANK(chip); i++)
	{
		int retry = 0;

		ret=1;
		while(ret)
		{
			ret = regmap_write(chip->regmap, regaddr+i, (unsigned int)value[i]);
			if(ret)
			{
				retry++;
				dev_err(&chip->client->dev, "jimmy adl1000_write_regs regmap_write failed reg:0x%x ret:0x%x retry:%d\n",regaddr+i,ret,retry);
				if(retry >= REGMAP_BUCK_RETRY_CNT)
					break;
				udelay(300);
			}
		}
		if (ret < 0) {
			dev_err(&chip->client->dev, "failed writing register\n");
			return ret;
		}
	}
	
#endif
	return 0;
}

static int adl1000_read_regs(struct adl1000_chip *chip, int reg, unsigned long *val)
{
	u8 regaddr = chip->recalc_addr(chip, reg, 0);
	u8 value[MAX_BANK];
	int i, ret;

#if REGMAP_BUCK
	ret = regmap_bulk_read(chip->regmap, regaddr, value, NBANK(chip));
	printk("jimmy adl1000_read_regs  regaddr:%d ret:0x%x i:%d\n",regaddr,ret,i++);
	if (ret < 0) {
		dev_err(&chip->client->dev, "failed reading register\n");
		return ret;
	}
#else
	for (i = 0; i < NBANK(chip); i++)
	{
		int retry = 0;

		ret=1;
		while(ret)
		{
			ret = regmap_read(chip->regmap, regaddr+i, ( unsigned int *)&value[i]);
			if(ret)
			{
				retry++;
				dev_err(&chip->client->dev, "jimmy adl1000_read_regs regmap_read failed reg:0x%x ret:0x%x retry:%d\n",regaddr+i,ret,retry);
				if(retry >= REGMAP_BUCK_RETRY_CNT)
					break;
				udelay(300);
			}
		}
		if (ret < 0) {
			dev_err(&chip->client->dev, "failed reading register\n");
			return ret;
		}
	}
#endif
	for (i = 0; i < NBANK(chip); i++)
	{
		bitmap_set_value8(val, value[i], i * BANK_SZ);
	}

	return 0;
}

static int adl1000_gpio_direction_input(struct gpio_chip *gc, unsigned off)
{
	struct adl1000_chip *chip = gpiochip_get_data(gc);
	u8 dirreg = chip->recalc_addr(chip, chip->regs->direction, off);
	u8 bit = BIT(off % BANK_SZ);
	int ret;

	mutex_lock(&chip->i2c_lock);

	ret = regmap_write_bits(chip->regmap, dirreg, bit, bit);
	mutex_unlock(&chip->i2c_lock);
	return ret;
}

static int adl1000_gpio_direction_output(struct gpio_chip *gc,
		unsigned off, int val)
{
	struct adl1000_chip *chip = gpiochip_get_data(gc);
	u8 dirreg = chip->recalc_addr(chip, chip->regs->direction, off);
	u8 outreg = chip->recalc_addr(chip, chip->regs->output, off);
	u8 bit = BIT(off % BANK_SZ);
	int ret;

	mutex_lock(&chip->i2c_lock);
	/* set output level */
	ret = regmap_write_bits(chip->regmap, outreg, bit, val ? bit : 0);
	if (ret)
		goto exit;

	/* then direction */
	ret = regmap_write_bits(chip->regmap, dirreg, bit, 0);
exit:
	mutex_unlock(&chip->i2c_lock);
	return ret;
}

static int adl1000_gpio_get_value(struct gpio_chip *gc, unsigned off)
{
	struct adl1000_chip *chip = gpiochip_get_data(gc);
	u8 inreg = chip->recalc_addr(chip, chip->regs->input, off);
	u8 bit = BIT(off % BANK_SZ);
	u32 reg_val;
	int ret;

	mutex_lock(&chip->i2c_lock);
	ret = regmap_read(chip->regmap, inreg, &reg_val);

	mutex_unlock(&chip->i2c_lock);
	if (ret < 0)
		return ret;

	return !!(reg_val & bit);
}

static void adl1000_gpio_set_value(struct gpio_chip *gc, unsigned off, int val)
{
	struct adl1000_chip *chip = gpiochip_get_data(gc);
	u8 outreg = chip->recalc_addr(chip, chip->regs->output, off);
	u8 bit = BIT(off % BANK_SZ);

	mutex_lock(&chip->i2c_lock);

	regmap_write_bits(chip->regmap, outreg, bit, val ? bit : 0);
	mutex_unlock(&chip->i2c_lock);
}

static int adl1000_gpio_get_direction(struct gpio_chip *gc, unsigned off)
{
	struct adl1000_chip *chip = gpiochip_get_data(gc);
	u8 dirreg = chip->recalc_addr(chip, chip->regs->direction, off);
	u8 bit = BIT(off % BANK_SZ);
	u32 reg_val;
	int ret;

	mutex_lock(&chip->i2c_lock);
	ret = regmap_read(chip->regmap, dirreg, &reg_val);

	mutex_unlock(&chip->i2c_lock);
	if (ret < 0)
		return ret;

	if (reg_val & bit)
		return GPIO_LINE_DIRECTION_IN;

	return GPIO_LINE_DIRECTION_OUT;
}

static int adl1000_gpio_set_pull_up_down(struct adl1000_chip *chip,
					 unsigned int offset,
					 unsigned long config)
{
	enum pin_config_param param = pinconf_to_config_param(config);

	u8 pull_en_reg = chip->recalc_addr(chip, ADL1000_PULL_EN, offset);
	u8 pull_sel_reg = chip->recalc_addr(chip, ADL1000_PULL_SEL, offset);
	u8 bit = BIT(offset % BANK_SZ);
	int ret;

	/*
	 * pull-up/pull-down configuration requires PCAL extended
	 * registers
	 */

	mutex_lock(&chip->i2c_lock);

	/* Configure pull-up/pull-down */
	if (param == PIN_CONFIG_BIAS_PULL_UP)
	{
		ret = regmap_write_bits(chip->regmap, pull_sel_reg, bit, bit);
	}
	else if (param == PIN_CONFIG_BIAS_PULL_DOWN)
	{
		ret = regmap_write_bits(chip->regmap, pull_sel_reg, bit, 0);
	}
	else
		ret = 0;
	if (ret)
		goto exit;

	/* Disable/Enable pull-up/pull-down */
	if (param == PIN_CONFIG_BIAS_DISABLE)
	{
		ret = regmap_write_bits(chip->regmap, pull_en_reg, bit, 0);
	}
	else
	{
		ret = regmap_write_bits(chip->regmap, pull_en_reg, bit, bit);
	}

exit:
	mutex_unlock(&chip->i2c_lock);
	return ret;
}

static int adl1000_gpio_set_config(struct gpio_chip *gc, unsigned int offset,
				   unsigned long config)
{
	struct adl1000_chip *chip = gpiochip_get_data(gc);

	switch (pinconf_to_config_param(config)) {
	case PIN_CONFIG_BIAS_PULL_UP:
	case PIN_CONFIG_BIAS_PULL_PIN_DEFAULT:
	case PIN_CONFIG_BIAS_PULL_DOWN:
	case PIN_CONFIG_BIAS_DISABLE:
		return adl1000_gpio_set_pull_up_down(chip, offset, config);
	default:
		return -ENOTSUPP;
	}
}

static void adl1000_setup_gpio(struct adl1000_chip *chip, int gpios)
{
	struct gpio_chip *gc;

	gc = &chip->gpio_chip;

	gc->direction_input  = adl1000_gpio_direction_input;
	gc->direction_output = adl1000_gpio_direction_output;
	gc->get = adl1000_gpio_get_value;
	gc->set = adl1000_gpio_set_value;
	gc->get_direction = adl1000_gpio_get_direction;
	gc->set_config = adl1000_gpio_set_config;
	gc->can_sleep = true;

	gc->base = chip->gpio_start;
	gc->ngpio = gpios;
	gc->label = dev_name(&chip->client->dev);
	gc->parent = &chip->client->dev;
	gc->owner = THIS_MODULE;
	gc->names = chip->names;
}


static void adl1000_irq_mask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct adl1000_chip *chip = gpiochip_get_data(gc);
	irq_hw_number_t hwirq = irqd_to_hwirq(d);

	clear_bit(hwirq, chip->irq_mask);
	gpiochip_disable_irq(gc, hwirq);
}

static void adl1000_irq_unmask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct adl1000_chip *chip = gpiochip_get_data(gc);
	irq_hw_number_t hwirq = irqd_to_hwirq(d);

	gpiochip_enable_irq(gc, hwirq);
	set_bit(hwirq, chip->irq_mask);
}

static int adl1000_irq_set_wake(struct irq_data *d, unsigned int on)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct adl1000_chip *chip = gpiochip_get_data(gc);

	if (on)
		atomic_inc(&chip->wakeup_path);
	else
		atomic_dec(&chip->wakeup_path);

	return irq_set_irq_wake(chip->client->irq, on);
}

static void adl1000_irq_bus_lock(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct adl1000_chip *chip = gpiochip_get_data(gc);

	mutex_lock(&chip->irq_lock);
}

static void adl1000_irq_bus_sync_unlock(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct adl1000_chip *chip = gpiochip_get_data(gc);
	DECLARE_BITMAP(irq_mask, MAX_LINE);
	DECLARE_BITMAP(reg_direction, MAX_LINE);
	int level;

	{
		bitmap_complement(irq_mask, chip->irq_mask, gc->ngpio);

		/* Unmask enabled interrupts */
		adl1000_write_regs(chip, ADL1000_INT_MASK, irq_mask);
	}

	/* Switch direction to input if needed */
	adl1000_read_regs(chip, chip->regs->direction, reg_direction);

	bitmap_or(irq_mask, chip->irq_trig_fall, chip->irq_trig_raise, gc->ngpio);
	bitmap_complement(reg_direction, reg_direction, gc->ngpio);
	bitmap_and(irq_mask, irq_mask, reg_direction, gc->ngpio);

	/* Look for any newly setup interrupt */
	for_each_set_bit(level, irq_mask, gc->ngpio)
		adl1000_gpio_direction_input(&chip->gpio_chip, level);

	mutex_unlock(&chip->irq_lock);
}

static int adl1000_irq_set_type(struct irq_data *d, unsigned int type)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct adl1000_chip *chip = gpiochip_get_data(gc);
	irq_hw_number_t hwirq = irqd_to_hwirq(d);

	if (!(type & IRQ_TYPE_EDGE_BOTH)) {
		dev_err(&chip->client->dev, "irq %d: unsupported type %d\n",
			d->irq, type);
		return -EINVAL;
	}

	assign_bit(hwirq, chip->irq_trig_fall, type & IRQ_TYPE_EDGE_FALLING);
	assign_bit(hwirq, chip->irq_trig_raise, type & IRQ_TYPE_EDGE_RISING);

	return 0;
}

static void adl1000_irq_shutdown(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct adl1000_chip *chip = gpiochip_get_data(gc);
	irq_hw_number_t hwirq = irqd_to_hwirq(d);

	clear_bit(hwirq, chip->irq_trig_raise);
	clear_bit(hwirq, chip->irq_trig_fall);
}

static void adl1000_irq_print_chip(struct irq_data *data, struct seq_file *p)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);

	seq_printf(p, dev_name(gc->parent));
}

static const struct irq_chip adl1000_irq_chip = {
	.irq_mask		= adl1000_irq_mask,
	.irq_unmask		= adl1000_irq_unmask,
	.irq_set_wake		= adl1000_irq_set_wake,
	.irq_bus_lock		= adl1000_irq_bus_lock,
	.irq_bus_sync_unlock	= adl1000_irq_bus_sync_unlock,
	.irq_set_type		= adl1000_irq_set_type,
	.irq_shutdown		= adl1000_irq_shutdown,
	.irq_print_chip		= adl1000_irq_print_chip,
	.flags			= IRQCHIP_IMMUTABLE,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

static bool adl1000_irq_pending(struct adl1000_chip *chip, unsigned long *pending)
{
	struct gpio_chip *gc = &chip->gpio_chip;
	DECLARE_BITMAP(reg_direction, MAX_LINE);
	DECLARE_BITMAP(old_stat, MAX_LINE);
	DECLARE_BITMAP(cur_stat, MAX_LINE);
	DECLARE_BITMAP(new_stat, MAX_LINE);
	DECLARE_BITMAP(trigger, MAX_LINE);
	int ret;

	/* Read the current interrupt status from the device */
	ret = adl1000_read_regs(chip, ADL1000_INT_STAT, trigger);
	if (ret)
		return false;

	/* Apply filter for rising/falling edge selection */
	bitmap_replace(new_stat, chip->irq_trig_fall, chip->irq_trig_raise, cur_stat, gc->ngpio);
	bitmap_and(pending, new_stat, trigger, gc->ngpio);

	return !bitmap_empty(pending, gc->ngpio);
}

static irqreturn_t adl1000_irq_handler(int irq, void *devid)
{
	struct adl1000_chip *chip = devid;
	struct gpio_chip *gc = &chip->gpio_chip;
	DECLARE_BITMAP(pending, MAX_LINE);
	int level;
	bool ret;

	bitmap_zero(pending, MAX_LINE);

	mutex_lock(&chip->i2c_lock);
	ret = adl1000_irq_pending(chip, pending);

	mutex_unlock(&chip->i2c_lock);

	if (ret) {
		ret = 0;

		for_each_set_bit(level, pending, gc->ngpio) {
			int nested_irq = irq_find_mapping(gc->irq.domain, level);

			if (unlikely(nested_irq <= 0)) {
				dev_warn_ratelimited(gc->parent, "unmapped interrupt %d\n", level);
				continue;
			}

			handle_nested_irq(nested_irq);
			ret = 1;
		}
	}
	return IRQ_RETVAL(ret);
}

static int adl1000_irq_setup(struct adl1000_chip *chip, int irq_base)
{
	struct i2c_client *client = chip->client;
	DECLARE_BITMAP(reg_direction, MAX_LINE);
	DECLARE_BITMAP(irq_stat, MAX_LINE);
	struct gpio_irq_chip *girq;
	int ret;

	if (!client->irq)
		return 0;

	if (irq_base == -1)
		return 0;

	ret = adl1000_read_regs(chip, chip->regs->input, irq_stat);
	if (ret)
		return ret;

	/*
	 * There is no way to know which GPIO line generated the
	 * interrupt.  We have to rely on the previous read for
	 * this purpose.
	 */
	adl1000_read_regs(chip, chip->regs->direction, reg_direction);
	bitmap_and(chip->irq_stat, irq_stat, reg_direction, chip->gpio_chip.ngpio);
	mutex_init(&chip->irq_lock);

	girq = &chip->gpio_chip.irq;
	gpio_irq_chip_set_chip(girq, &adl1000_irq_chip);
	/* This will let us handle the parent IRQ in the driver */
	girq->parent_handler = NULL;
	girq->num_parents = 0;
	girq->parents = NULL;
	girq->default_type = IRQ_TYPE_NONE;
	girq->handler = handle_simple_irq;
	girq->threaded = true;
	girq->first = irq_base; /* FIXME: get rid of this */

	ret = devm_request_threaded_irq(&client->dev, client->irq,
					NULL, adl1000_irq_handler,
					IRQF_ONESHOT | IRQF_SHARED,
					dev_name(&client->dev), chip);
	if (ret) {
		dev_err(&client->dev, "failed to request irq %d\n",
			client->irq);
		return ret;
	}

	return 0;
}



static int device_pca95xx_init(struct adl1000_chip *chip, u32 invert)
{
	DECLARE_BITMAP(val, MAX_LINE);
	u8 regaddr;
	int ret;

	regaddr = chip->recalc_addr(chip, chip->regs->output, 0);
	ret = regcache_sync_region(chip->regmap, regaddr,
				   regaddr + NBANK(chip) - 1);
	if (ret)
		goto out;

	regaddr = chip->recalc_addr(chip, chip->regs->direction, 0);
	ret = regcache_sync_region(chip->regmap, regaddr,
				   regaddr + NBANK(chip) - 1);
	if (ret)
		goto out;

out:
	return ret;
}



static int adl1000_probe(struct i2c_client *client)
{
	struct adl1000_platform_data *pdata;
	struct adl1000_chip *chip;
	int irq_base = 0;
	int ret;
	u32 invert = 0;
	struct regulator *reg;
	const struct regmap_config *regmap_config;

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (chip == NULL)
		return -ENOMEM;

	pdata = dev_get_platdata(&client->dev);
	if (pdata) {
		irq_base = pdata->irq_base;
		chip->gpio_start = pdata->gpio_base;
		invert = pdata->invert;
		chip->names = pdata->names;
	} else {
		struct gpio_desc *reset_gpio;

		chip->gpio_start = -1;
		irq_base = 0;

		reset_gpio = devm_gpiod_get_optional(&client->dev, "reset",
						     GPIOD_OUT_LOW);
		if (IS_ERR(reset_gpio))
			return PTR_ERR(reset_gpio);
	}

	chip->client = client;
	chip->driver_data = (uintptr_t)i2c_get_match_data(client);
	if (!chip->driver_data)
		return -ENODEV;

	reg = devm_regulator_get(&client->dev, "vcc");
	if (IS_ERR(reg))
		return dev_err_probe(&client->dev, PTR_ERR(reg), "reg get err\n");

	ret = regulator_enable(reg);
	if (ret) {
		dev_err(&client->dev, "reg en err: %d\n", ret);
		return ret;
	}
	chip->regulator = reg;

	i2c_set_clientdata(client, chip);
	adl1000_setup_gpio(chip, chip->driver_data );

	regmap_config = &adl1000_i2c_regmap;
	chip->recalc_addr = adl1000_recalc_addr;

	chip->regmap = devm_regmap_init_i2c(client, regmap_config);
	if (IS_ERR(chip->regmap)) {
		ret = PTR_ERR(chip->regmap);
		goto err_exit;
	}

	regcache_mark_dirty(chip->regmap);

	mutex_init(&chip->i2c_lock);
	/*
	 * In case we have an i2c-mux controlled by a GPIO provided by an
	 * expander using the same driver higher on the device tree, read the
	 * i2c adapter nesting depth and use the retrieved value as lockdep
	 * subclass for chip->i2c_lock.
	 *
	 * REVISIT: This solution is not complete. It protects us from lockdep
	 * false positives when the expander controlling the i2c-mux is on
	 * a different level on the device tree, but not when it's on the same
	 * level on a different branch (in which case the subclass number
	 * would be the same).
	 *
	 * TODO: Once a correct solution is developed, a similar fix should be
	 * applied to all other i2c-controlled GPIO expanders (and potentially
	 * regmap-i2c).
	 */
	lockdep_set_subclass(&chip->i2c_lock,
			     i2c_adapter_depth(client->adapter));

	ret = device_reset(&client->dev);
	if (ret == -EPROBE_DEFER)
		return -EPROBE_DEFER;

	/* initialize cached registers from their original values.
	 * we can't share this chip with another i2c master.
	 */

	chip->regs = &adl1000_regs;
	ret = device_pca95xx_init(chip, invert);

	if (ret)
		goto err_exit;

	ret = adl1000_irq_setup(chip, irq_base);
	if (ret)
		goto err_exit;

	ret = devm_gpiochip_add_data(&client->dev, &chip->gpio_chip, chip);
	if (ret)
		goto err_exit;

	if (pdata && pdata->setup) {
		ret = pdata->setup(client, chip->gpio_chip.base,
				   chip->gpio_chip.ngpio, pdata->context);
		if (ret < 0)
			dev_warn(&client->dev, "setup failed, %d\n", ret);
	}

	return 0;

err_exit:
	regulator_disable(chip->regulator);
	return ret;
}

static void adl1000_remove(struct i2c_client *client)
{
	struct adl1000_platform_data *pdata = dev_get_platdata(&client->dev);
	struct adl1000_chip *chip = i2c_get_clientdata(client);

	if (pdata && pdata->teardown) {
		pdata->teardown(client, chip->gpio_chip.base,
				chip->gpio_chip.ngpio, pdata->context);
	}

	regulator_disable(chip->regulator);
}


static const struct of_device_id adl1000_dt_ids[] = {
	{ .compatible = "adlink,adl1000", },
	{ }
};

MODULE_DEVICE_TABLE(of, adl1000_dt_ids);


static struct i2c_driver adl1000_driver = {
	.driver = {
		.name	= "adl1000",
		.of_match_table = adl1000_dt_ids,
	},
	.probe		= adl1000_probe,
	.remove		= adl1000_remove,
	.id_table	= adl1000_id,
};

static int __init adl1000_init(void)
{
	return i2c_add_driver(&adl1000_driver);
}
/* register after i2c postcore initcall and before
 * subsys initcalls that may rely on these GPIOs
 */
subsys_initcall(adl1000_init);

static void __exit adl1000_exit(void)
{
	i2c_del_driver(&adl1000_driver);
}
module_exit(adl1000_exit);

MODULE_AUTHOR("Jimmy Yu <jimmy.yu@adlinktech.com>");
MODULE_DESCRIPTION("GPIO expander driver for PCA953x");
MODULE_LICENSE("GPL");

