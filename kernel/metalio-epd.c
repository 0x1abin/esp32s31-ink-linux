// SPDX-License-Identifier: GPL-2.0-only
/* E0470A14-AF, Metalio E-Ink4 Plus v1.4/v1.5, ESP32-S31. */
#include <linux/bitfield.h>
#include <linux/completion.h>
#include <linux/compat.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kref.h>
#include <linux/ktime.h>
#include <linux/mfd/syscon.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <linux/metalio_epd.h>
#include "metalio-epd-phase.h"
#include "metalio-epd-regs.h"

#define EPD_DESC_COUNT DIV_ROUND_UP(METALIO_EPD_PHASE_BYTES, EPD_DESC_CHUNK)
#define EPD_DESC_OFFSET ALIGN(METALIO_EPD_PHASE_BYTES, 64U)
#define EPD_DESC_BYTES (EPD_DESC_COUNT * sizeof(struct epd_descriptor))
#define EPD_DMA_BYTES (EPD_DESC_OFFSET + EPD_DESC_BYTES)
#define EPD_SCAN_TIMEOUT_MS 100U
#define TPS_ENABLE 0x01
#define TPS_VCOM1 0x03
#define TPS_VCOM2 0x04
#define TPS_UPSEQ0 0x09
#define TPS_UPSEQ1 0x0a
#define TPS_PG 0x0f
#define TPS_REVID 0x10

struct epd_descriptor {
	__le32 control;
	__le32 buffer;
	__le32 next;
	__le32 padding;
} __aligned(8);

struct metalio_epd {
	struct device *dev;
	struct miscdevice misc;
	struct kref ref;
	struct mutex lock;
	struct work_struct work;
	wait_queue_head_t wait;
	struct completion scan_done;
	spinlock_t scan_lock;
	void __iomem *lcd, *dma, *rmt, *rmtmem;
	struct regmap *clkrst, *system, *matrix, *iomux;
	struct gpio_desc *oe, *mode, *border, *stv;
	struct gpio_desc *wakeup, *pwrup, *vcom, *powergood, *panel_power;
	struct gpio_descs *bus;
	struct i2c_client *tps;
	u8 *pending, *previous, *phase;
	dma_addr_t phase_dma, descriptors_dma;
	struct epd_descriptor *descriptors;
	struct metalio_epd_status status;
	u32 refresh_mode, vcom_mv;
	int lcd_irq, dma_irq, scan_error;
	bool opened, removed, abort, frame_valid, known, completion_pending;
	bool dma_done;
};

static int epd_hw_init(struct metalio_epd *epd);

static void epd_destroy(struct kref *ref)
{
	struct metalio_epd *epd = container_of(ref, struct metalio_epd, ref);

	kvfree(epd->pending);
	kvfree(epd->previous);
	kfree(epd);
}

static void epd_put(void *data)
{
	struct metalio_epd *epd = data;

	kref_put(&epd->ref, epd_destroy);
}

static void epd_set(void __iomem *base, unsigned int reg, u32 bits)
{
	writel(readl(base + reg) | bits, base + reg);
}

static void epd_stop_scan(struct metalio_epd *epd)
{
	writel(0, epd->lcd + EPD_LCD_INT_ENA);
	writel(0, epd->dma + EPD_DMA_ENA);
	writel(EPD_LCD_USER_CONFIGURATION, epd->lcd + EPD_LCD_USER);
	writel(EPD_LCD_USER_CONFIGURATION | EPD_LCD_UPDATE, epd->lcd + EPD_LCD_USER);
	writel(EPD_DMA_STOP, epd->dma + EPD_DMA_LINK1);
	epd_set(epd->dma, EPD_DMA_CONF0, EPD_DMA_ABORT);
	writel(EPD_RMT_CONFIGURATION | EPD_RMT_STOP, epd->rmt + EPD_RMT_CONF);
	writel(EPD_RMT_CONFIGURATION | EPD_RMT_STOP | EPD_RMT_UPDATE,
	       epd->rmt + EPD_RMT_CONF);
}

static irqreturn_t epd_lcd_irq(int irq, void *data)
{
	struct metalio_epd *epd = data;
	u32 status = readl(epd->lcd + EPD_LCD_INT_ST) & EPD_LCD_EVENTS;

	if (!status)
		return IRQ_NONE;
	writel(status, epd->lcd + EPD_LCD_INT_CLR);
	if (status & EPD_LCD_UNDERRUN) {
		WRITE_ONCE(epd->scan_error, -EPIPE);
		gpiod_set_value(epd->oe, 0);
		epd_stop_scan(epd);
		dev_err_ratelimited(epd->dev, "LCD scan underrun: status=%#x\n", status);
	}
	complete(&epd->scan_done);
	return IRQ_HANDLED;
}

