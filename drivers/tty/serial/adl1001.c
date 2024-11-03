// SPDX-License-Identifier: GPL-2.0+
/*
 *  ADL1001 serial driver
 *
 *
 *  Based on max310x.c, by Jimmy Yu <jimmy.yu@adlinktech.com>
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/driver.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/serial_core.h>
#include <linux/serial.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/uaccess.h>

#define CIRC_EMPTY_SLEEP	10
#define TX_BUSY_SLEEP		10
#define ADL1001_BAUD_MIN	4800
#define ADL1001_BAUD_MAX	115200
#define ADL1001_NAME			"adl1001"
#define ADL1001_UART_NRMAX		1

/* ADL1001 register definitions */
#define ADL1001_BAUD			(0x1)
#define ADL1001_RXFIFOLVL_REG	(0x2) /* RX FIFO level */
#define ADL1001_RHR_REG			(0x03) /* RX FIFO */
#define ADL1001_THR_REG			(0x04) /* TX FIFO */
#define ADL1001_TX_BUSY			(0x05) /* TX BUSY */
#define ADL1001_WORD_LENGTH		(0x06) 
#define ADL1001_STOP_BITS		(0x07) 
#define ADL1001_PARITY			(0x08) 
#define ADL1001_HWCONTROL			(0x09) 

#define ADL1001_HWCONTROL_RTS_CTS	(0x08)
#define ADL1001_HWCONTROL_NONE		(0x01)

#define ADL1001_BAUD_4800			(0x01)
#define ADL1001_BAUD_9600			(0x02)
#define ADL1001_BAUD_19200			(0x04)
#define ADL1001_BAUD_38400			(0x08)
#define ADL1001_BAUD_57600			(0x10)
#define ADL1001_BAUD_115200			(0x20)

#define ADL1001_WORD_LENGTH_7B		(0x01) 
#define ADL1001_WORD_LENGTH_8B		(0x02) 

#define ADL1001_STOP_BITS_1		(0x02)
#define ADL1001_STOP_BITS_2		(0x08)

#define ADL1001_PARITY_NONE			(0x01) 
#define ADL1001_PARITY_EVEN			(0x02) 
#define ADL1001_PARITY_ODD			(0x04)
#define ADL1001_REG_1F			(0x1f)

/* Global commands */

/* Misc definitions */
#define ADL1001_FIFO_SIZE		(128)

struct adl1001_devtype {
	char	name[9];
	int	nr;
	void	(*power)(struct uart_port *, int);
};

struct adl1001_one {
	struct uart_port	port;
	struct work_struct	tx_work;
	struct regmap		*regmap;

	u8 rx_buf[ADL1001_FIFO_SIZE];
};
#define to_adl1001_port(_port) \
	container_of(_port, struct adl1001_one, port)

struct adl1001_port {
	const struct adl1001_devtype *devtype;
	struct regmap		*regmap;
	struct clk		*clk;
	struct adl1001_one	p[];
};

static struct uart_driver adl1001_uart = {
	.owner		= THIS_MODULE,
	.driver_name	= ADL1001_NAME,
	.dev_name	= "ttyADL",
	.nr		= ADL1001_UART_NRMAX,
};

static DECLARE_BITMAP(adl1001_lines, ADL1001_UART_NRMAX);

static u8 adl1001_port_read(struct uart_port *port, u8 reg)
{
	struct adl1001_one *one = to_adl1001_port(port);
	unsigned int val = 0;

	regmap_read(one->regmap, reg, &val);

	return val;
}

static void adl1001_port_write(struct uart_port *port, u8 reg, u8 val)
{
	struct adl1001_one *one = to_adl1001_port(port);

	regmap_write(one->regmap, reg, val);
}

static void adl1001_power(struct uart_port *port, int on)
{

}

static const struct adl1001_devtype adl1001_devtype = {
	.name	= "ADL1001",
	.nr	= 1,
	.power	= adl1001_power,
};

static bool adl1001_reg_writeable(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case ADL1001_RXFIFOLVL_REG:
	case ADL1001_TX_BUSY:
		return false;
	default:
		break;
	}

	return true;

}

static bool adl1001_reg_volatile(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case ADL1001_RHR_REG:
	case ADL1001_THR_REG:
	case ADL1001_RXFIFOLVL_REG:
	case ADL1001_TX_BUSY:
		return true;
	default:
		break;
	}

	return false;
}

static bool adl1001_reg_precious(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case ADL1001_RHR_REG:
		return true;
	default:
		break;
	}

	return false;
}