static irqreturn_t epd_dma_irq(int irq, void *data)
{
	struct metalio_epd *epd = data;
	u32 status = readl(epd->dma + EPD_DMA_ST) & EPD_DMA_EVENTS;

	if (!status)
		return IRQ_NONE;
	writel(status, epd->dma + EPD_DMA_CLR);
	if (status & EPD_DMA_EOF)
		WRITE_ONCE(epd->dma_done, true);
	if (status & EPD_DMA_ERRORS) {
		WRITE_ONCE(epd->scan_error, -EIO);
		gpiod_set_value(epd->oe, 0);
		epd_stop_scan(epd);
		dev_err_ratelimited(epd->dev, "DMA scan error: status=%#x\n", status);
		complete(&epd->scan_done);
	}
	return IRQ_HANDLED;
}

/* The reference TPS driver uses a STOP between register address and read. */
static int epd_tps_read(struct metalio_epd *epd, u8 reg)
{
	u8 value;
	int ret = i2c_master_send(epd->tps, &reg, 1);

	if (ret != 1)
		return ret < 0 ? ret : -EIO;
	ret = i2c_master_recv(epd->tps, &value, 1);
	return ret == 1 ? value : ret < 0 ? ret : -EIO;
}

static int epd_tps_write(struct metalio_epd *epd, u8 reg, u8 value)
{
	u8 bytes[] = { reg, value };
	int ret = i2c_master_send(epd->tps, bytes, sizeof(bytes));

	return ret == sizeof(bytes) ? 0 : ret < 0 ? ret : -EIO;
}

static int epd_wait_pg(struct metalio_epd *epd, bool powered)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(250);
	int value;

	do {
		value = gpiod_get_value_cansleep(epd->powergood);
		if (value < 0)
			return value;
		if (!!value == powered)
			return 0;
		usleep_range(1000, 2000);
	} while (time_before(jiffies, deadline));
	return -ETIMEDOUT;
}

static int epd_poweroff(struct metalio_epd *epd)
{
	int ret = 0, error;

	epd_stop_scan(epd);
	gpiod_set_value(epd->oe, 0);
	gpiod_set_value(epd->mode, 0);
	gpiod_set_value(epd->stv, 0);
	/* Panel section 6.1: XOE must precede VPOS/VNEG removal by >=12 us. */
	udelay(12);
	error = gpiod_direction_output(epd->vcom, 0);
	if (error)
		ret = error;
	error = gpiod_direction_output(epd->pwrup, 0);
	if (error && !ret)
		ret = error;
	error = epd_wait_pg(epd, false);
	if (error && !ret)
		ret = error;
	error = gpiod_direction_output(epd->wakeup, 0);
	if (error && !ret)
		ret = error;
	msleep(120);
	return ret;
}

static int epd_poweron(struct metalio_epd *epd)
{
	unsigned long deadline;
	int ret, value;

	gpiod_set_value(epd->oe, 0);
	gpiod_set_value(epd->mode, 0);
	gpiod_set_value(epd->border, 0);
	gpiod_set_value(epd->stv, 1);
	ret = gpiod_direction_output(epd->panel_power, 1);
	if (ret)
		return ret;
	msleep(80);
	ret = gpiod_direction_output(epd->pwrup, 0);
	if (ret)
		return ret;
	ret = gpiod_direction_output(epd->vcom, 0);
	if (ret)
		return ret;
	ret = gpiod_direction_output(epd->wakeup, 1);
	if (ret)
		return ret;
	msleep(50);
	ret = epd_tps_read(epd, TPS_REVID);
	if (ret < 0)
		return ret;
	ret = epd_tps_write(epd, TPS_UPSEQ0, 0xe4);
	if (ret)
		return ret;
	ret = epd_tps_write(epd, TPS_UPSEQ1, 0x55);
	if (ret)
		return ret;
	ret = gpiod_direction_output(epd->vcom, 1);
	if (ret)
		return ret;
	ret = gpiod_direction_output(epd->pwrup, 1);
	if (ret)
		return ret;
	ret = epd_wait_pg(epd, true);
	if (ret)
		return ret;
	ret = epd_tps_write(epd, TPS_ENABLE, 0x3f);
	if (ret)
		return ret;
	ret = epd_tps_write(epd, TPS_VCOM2, (epd->vcom_mv / 10) >> 8);
	if (ret)
		return ret;
	ret = epd_tps_write(epd, TPS_VCOM1, epd->vcom_mv / 10);
	if (ret)
		return ret;
	deadline = jiffies + msecs_to_jiffies(250);
	do {
		value = epd_tps_read(epd, TPS_PG);
		if (value < 0)
			return value;
		if ((value & 0xfa) == 0xfa) {
			gpiod_set_value(epd->oe, 1);
			return 0;
		}
		usleep_range(1000, 2000);
	} while (time_before(jiffies, deadline));
	return -ETIMEDOUT;
}