static bool adl1001_reg_noinc(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case ADL1001_RHR_REG:
	case ADL1001_THR_REG:
		return true;
	default:
		break;
	}

	return false;
}

static int adl1001_set_baud(struct uart_port *port, int baud)
{
	u8 val;
	switch (baud) {
	case 115200:
		val = ADL1001_BAUD_115200;
		break;
	case 57600:
		val = ADL1001_BAUD_57600;
		break;
	case 38400:
		val = ADL1001_BAUD_38400;
		break;
	case 19200:
		val = ADL1001_BAUD_19200;
		break;
	case 9600:
		val = ADL1001_BAUD_9600;
		break;
	case 4800:
		val = ADL1001_BAUD_4800;
		break;
	default:
		val = ADL1001_BAUD_115200;
		baud = 115200;
		break;
	}
	adl1001_port_write(port, ADL1001_BAUD, val);
	/* Return the actual baud rate we just programmed */
	return baud;
}

static void adl1001_batch_write(struct uart_port *port, u8 *txbuf, unsigned int len)
{
	u8 buf[ADL1001_FIFO_SIZE+1];
	struct adl1001_one *one = to_adl1001_port(port);

	buf[0]=len;
	memcpy(buf+1,txbuf,len);

	while (adl1001_port_read(port, ADL1001_TX_BUSY))
	{
		dev_dbg(port->dev, "TX Busy !!! sleep %dms\n",TX_BUSY_SLEEP);
		msleep(TX_BUSY_SLEEP);
	}

	regmap_noinc_write(one->regmap, ADL1001_THR_REG, buf, len+1);
}

static void adl1001_batch_read(struct uart_port *port, u8 *rxbuf, unsigned int len)
{
	struct adl1001_one *one = to_adl1001_port(port);

	regmap_noinc_read(one->regmap, ADL1001_RHR_REG, rxbuf, len);
}

static void adl1001_handle_rx(struct uart_port *port, unsigned int rxlen)
{
	struct adl1001_one *one = to_adl1001_port(port);
	unsigned int sts, i;
	u8 flag;

	{
		adl1001_batch_read(port, one->rx_buf, rxlen);

		port->icount.rx += rxlen;
		flag = TTY_NORMAL;

		for (i = 0; i < rxlen; ++i)
			uart_insert_char(port, sts, 0, one->rx_buf[i], flag);

	} 

	tty_flip_buffer_push(&port->state->port);
}

static void adl1001_handle_tx(struct uart_port *port)
{
	struct circ_buf *xmit = &port->state->xmit;
	unsigned int txlen, to_send, until_end;

	while(1)
	{
		if (unlikely(port->x_char)) {
			dev_dbg(port->dev, "send x_char\n");
			adl1001_batch_write(port, &(port->x_char), 1);
			port->icount.tx++;
			port->x_char = 0;
			continue;
		}
		
		if (uart_tx_stopped(port))
		{
			dev_dbg(port->dev, "tx stop\n");
			break;
		}
	
		while (uart_circ_empty(xmit))
		{
			dev_dbg(port->dev, "TX CIRC EMPTY !!! sleep %dms\n",TX_BUSY_SLEEP);
			msleep(CIRC_EMPTY_SLEEP);
		}
		/* Get length of data pending in circular buffer */
		to_send = uart_circ_chars_pending(xmit);
		until_end = CIRC_CNT_TO_END(xmit->head, xmit->tail, UART_XMIT_SIZE);
		
		if (likely(to_send)) {
			/* Limit to size of TX FIFO */
			txlen = 0;
			txlen = port->fifosize - txlen;
			to_send = (to_send > txlen) ? txlen : to_send;

			if (until_end < to_send) {
				/* It's a circ buffer -- wrap around.
			 	* We could do that in one SPI transaction, but meh. */
				adl1001_batch_write(port, xmit->buf + xmit->tail, until_end);
				adl1001_batch_write(port, xmit->buf, to_send - until_end);
			} else {
				adl1001_batch_write(port, xmit->buf + xmit->tail, to_send);
			}
			uart_xmit_advance(port, to_send);
		}

		if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
			uart_write_wakeup(port);
	}
}

static void adl1001_start_tx(struct uart_port *port)
{
	struct adl1001_one *one = to_adl1001_port(port);

	schedule_work(&one->tx_work);
}