static void epd_descriptors(struct metalio_epd *epd)
{
	unsigned int i, offset = 0;

	for (i = 0; i < EPD_DESC_COUNT; ++i) {
		u32 length = min_t(u32, EPD_DESC_CHUNK, METALIO_EPD_PHASE_BYTES - offset);
		bool last = i == EPD_DESC_COUNT - 1;

		epd->descriptors[i].control = cpu_to_le32(length | (length << 12) |
			EPD_DESC_OWNER | (last ? EPD_DESC_EOF : 0));
		epd->descriptors[i].buffer = cpu_to_le32(epd->phase_dma + offset);
		epd->descriptors[i].next = cpu_to_le32(last ? 0 :
			epd->descriptors_dma + (i + 1) * sizeof(struct epd_descriptor));
		epd->descriptors[i].padding = 0;
		offset += length;
	}
	/* CPU stores must reach coherent memory before the DMA fetches ownership. */
	dma_wmb();
}

static int epd_scan(struct metalio_epd *epd)
{
	unsigned long flags;
	u32 value;
	int ret, error;

	if (READ_ONCE(epd->abort))
		return -ECANCELED;
	ret = gpiod_get_value_cansleep(epd->powergood);
	if (ret <= 0)
		return ret < 0 ? ret : -EIO;
	epd_stop_scan(epd);
	synchronize_irq(epd->lcd_irq);
	synchronize_irq(epd->dma_irq);
	ret = readl_poll_timeout(epd->dma + EPD_DMA_RESET_AVAIL, value,
				 value & BIT(0), 1, 1000);
	if (ret)
		return ret;
	reinit_completion(&epd->scan_done);
	WRITE_ONCE(epd->scan_error, 0);
	WRITE_ONCE(epd->dma_done, false);
	writel(EPD_DMA_RESET, epd->dma + EPD_DMA_CONF0);
	writel(BIT(2) | BIT(3) | (3U << 5) | BIT(10), epd->dma + EPD_DMA_CONF0);
	writel(BIT(12), epd->dma + EPD_DMA_CONF1);
	writel(EPD_LCD_MISC_CONFIGURATION | EPD_LCD_FIFO_RESET, epd->lcd + EPD_LCD_MISC);
	writel(EPD_LCD_EVENTS, epd->lcd + EPD_LCD_INT_CLR);
	writel(0x43f, epd->dma + EPD_DMA_CLR);
	writel(EPD_RMT_DONE | EPD_RMT_LOOP_DONE | EPD_RMT_ERROR, epd->rmt + EPD_RMT_CLR);
	epd_descriptors(epd);
	writel(epd->descriptors_dma, epd->dma + EPD_DMA_LINK2);
	writel(EPD_DMA_EVENTS, epd->dma + EPD_DMA_ENA);
	writel(EPD_DMA_START, epd->dma + EPD_DMA_LINK1);
	writel(((METALIO_EPD_PHASE_ROWS + 5U) << 9) | EPD_RMT_LOOP_EN |
	       EPD_RMT_AUTOSTOP | EPD_RMT_LOOP_RESET, epd->rmt + EPD_RMT_LIMIT);
	writel(((METALIO_EPD_PHASE_ROWS + 5U) << 9) | EPD_RMT_LOOP_EN |
	       EPD_RMT_AUTOSTOP, epd->rmt + EPD_RMT_LIMIT);
	writel(EPD_RMT_CONFIGURATION | EPD_RMT_RD_RESET | EPD_RMT_APB_RESET,
	       epd->rmt + EPD_RMT_CONF);
	writel(EPD_RMT_CONFIGURATION, epd->rmt + EPD_RMT_CONF);
	gpiod_set_value(epd->mode, 1);
	/* Keep the reference STV/CKV launch relationship independent of scheduling. */
	spin_lock_irqsave(&epd->scan_lock, flags);
	gpiod_set_value(epd->stv, 0);
	writel(EPD_RMT_CONFIGURATION | EPD_RMT_UPDATE, epd->rmt + EPD_RMT_CONF);
	writel(EPD_RMT_CONFIGURATION | EPD_RMT_START, epd->rmt + EPD_RMT_CONF);
	udelay(EPD_LINE_US);
	gpiod_set_value(epd->stv, 1);
	udelay(EPD_LINE_US + EPD_CKV_LAUNCH_US);
	writel(EPD_LCD_EVENTS, epd->lcd + EPD_LCD_INT_ENA);
	writel(EPD_LCD_USER_CONFIGURATION | EPD_LCD_UPDATE, epd->lcd + EPD_LCD_USER);
	writel(EPD_LCD_USER_CONFIGURATION | EPD_LCD_START, epd->lcd + EPD_LCD_USER);
	spin_unlock_irqrestore(&epd->scan_lock, flags);

	ret = wait_for_completion_timeout(&epd->scan_done,
					msecs_to_jiffies(EPD_SCAN_TIMEOUT_MS)) ? 0 : -ETIMEDOUT;
	if (!ret)
		ret = READ_ONCE(epd->scan_error);
	if (!ret && !READ_ONCE(epd->dma_done) &&
	    !(readl(epd->dma + EPD_DMA_RAW) & EPD_DMA_EOF))
		ret = -EIO;
	if (!ret)
		ret = readl_poll_timeout(epd->rmt + EPD_RMT_RAW, value,
			value & (EPD_RMT_DONE | EPD_RMT_LOOP_DONE | EPD_RMT_ERROR), 1, 2000);
	if (!ret && (value & EPD_RMT_ERROR))
		ret = -EIO;
	epd_stop_scan(epd);
	synchronize_irq(epd->lcd_irq);
	synchronize_irq(epd->dma_irq);
	error = readl_poll_timeout(epd->dma + EPD_DMA_LINK1, value,
				   value & EPD_DMA_PARK, 1, 1000);
	if (!ret)
		ret = error;
	error = readl_poll_timeout(epd->dma + EPD_DMA_RESET_AVAIL, value,
				   value & BIT(0), 1, 1000);
	if (!ret)
		ret = error;
	if (!ret)
		ret = READ_ONCE(epd->scan_error);
	if (READ_ONCE(epd->abort))
		ret = -ECANCELED;
	gpiod_set_value(epd->mode, 0);
	return ret;
}