static irqreturn_t adl1001_port_irq(struct adl1001_port *s, int portno)
{
	struct uart_port *port = &s->p[portno].port;
	irqreturn_t res = IRQ_NONE;

	{
		unsigned int rxlen;

		/* Read IRQ status & RX FIFO level */
		rxlen = adl1001_port_read(port, ADL1001_RXFIFOLVL_REG);

		res = IRQ_HANDLED;

		if (rxlen)
			adl1001_handle_rx(port, rxlen);
	} 
	return res;
}

static irqreturn_t adl1001_ist(int irq, void *dev_id)
{
	struct adl1001_port *s = (struct adl1001_port *)dev_id;
	bool handled = false;
#if 0
	if (s->devtype->nr > 1) {
		do {
			unsigned int val = ~0;

			//WARN_ON_ONCE(regmap_read(s->regmap,
			//			 ADL1001_GLOBALIRQ_REG, &val));
			val = ((1 << s->devtype->nr) - 1) & ~val;
			if (!val)
				break;
			if (adl1001_port_irq(s, fls(val) - 1) == IRQ_HANDLED)
				handled = true;
		} while (1);
	} else 
#endif
	{
		if (adl1001_port_irq(s, 0) == IRQ_HANDLED)
			handled = true;
	}

	return IRQ_RETVAL(handled);
}

static void adl1001_tx_proc(struct work_struct *ws)
{
	struct adl1001_one *one = container_of(ws, struct adl1001_one, tx_work);
	
	adl1001_handle_tx(&one->port);
}

static unsigned int adl1001_tx_empty(struct uart_port *port)
{
	u8 lvl = adl1001_port_read(port, ADL1001_TX_BUSY);

	return lvl ? 0 : TIOCSER_TEMT;
}

static unsigned int adl1001_get_mctrl(struct uart_port *port)
{
	/* DCD and DSR are not wired and CTS/RTS is handled automatically
	 * so just indicate DSR and CAR asserted
	 */
	return TIOCM_DSR | TIOCM_CAR;
}

static void adl1001_set_mctrl(struct uart_port *port, unsigned int mctrl)
{

}

static void adl1001_break_ctl(struct uart_port *port, int break_state)
{

}

static void adl1001_set_termios(struct uart_port *port,
				struct ktermios *termios,
				const struct ktermios *old)
{
	int baud;

	/* Mask termios capabilities we don't support */
	termios->c_cflag &= ~CMSPAR;
	termios->c_cflag &= ~(CMSPAR);

	{
		u8 val;
		if (termios->c_cflag & CRTSCTS) {
			/* Enable AUTORTS and AUTOCTS */
			port->status |= UPSTAT_AUTOCTS | UPSTAT_AUTORTS;
			val = ADL1001_HWCONTROL_RTS_CTS;
		}
		else
			val = ADL1001_HWCONTROL_NONE;
		adl1001_port_write(port, ADL1001_HWCONTROL , val);
	}

	//only support CS7,CS8
	if (((termios->c_cflag & CSIZE) == CS5 )||((termios->c_cflag & CSIZE) == CS6 ))
	{
		termios->c_cflag &= ~(CSIZE);
		termios->c_cflag |= (CS8);
	}

	{
		u8 val;
		switch (termios->c_cflag & CSIZE) {
		case CS7:
			val = ADL1001_WORD_LENGTH_7B;
			break;
		case CS8:
		default:
			val = ADL1001_WORD_LENGTH_8B;
			break;
		}
		adl1001_port_write(port, ADL1001_WORD_LENGTH, val);
	}

	if (termios->c_cflag & CSTOPB)
		adl1001_port_write(port, ADL1001_STOP_BITS, ADL1001_STOP_BITS_2); //2 stop bit
	else
		adl1001_port_write(port, ADL1001_STOP_BITS, ADL1001_STOP_BITS_1); //1 stop bit


	if (termios->c_cflag & PARENB) {
		if (termios->c_cflag & PARODD)
			adl1001_port_write(port, ADL1001_PARITY, ADL1001_PARITY_ODD);
		else
			adl1001_port_write(port, ADL1001_PARITY, ADL1001_PARITY_EVEN);
	}
	else
		adl1001_port_write(port, ADL1001_PARITY, ADL1001_PARITY_NONE);

	/* Set status ignore mask */
	port->ignore_status_mask = 0;

	baud = uart_get_baud_rate(port, termios, old,ADL1001_BAUD_MIN,ADL1001_BAUD_MAX);

	/* Setup baudrate generator */
	baud = adl1001_set_baud(port, baud);

	/* Update timeout according to new baud rate */
	uart_update_timeout(port, termios->c_cflag, baud);
}

static int adl1001_startup(struct uart_port *port)
{
	return 0;
}