static void epd_phase_pause(ktime_t started, unsigned int minimum_us)
{
	s64 remaining = minimum_us - ktime_us_delta(ktime_get(), started);

	if (remaining > 0)
		usleep_range(remaining, remaining + 100);
}

static int epd_clear_panel(struct metalio_epd *epd)
{
	unsigned int cycle, step, repeat;
	int ret;

	/* Reference epd_clear_area_cycles: 3 * (10 dark + 10 white + 3 no-op). */
	for (cycle = 0; cycle < 3; ++cycle)
		for (step = 0; step < 3; ++step) {
			ret = metalio_epd_encode_uniform_phase(epd->phase,
				METALIO_EPD_PHASE_BYTES, step == 0 ? 1 : step == 1 ? 2 : 0);
			if (ret)
				return ret;
			for (repeat = 0; repeat < (step == 2 ? 3 : 10); ++repeat) {
				ktime_t started = ktime_get();

				ret = epd_scan(epd);
				if (ret)
					return ret;
				epd_phase_pause(started, 12000);
			}
		}
	memset(epd->previous, 0, METALIO_EPD_FRAME_BYTES);
	return 0;
}

static void epd_refresh_work(struct work_struct *work)
{
	struct metalio_epd *epd = container_of(work, struct metalio_epd, work);
	ktime_t started = ktime_get();
	unsigned int phase, mode = epd->known ? epd->refresh_mode : METALIO_EPD_GC;
	int ret, error;

	/* Reinitialize the engines so a previous aborted scan cannot poison a retry. */
	ret = epd_hw_init(epd);
	if (!ret)
		ret = epd_poweron(epd);
	if (!ret && !epd->known)
		ret = epd_clear_panel(epd);
	for (phase = 0; !ret && phase < metalio_epd_phase_count(mode); ++phase) {
		ktime_t phase_start = ktime_get();

		if (metalio_epd_phase_needs_encoding(mode, phase))
			ret = metalio_epd_encode_phase(epd->phase, METALIO_EPD_PHASE_BYTES,
					epd->previous, epd->pending, mode, phase, 0);
		if (!ret)
			ret = epd_scan(epd);
		if (!ret)
			epd_phase_pause(phase_start, metalio_epd_phase_time_us(mode, phase));
	}
	error = epd_poweroff(epd);
	if (!ret)
		ret = error;
	mutex_lock(&epd->lock);
	if (!ret && !epd->abort) {
		memcpy(epd->previous, epd->pending, METALIO_EPD_FRAME_BYTES);
		epd->known = true;
	} else {
		epd->known = false;
		if (!ret)
			ret = -ECANCELED;
	}
	epd->status.last_error = ret;
	epd->status.elapsed_ms = ktime_ms_delta(ktime_get(), started);
	epd->status.sequence++;
	epd->status.busy = 0;
	epd->completion_pending = true;
	mutex_unlock(&epd->lock);
	wake_up_interruptible(&epd->wait);
	if (ret)
		dev_err(epd->dev, "refresh failed: %d; next update will clear the panel\n", ret);
}

static int epd_open(struct inode *inode, struct file *file)
{
	struct metalio_epd *epd = container_of(file->private_data, struct metalio_epd, misc);
	int ret = 0;

	mutex_lock(&epd->lock);
	if (epd->removed)
		ret = -ENODEV;
	else if (epd->opened)
		ret = -EBUSY;
	else {
		epd->opened = true;
		epd->abort = false;
		epd->frame_valid = false;
		kref_get(&epd->ref);
		file->private_data = epd;
	}
	mutex_unlock(&epd->lock);
	return ret;
}

static int epd_release(struct inode *inode, struct file *file)
{
	struct metalio_epd *epd = file->private_data;

	WRITE_ONCE(epd->abort, true);
	cancel_work_sync(&epd->work);
	mutex_lock(&epd->lock);
	if (!epd->removed && epd->status.busy) {
		epd_poweroff(epd);
		epd->status.busy = 0;
		epd->known = false;
	}
	epd->opened = false;
	mutex_unlock(&epd->lock);
	kref_put(&epd->ref, epd_destroy);
	return 0;
}