static void adl1001_shutdown(struct uart_port *port)
{

}

static const char *adl1001_type(struct uart_port *port)
{
	struct adl1001_port *s = dev_get_drvdata(port->dev);

	return (port->type == 124) ? s->devtype->name : NULL;
}

static int adl1001_request_port(struct uart_port *port)
{
	/* Do nothing */
	return 0;
}

static void adl1001_config_port(struct uart_port *port, int flags)
{
	if (flags & UART_CONFIG_TYPE)
		port->type = 124;
}

static int adl1001_verify_port(struct uart_port *port, struct serial_struct *s)
{
	if ((s->type != PORT_UNKNOWN) && (s->type != 124))
		return -EINVAL;
	if (s->irq != port->irq)
		return -EINVAL;

	return 0;
}

static void adl1001_null_void(struct uart_port *port)
{
	/* Do nothing */
}

static const struct uart_ops adl1001_ops = {
	.tx_empty	= adl1001_tx_empty,
	.set_mctrl	= adl1001_set_mctrl,
	.get_mctrl	= adl1001_get_mctrl,
	.stop_tx	= adl1001_null_void,
	.start_tx	= adl1001_start_tx,
	.stop_rx	= adl1001_null_void,
	.break_ctl	= adl1001_break_ctl,
	.startup	= adl1001_startup,
	.shutdown	= adl1001_shutdown,
	.set_termios	= adl1001_set_termios,
	.type		= adl1001_type,
	.request_port	= adl1001_request_port,
	.release_port	= adl1001_null_void,
	.config_port	= adl1001_config_port,
	.verify_port	= adl1001_verify_port,
};

static int __maybe_unused adl1001_suspend(struct device *dev)
{
	struct adl1001_port *s = dev_get_drvdata(dev);
	int i;

	for (i = 0; i < s->devtype->nr; i++) {
		uart_suspend_port(&adl1001_uart, &s->p[i].port);
		s->devtype->power(&s->p[i].port, 0);
	}

	return 0;
}

static int __maybe_unused adl1001_resume(struct device *dev)
{
	struct adl1001_port *s = dev_get_drvdata(dev);
	int i;

	for (i = 0; i < s->devtype->nr; i++) {
		s->devtype->power(&s->p[i].port, 1);
		uart_resume_port(&adl1001_uart, &s->p[i].port);
	}

	return 0;
}

static SIMPLE_DEV_PM_OPS(adl1001_pm_ops, adl1001_suspend, adl1001_resume);

static int adl1001_probe(struct device *dev, const struct adl1001_devtype *devtype,
//			 const struct adl1001_if_cfg *if_cfg,
			 struct regmap *regmaps[], int irq)
{
	int i, ret;
	struct adl1001_port *s;
	u32 uartclk = 0;

	for (i = 0; i < devtype->nr; i++)
		if (IS_ERR(regmaps[i]))
			return PTR_ERR(regmaps[i]);

	/* Alloc port structure */
	s = devm_kzalloc(dev, struct_size(s, p, devtype->nr), GFP_KERNEL);
	if (!s) {
		dev_err(dev, "Error allocating port structure\n");
		return -ENOMEM;
	}

	s->regmap = regmaps[0];
	s->devtype = devtype;
	//s->if_cfg = if_cfg;
	dev_set_drvdata(dev, s);

	/* Check device to ensure we are talking to what we expect */

	for (i = 0; i < devtype->nr; i++) {
		unsigned int line;

		line = find_first_zero_bit(adl1001_lines, ADL1001_UART_NRMAX);
		if (line == ADL1001_UART_NRMAX) {
			ret = -ERANGE;
			goto out_uart;
		}

		/* Initialize port data */
		s->p[i].port.line	= line;
		s->p[i].port.dev	= dev;
		s->p[i].port.irq	= irq;
		s->p[i].port.type	= 124;
		s->p[i].port.fifosize	= ADL1001_FIFO_SIZE;
		s->p[i].port.flags	= UPF_FIXED_TYPE | UPF_LOW_LATENCY;
		s->p[i].port.iotype	= UPIO_PORT;
		s->p[i].port.iobase	= i;
		/*
		 * Use all ones as membase to make sure uart_configure_port() in
		 * serial_core.c does not abort for SPI/I2C devices where the
		 * membase address is not applicable.
		 */
		s->p[i].port.membase	= (void __iomem *)~0;
		s->p[i].port.uartclk	= uartclk;
		s->p[i].port.ops	= &adl1001_ops;
		s->p[i].regmap		= regmaps[i];

		INIT_WORK(&s->p[i].tx_work, adl1001_tx_proc);

		/* Register port */
		ret = uart_add_one_port(&adl1001_uart, &s->p[i].port);
		if (ret) {
			s->p[i].port.dev = NULL;
			goto out_uart;
		}
		set_bit(line, adl1001_lines);

		/* Go to suspend mode */
		devtype->power(&s->p[i].port, 0);
	}