static ssize_t epd_write(struct file *file, const char __user *buffer,
			 size_t length, loff_t *position)
{
	struct metalio_epd *epd = file->private_data;
	int ret;

	if (length != METALIO_EPD_FRAME_BYTES)
		return -EINVAL;
	mutex_lock(&epd->lock);
	if (epd->removed)
		ret = -ENODEV;
	else if (epd->status.busy)
		ret = -EBUSY;
	else {
		epd->frame_valid = false;
		ret = copy_from_user(epd->pending, buffer, length) ? -EFAULT : length;
		if (ret > 0)
			epd->frame_valid = true;
	}
	mutex_unlock(&epd->lock);
	return ret;
}

static long epd_ioctl(struct file *file, unsigned int command, unsigned long argument)
{
	struct metalio_epd *epd = file->private_data;
	void __user *pointer = (void __user *)argument;
	struct metalio_epd_refresh request;
	const struct metalio_epd_info info = {
		.abi_version = METALIO_EPD_ABI_VERSION, .width = METALIO_EPD_WIDTH,
		.height = METALIO_EPD_HEIGHT, .stride = METALIO_EPD_STRIDE,
		.frame_bytes = METALIO_EPD_FRAME_BYTES,
		.supported_modes = metalio_epd_supported_modes(),
	};
	int ret = 0;

	mutex_lock(&epd->lock);
	if (epd->removed) {
		ret = -ENODEV;
		goto out;
	}
	switch (command) {
	case METALIO_EPD_GET_INFO:
		ret = copy_to_user(pointer, &info, sizeof(info)) ? -EFAULT : 0;
		break;
	case METALIO_EPD_GET_STATUS:
		ret = copy_to_user(pointer, &epd->status, sizeof(epd->status)) ? -EFAULT : 0;
		if (!ret)
			epd->completion_pending = false;
		break;
	case METALIO_EPD_REFRESH:
		if (copy_from_user(&request, pointer, sizeof(request))) {
			ret = -EFAULT;
			break;
		}
		if (!metalio_epd_phase_count(request.mode) || request.reserved[0] ||
		    request.reserved[1] || request.reserved[2])
			ret = -EINVAL;
		else if (epd->status.busy)
			ret = -EBUSY;
		else if (!epd->frame_valid)
			ret = -ENODATA;
		else {
			epd->refresh_mode = request.mode;
			epd->status.busy = 1;
			epd->completion_pending = false;
			schedule_work(&epd->work);
		}
		break;
	default:
		ret = -ENOTTY;
	}
out:
	mutex_unlock(&epd->lock);
	return ret;
}

static __poll_t epd_poll(struct file *file, poll_table *wait)
{
	struct metalio_epd *epd = file->private_data;
	__poll_t events = 0;

	poll_wait(file, &epd->wait, wait);
	mutex_lock(&epd->lock);
	if (epd->removed)
		events = EPOLLERR | EPOLLHUP;
	else if (epd->completion_pending)
		events = EPOLLIN | EPOLLRDNORM | (epd->status.last_error ? EPOLLERR : 0);
	mutex_unlock(&epd->lock);
	return events;
}

static const struct file_operations epd_fops = {
	.owner = THIS_MODULE,
	.open = epd_open,
	.release = epd_release,
	.write = epd_write,
	.unlocked_ioctl = epd_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.poll = epd_poll,
};

static int epd_route(struct metalio_epd *epd, unsigned int pin,
		     unsigned int function, unsigned int signal)
{
	int ret = regmap_update_bits(epd->iomux, pin * 4,
		EPD_IOMUX_FUNCTION | EPD_IOMUX_DRIVE | EPD_IOMUX_PULLS,
		FIELD_PREP(EPD_IOMUX_FUNCTION, function) | FIELD_PREP(EPD_IOMUX_DRIVE, 3));

	if (ret)
		return ret;
	return regmap_update_bits(epd->matrix, EPD_MATRIX_OUT(pin), EPD_MATRIX_MASK,
				  signal | EPD_MATRIX_OE);
}

static void epd_disable_clocks(void *data)
{
	struct metalio_epd *epd = data;

	/* Assert reset before coherent buffers can be released by devres. */
	regmap_update_bits(epd->clkrst, EPD_CLK_DMA, BIT(1), BIT(1));
	regmap_update_bits(epd->clkrst, EPD_CLK_RMT, BIT(1), BIT(1));
	regmap_update_bits(epd->clkrst, EPD_CLK_LCD_BUS, BIT(1) | BIT(3), BIT(1) | BIT(3));
	regmap_update_bits(epd->clkrst, EPD_CLK_LCD, BIT(2), 0);
	regmap_update_bits(epd->clkrst, EPD_CLK_LCDCAM, BIT(2), 0);
	regmap_update_bits(epd->clkrst, EPD_CLK_LCD_BUS, BIT(0) | BIT(2), 0);
	regmap_update_bits(epd->clkrst, EPD_CLK_RMT, BIT(0) | BIT(5), 0);
	regmap_update_bits(epd->clkrst, EPD_CLK_DMA, BIT(0), 0);
}

static void epd_release_bus(void *data)
{
	struct metalio_epd *epd = data;
	static const unsigned int pins[] = { 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 4, 40 };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pins); ++i) {
		epd_route(epd, pins[i], 1, EPD_SIGNAL_GPIO);
		gpiod_set_value_cansleep(epd->bus->desc[i], 0);
	}
}

static int epd_hw_init(struct metalio_epd *epd)
{
	unsigned int pin;
	int ret;
	const struct { u32 reg, mask, value; } clocks[] = {
		{ EPD_CLK_DMA, BIT(0) | BIT(1), BIT(0) | BIT(1) },
		{ EPD_CLK_DMA, BIT(1), 0 },
		{ EPD_CLK_RMT, GENMASK(29, 0), BIT(0) | BIT(1) | (2U << 3) | BIT(5) | (1U << 22) },
		{ EPD_CLK_RMT, BIT(1), 0 },
		{ EPD_CLK_LCD_BUS, GENMASK(3, 0), GENMASK(3, 0) },
		{ EPD_CLK_LCD_BUS, BIT(1) | BIT(3), 0 },
		{ EPD_CLK_LCDCAM, BIT(2), BIT(2) },
		/* Match IDF's stable prescale=2 path: PLL160M / 4 / 2 = 20 MHz. */
		{ EPD_CLK_LCD, GENMASK(26, 0), 1U | BIT(2) | (3U << 3) },
	};

	for (pin = 0; pin < ARRAY_SIZE(clocks); ++pin) {
		ret = regmap_update_bits(epd->clkrst, clocks[pin].reg,
					 clocks[pin].mask, clocks[pin].value);
		if (ret)
			return ret;
	}
	ret = regmap_update_bits(epd->system, EPD_MEM_LCD, GENMASK(3, 0), BIT(3));
	if (ret)
		return ret;
	ret = regmap_update_bits(epd->system, EPD_MEM_RMT, GENMASK(3, 0), BIT(3));
	if (ret)
		return ret;
	writel(EPD_LCD_RESET, epd->lcd + EPD_LCD_USER);
	writel(BIT(31) | (2U << 29) | (1U << 9) | 1U,
	       epd->lcd + EPD_LCD_CLOCK);
	writel(EPD_LCD_USER_CONFIGURATION, epd->lcd + EPD_LCD_USER);
	writel(EPD_LCD_MISC_CONFIGURATION | EPD_LCD_FIFO_RESET, epd->lcd + EPD_LCD_MISC);
	writel(BIT(0), epd->lcd + EPD_LCD_BUFFER);
	/* hsync=8, porch=4, data=305 including dummy; total includes the tail. */
	writel((METALIO_EPD_PHASE_STRIDE - 1) | ((EPD_LINE_CLOCKS - 1) << 16),
	       epd->lcd + EPD_LCD_HORIZONTAL);
	writel((METALIO_EPD_PHASE_ROWS - 1) | ((METALIO_EPD_PHASE_ROWS + 1) << 16),
	       epd->lcd + EPD_LCD_VERTICAL);
	writel(11, epd->lcd + EPD_LCD_BLANK);
	writel(BIT(10) | BIT(11) | (7U << 16) | (1U << 24), epd->lcd + EPD_LCD_RGB);
	writel(BIT(4), epd->dma + EPD_DMA_MISC);
	writel(0x2f000000, epd->dma + EPD_DMA_SRAM_START);
	writel(0x2f07ffff, epd->dma + EPD_DMA_SRAM_END);
	writel(0x40000000, epd->dma + EPD_DMA_EXT_START);
	writel(0x53ffffff, epd->dma + EPD_DMA_EXT_END);
	writel(0, epd->dma + EPD_DMA_PERIPH);
	writel(5, epd->dma + EPD_DMA_PRI);
	writel(BIT(0), epd->rmt + EPD_RMT_SYS);
	writel(EPD_RMT_CONFIGURATION, epd->rmt + EPD_RMT_CONF);
	writel(EPD_CKV_HIGH_TICKS | BIT(15) | (EPD_CKV_LOW_TICKS << 16), epd->rmtmem);
	writel(0, epd->rmtmem + 4);
	epd_stop_scan(epd);
	for (pin = 8; pin <= 15; ++pin) {
		ret = epd_route(epd, pin, 3, EPD_SIGNAL_GPIO);
		if (ret)
			return ret;
	}
	ret = epd_route(epd, 16, 1, EPD_SIGNAL_PCLK);
	if (ret)
		return ret;
	ret = epd_route(epd, 17, 1, EPD_SIGNAL_HSYNC);
	if (ret)
		return ret;
	ret = epd_route(epd, 4, 1, EPD_SIGNAL_DE);
	if (ret)
		return ret;
	return epd_route(epd, 40, 1, EPD_SIGNAL_RMT0);
}