	/* Setup interrupt */
	ret = devm_request_threaded_irq(dev, irq, NULL, adl1001_ist,
					IRQF_ONESHOT | IRQF_SHARED, dev_name(dev), s);
	if (!ret)
	{
		return 0;
	}
	dev_err(dev, "Unable to reguest IRQ %i\n", irq);

out_uart:
	for (i = 0; i < devtype->nr; i++) {
		if (s->p[i].port.dev) {
			uart_remove_one_port(&adl1001_uart, &s->p[i].port);
			clear_bit(s->p[i].port.line, adl1001_lines);
		}
	}

	clk_disable_unprepare(s->clk);

	return ret;
}

static void adl1001_remove(struct device *dev)
{
	struct adl1001_port *s = dev_get_drvdata(dev);
	int i;

	for (i = 0; i < s->devtype->nr; i++) {
		cancel_work_sync(&s->p[i].tx_work);
		uart_remove_one_port(&adl1001_uart, &s->p[i].port);
		clear_bit(s->p[i].port.line, adl1001_lines);
		s->devtype->power(&s->p[i].port, 0);
	}

	clk_disable_unprepare(s->clk);
}

static const struct of_device_id __maybe_unused adl1001_dt_ids[] = {
	{ .compatible = "adlink,adl1001",	.data = &adl1001_devtype, },
	{ }
};
MODULE_DEVICE_TABLE(of, adl1001_dt_ids);

#ifdef CONFIG_I2C

static struct regmap_config regcfg_i2c = {
	.reg_bits = 8,
	.val_bits = 8,
	.cache_type = REGCACHE_RBTREE,
	.writeable_reg = adl1001_reg_writeable,
	.volatile_reg = adl1001_reg_volatile,
	.precious_reg = adl1001_reg_precious,
	.max_register = ADL1001_REG_1F,
	.writeable_noinc_reg = adl1001_reg_noinc,
	.readable_noinc_reg = adl1001_reg_noinc,
	.max_raw_read = ADL1001_FIFO_SIZE,
	.max_raw_write = ADL1001_FIFO_SIZE,
};

static int adl1001_i2c_probe(struct i2c_client *client)
{
	const struct adl1001_devtype *devtype =
			device_get_match_data(&client->dev);
	struct i2c_client *port_client;
	struct regmap *regmaps[4];
	unsigned int i;
	u8 port_addr;

	dev_info(&client->dev, "adl1001_i2c_probe client->addr:0x%x\n",client->addr);

	regmaps[0] = devm_regmap_init_i2c(client, &regcfg_i2c);

	return adl1001_probe(&client->dev, devtype,
			     regmaps, client->irq);
}

static void adl1001_i2c_remove(struct i2c_client *client)
{
	adl1001_remove(&client->dev);
}

static struct i2c_driver adl1001_i2c_driver = {
	.driver = {
		.name		= ADL1001_NAME,
		.of_match_table	= adl1001_dt_ids,
		.pm		= &adl1001_pm_ops,
	},
	.probe		= adl1001_i2c_probe,
	.remove		= adl1001_i2c_remove,
};
#endif

static int __init adl1001_uart_init(void)
{
	int ret;

	bitmap_zero(adl1001_lines, ADL1001_UART_NRMAX);

	ret = uart_register_driver(&adl1001_uart);
	if (ret)
		return ret;

#ifdef CONFIG_I2C
	ret = i2c_add_driver(&adl1001_i2c_driver);
	if (ret)
		goto err_i2c_register;
#endif

	return 0;
#ifdef CONFIG_I2C
err_i2c_register:
	uart_unregister_driver(&adl1001_uart);

	return ret;
#endif
}
module_init(adl1001_uart_init);

static void __exit adl1001_uart_exit(void)
{
#ifdef CONFIG_I2C
	i2c_del_driver(&adl1001_i2c_driver);
#endif

	uart_unregister_driver(&adl1001_uart);
}
module_exit(adl1001_uart_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jimmy Yu <jimmy.yu@adlinktech.com>");
MODULE_DESCRIPTION("ADL1001 serial driver");