static void epd_unregister_tps(void *data)
{
	i2c_unregister_device(data);
}

static int epd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct metalio_epd *epd;
	struct device_node *bus_node;
	struct i2c_adapter *adapter;
	int ret;

	static_assert(sizeof(struct epd_descriptor) == 16);
	static_assert(EPD_DMA_BYTES <= 256 * 1024);
	static_assert(EPD_DESC_BYTES <= PAGE_SIZE);
	static_assert(METALIO_EPD_PHASE_ROWS + 5 < 1024);
	epd = kzalloc(sizeof(*epd), GFP_KERNEL);
	if (!epd)
		return -ENOMEM;
	epd->dev = dev;
	kref_init(&epd->ref);
	/* Added first, released last: every IRQ and cleanup action needs epd. */
	ret = devm_add_action_or_reset(dev, epd_put, epd);
	if (ret)
		return ret;
	mutex_init(&epd->lock);
	spin_lock_init(&epd->scan_lock);
	init_completion(&epd->scan_done);
	init_waitqueue_head(&epd->wait);
	INIT_WORK(&epd->work, epd_refresh_work);
	platform_set_drvdata(pdev, epd);
#define EPD_MAP(member, name) do { \
	epd->member = devm_platform_ioremap_resource_byname(pdev, name); \
	if (IS_ERR(epd->member)) { ret = PTR_ERR(epd->member); goto fail; } \
} while (0)
	EPD_MAP(lcd, "lcd-cam");
	EPD_MAP(dma, "axi-dma");
	EPD_MAP(rmt, "rmt");
	EPD_MAP(rmtmem, "rmtmem");
#undef EPD_MAP
#define EPD_SYSCON(member, name) do { \
	struct device_node *node = of_parse_phandle(dev->of_node, name, 0); \
	if (!node) { ret = -EINVAL; goto fail; } \
	epd->member = device_node_to_regmap(node); \
	of_node_put(node); \
	if (IS_ERR(epd->member)) { ret = PTR_ERR(epd->member); goto fail; } \
} while (0)
	EPD_SYSCON(clkrst, "hp-clkrst");
	EPD_SYSCON(system, "hp-system");
	EPD_SYSCON(matrix, "gpio-matrix");
	EPD_SYSCON(iomux, "iomux");
#undef EPD_SYSCON
#define EPD_GPIO(member, name, flags) do { \
	epd->member = devm_gpiod_get(dev, name, flags); \
	if (IS_ERR(epd->member)) { ret = PTR_ERR(epd->member); goto fail; } \
} while (0)
	EPD_GPIO(oe, "oe", GPIOD_OUT_LOW);
	EPD_GPIO(mode, "mode", GPIOD_OUT_LOW);
	EPD_GPIO(border, "border", GPIOD_OUT_LOW);
	EPD_GPIO(stv, "stv", GPIOD_OUT_LOW);
	udelay(12);
	EPD_GPIO(vcom, "vcom", GPIOD_OUT_LOW);
	EPD_GPIO(pwrup, "pwrup", GPIOD_OUT_LOW);
	EPD_GPIO(wakeup, "wakeup", GPIOD_OUT_LOW);
	EPD_GPIO(powergood, "powergood", GPIOD_IN);
	EPD_GPIO(panel_power, "panel-power", GPIOD_OUT_HIGH);
#undef EPD_GPIO
	if (gpiod_cansleep(epd->oe) || gpiod_cansleep(epd->mode) ||
	    gpiod_cansleep(epd->stv)) {
		ret = -EINVAL;
		goto fail;
	}
	epd->bus = devm_gpiod_get_array(dev, "bus", GPIOD_OUT_LOW);
	if (IS_ERR(epd->bus)) {
		ret = PTR_ERR(epd->bus);
		goto fail;
	}
	if (epd->bus->ndescs != 12) {
		ret = -EINVAL;
		goto fail;
	}
	epd->vcom_mv = 1200;
	of_property_read_u32(dev->of_node, "vcom-millivolt", &epd->vcom_mv);
	if (!epd->vcom_mv || epd->vcom_mv > 5110 || epd->vcom_mv % 10) {
		ret = -EINVAL;
		goto fail;
	}
	bus_node = of_parse_phandle(dev->of_node, "i2c-bus", 0);
	if (!bus_node) {
		ret = -EINVAL;
		goto fail;
	}
	adapter = of_find_i2c_adapter_by_node(bus_node);
	of_node_put(bus_node);
	if (!adapter) {
		ret = -EPROBE_DEFER;
		goto fail;
	}
	epd->tps = i2c_new_dummy_device(adapter, 0x68);
	i2c_put_adapter(adapter);
	if (IS_ERR(epd->tps)) {
		ret = PTR_ERR(epd->tps);
		goto fail;
	}
	ret = devm_add_action_or_reset(dev, epd_unregister_tps, epd->tps);
	if (ret)
		goto fail;
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		goto fail;
	/* One allocation fits the pool when rounded to a page order. */
	epd->phase = dmam_alloc_coherent(dev, EPD_DMA_BYTES,
						 &epd->phase_dma, GFP_KERNEL);
	if (!epd->phase) {
		ret = -ENOMEM;
		goto fail;
	}
	memset(epd->phase, 0, EPD_DMA_BYTES);
	epd->descriptors = (struct epd_descriptor *)(epd->phase + EPD_DESC_OFFSET);
	epd->descriptors_dma = epd->phase_dma + EPD_DESC_OFFSET;
	epd->pending = kvzalloc(METALIO_EPD_FRAME_BYTES, GFP_KERNEL);
	epd->previous = kvzalloc(METALIO_EPD_FRAME_BYTES, GFP_KERNEL);
	if (!epd->phase || !epd->pending || !epd->previous) {
		ret = -ENOMEM;
		goto fail;
	}
	ret = devm_add_action_or_reset(dev, epd_disable_clocks, epd);
	if (ret)
		goto fail;
	ret = devm_add_action_or_reset(dev, epd_release_bus, epd);
	if (ret)
		goto fail;
	ret = epd_hw_init(epd);
	if (ret)
		goto fail;
	epd->lcd_irq = platform_get_irq_byname(pdev, "lcd");
	epd->dma_irq = platform_get_irq_byname(pdev, "dma");
	if (epd->lcd_irq < 0 || epd->dma_irq < 0) {
		ret = epd->lcd_irq < 0 ? epd->lcd_irq : epd->dma_irq;
		goto fail;
	}
	ret = devm_request_irq(dev, epd->lcd_irq, epd_lcd_irq, 0, dev_name(dev), epd);
	if (ret)
		goto fail;
	ret = devm_request_irq(dev, epd->dma_irq, epd_dma_irq, 0, dev_name(dev), epd);
	if (ret)
		goto fail;
	epd->misc.minor = MISC_DYNAMIC_MINOR;
	epd->misc.name = "metalio-epd";
	epd->misc.fops = &epd_fops;
	epd->misc.parent = dev;
	epd->misc.mode = 0600;
	ret = misc_register(&epd->misc);
	if (ret)
		goto fail;
	dev_info(dev, "%ux%u, phase=%u bytes, DMA %pad, VCOM=-%u mV\n",
		 METALIO_EPD_WIDTH, METALIO_EPD_HEIGHT, METALIO_EPD_PHASE_BYTES,
		 &epd->phase_dma, epd->vcom_mv);
	dev_info(dev, "profile=%s, PCLK=%u MHz, line=%u us/%u clocks, CKV=%u/%u ticks, DU=%u GC=%u feedback=%u phases, min-phase=%u us\n",
		 IS_ENABLED(CONFIG_METALIO_EPD_FAST_REFRESH) ? "du14-follow8" : "legacy",
		 EPD_PCLK_MHZ, EPD_LINE_US, EPD_LINE_CLOCKS,
		 EPD_CKV_HIGH_TICKS, EPD_CKV_LOW_TICKS,
		 metalio_epd_phase_count(METALIO_EPD_DU),
		 metalio_epd_phase_count(METALIO_EPD_GC),
		 metalio_epd_phase_count(METALIO_EPD_FEEDBACK),
		 metalio_epd_phase_time_us(METALIO_EPD_DU, 0));
	return 0;
fail:
	return dev_err_probe(dev, ret, "cannot initialize EPD\n");
}

static void epd_remove(struct platform_device *pdev)
{
	struct metalio_epd *epd = platform_get_drvdata(pdev);

	misc_deregister(&epd->misc);
	mutex_lock(&epd->lock);
	epd->removed = true;
	WRITE_ONCE(epd->abort, true);
	mutex_unlock(&epd->lock);
	cancel_work_sync(&epd->work);
	epd_poweroff(epd);
	wake_up_interruptible(&epd->wait);
}

static void epd_shutdown(struct platform_device *pdev)
{
	struct metalio_epd *epd = platform_get_drvdata(pdev);

	mutex_lock(&epd->lock);
	epd->removed = true;
	WRITE_ONCE(epd->abort, true);
	mutex_unlock(&epd->lock);
	cancel_work_sync(&epd->work);
	epd_poweroff(epd);
	wake_up_interruptible(&epd->wait);
}

static const struct of_device_id epd_match[] = {
	{ .compatible = "metalio,eink4-plus-epd" },
	{ }
};
MODULE_DEVICE_TABLE(of, epd_match);

static struct platform_driver epd_driver = {
	.probe = epd_probe,
	.remove = epd_remove,
	.shutdown = epd_shutdown,
	.driver = { .name = "metalio-epd", .of_match_table = epd_match },
};
module_platform_driver(epd_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Metalio ESP32-S31 native parallel E0470A14-AF panel");
