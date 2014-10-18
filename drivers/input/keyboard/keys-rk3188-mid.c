#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/timer.h>
#include <linux/wait.h>
#include <linux/earlysuspend.h>
#include <linux/suspend.h>
#include <mach/iomux.h>
#include <mach/irqs.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/delay.h>


#define KEYS_I2C_ADDR			(0x8c >> 1)
#define KEYS_DATA_ADDR			0x55
#define I2C_IRQ_NUM				RK30_PIN0_PC6
#define I2C_IRQ_GPIO			GPIO0_C6
#define BUTTON_SHACK_MAX		2
#define REPORT_KEY_DELAY		msecs_to_jiffies(10)
#define JOY_EDGE_OFFSET			10
#define JOY_MID_OFFSET			18

#define BUTTON_BYTE_SIZE		15
#define JOYSTICK_BYTE_SIZE		4
#define BUTTON_NUM_MAX			BUTTON_BYTE_SIZE
#define JOYSTICK_NUM_MAX		JOYSTICK_BYTE_SIZE

#define ABS_L_X					ABS_X
#define ABS_L_Y					ABS_Y
#define ABS_R_X					ABS_Z
#define ABS_R_Y					ABS_RZ
#define DRV_NAME		"jxdkey_driver"

typedef struct __buttons_data
{
	u8 dataBuffer[BUTTON_BYTE_SIZE];
	u8 shackCount[BUTTON_BYTE_SIZE];
}st_buttons_data;

typedef struct __joystick_data
{
	u8 dataBuffer[JOYSTICK_BYTE_SIZE];
	u8 readBuffer[JOYSTICK_BYTE_SIZE];
}st_joystick_data;

typedef struct __rk3188_keys_data
{
	struct i2c_client *client;
	struct input_dev *input;
	struct work_struct i2c_read_work;
	struct delayed_work poll_key_work;
	struct early_suspend early_suspend;
	struct mutex my_mutex;
	int irq_no;

	st_buttons_data buttons_data;
	st_joystick_data joystick_data;
}st_rk3188_keys_data;

st_rk3188_keys_data *rk3188_keys_data;
//buttons sort by up, down, left, rigth, A, B, X, Y, L1, R1, L2, R2, SELECT, START, HOT
static unsigned buttons_gpio_mode[BUTTON_NUM_MAX] = {0, GPIO1_A6, GPIO1_A5, GPIO1_A4, GPIO1_B2, GPIO1_B3, GPIO1_B5, 
GPIO1_B4, GPIO0_C1, GPIO0_C5, GPIO0_C2, GPIO0_C3, GPIO0_C0, GPIO0_C4, GPIO0_C7};
static unsigned buttons_gpio_pin[BUTTON_NUM_MAX] = {RK30_PIN0_PB4, RK30_PIN1_PA6, RK30_PIN1_PA5, RK30_PIN1_PA4, RK30_PIN1_PB2, 
	RK30_PIN1_PB3, RK30_PIN1_PB5, RK30_PIN1_PB4, RK30_PIN0_PC1, RK30_PIN0_PC5, RK30_PIN0_PC2, RK30_PIN0_PC3, RK30_PIN0_PC0, 
	RK30_PIN0_PC4, RK30_PIN0_PC7};

static unsigned int key_report_value[BUTTON_NUM_MAX] = {KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, BTN_A, BTN_B, BTN_X, BTN_Y, 
	BTN_TL, BTN_TR2, BTN_TL2, BTN_TR, BTN_SELECT, BTN_START, 0x58};
static const unsigned int joy_report_value[JOYSTICK_NUM_MAX] = {ABS_L_Y, ABS_L_X, ABS_R_X, ABS_R_Y};


static ssize_t rk3188_keys_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "LX = %d, LY = %d, RX = %d, RY = %d\n", rk3188_keys_data->joystick_data.dataBuffer[1], 
		rk3188_keys_data->joystick_data.dataBuffer[0], rk3188_keys_data->joystick_data.dataBuffer[2], 
		rk3188_keys_data->joystick_data.dataBuffer[3]);
}

static ssize_t rk3188_keys_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	return 0;
}

static struct kobj_attribute keys_attribute = __ATTR(rk3188_keys, 0666, rk3188_keys_show, rk3188_keys_store);
static const struct attribute *rk3188_keys_attribute[] = {
	&keys_attribute.attr,
	NULL,
	};


static int i2c_irq_err_flag = true;
static int i2c_poll_delay = 0;
static int i2c_poll_tryCnt = 0;

static void poll_key_func(struct work_struct *work)
{
	int i = 0, read_value = 0;

	for (i = 0; i < BUTTON_NUM_MAX; i++)
	{
		read_value = gpio_get_value(buttons_gpio_pin[i]);
		read_value = ~read_value & 0x01;

		if (read_value != rk3188_keys_data->buttons_data.dataBuffer[i])
		{
			rk3188_keys_data->buttons_data.shackCount[i]--;
			if (rk3188_keys_data->buttons_data.shackCount[i] == 0)
			{
				rk3188_keys_data->buttons_data.shackCount[i] = BUTTON_SHACK_MAX;
				rk3188_keys_data->buttons_data.dataBuffer[i] = read_value;
				input_report_key(rk3188_keys_data->input, key_report_value[i], rk3188_keys_data->buttons_data.dataBuffer[i]);
			}
		}
		else
		{
			rk3188_keys_data->buttons_data.shackCount[i] = BUTTON_SHACK_MAX;
		}
	}
	
	mutex_lock(&rk3188_keys_data->my_mutex);
	for (i = 0; i < JOYSTICK_BYTE_SIZE; i++)
	{

		rk3188_keys_data->joystick_data.dataBuffer[i] = rk3188_keys_data->joystick_data.readBuffer[i];
		
		input_report_abs(rk3188_keys_data->input,joy_report_value[i], rk3188_keys_data->joystick_data.dataBuffer[i]);

	}
	
	input_sync(rk3188_keys_data->input);
	mutex_unlock(&rk3188_keys_data->my_mutex);

	if (i2c_irq_err_flag == true)
	{
		i2c_poll_delay++;
		if (i2c_poll_delay > 10)
		{
			i2c_poll_delay = 0;
			if (i2c_poll_tryCnt < 10)
			{
				i2c_poll_tryCnt++;
		//		printk(KERN_NOTICE "##########################################\n");
				schedule_work(&rk3188_keys_data->i2c_read_work);
			}
		}
	}
	else
	{
		i2c_poll_delay = 0;
		i2c_poll_tryCnt = 0;
	}

	schedule_delayed_work(&rk3188_keys_data->poll_key_work, REPORT_KEY_DELAY);
}

static void i2c_read_func(struct work_struct *work)
{
	struct i2c_msg msgs[2];
	u8 authbuff[1] = {0x55};
	int i = 0;
	int temp_value = 0;
	
	u8 i2cReadBuffer[JOYSTICK_BYTE_SIZE] = {0};
	
	memset(msgs, 0, sizeof(msgs));
	msgs[0].addr = rk3188_keys_data->client->addr;
	msgs[0].flags = rk3188_keys_data->client->flags;
	msgs[0].buf = authbuff;
	msgs[0].len = 1;
	msgs[0].scl_rate = 100000;
	msgs[0].udelay = rk3188_keys_data->client->udelay;

	msgs[1].addr = rk3188_keys_data->client->addr;
	msgs[1].flags = rk3188_keys_data->client->flags | I2C_M_RD;
	msgs[1].buf = i2cReadBuffer;
	msgs[1].len = JOYSTICK_BYTE_SIZE;
	msgs[1].scl_rate = 100000;
	msgs[1].udelay = rk3188_keys_data->client->udelay;

	if (i2c_transfer(rk3188_keys_data->client->adapter, msgs, 2) > 0)
	{
	
		i2cReadBuffer[3] = 255 - i2cReadBuffer[3];
		for (i = 0; i < JOYSTICK_BYTE_SIZE; i++)
		{
			if (i2cReadBuffer[i] < JOY_EDGE_OFFSET)
			{
				i2cReadBuffer[i] = 0x00;
			}
			else if ((i2cReadBuffer[i] >= JOY_EDGE_OFFSET) && (i2cReadBuffer[i] < (128 - JOY_MID_OFFSET)))
			{
				temp_value = i2cReadBuffer[i];
				temp_value = 128 * (temp_value - JOY_EDGE_OFFSET) / (128 - JOY_EDGE_OFFSET - JOY_MID_OFFSET);
				i2cReadBuffer[i] = temp_value;
			}
			else if ((i2cReadBuffer[i] >= (128 - JOY_MID_OFFSET)) && (i2cReadBuffer[i] < (128 + JOY_MID_OFFSET)))
			{
				i2cReadBuffer[i] = 128;
			}
			else if ((i2cReadBuffer[i] >= (128 + JOY_MID_OFFSET)) && (i2cReadBuffer[i] < (255 - JOY_EDGE_OFFSET)))
			{
				temp_value = i2cReadBuffer[i];
				temp_value = 128 * (temp_value - 128 - JOY_MID_OFFSET) / (128 - JOY_EDGE_OFFSET - JOY_MID_OFFSET) + 128;
				i2cReadBuffer[i] = temp_value;
			}
			else if (i2cReadBuffer[i] >= (255 - JOY_EDGE_OFFSET))
			{
				i2cReadBuffer[i] = 255;
			}
		}
		
		mutex_lock(&rk3188_keys_data->my_mutex);
		memcpy(rk3188_keys_data->joystick_data.readBuffer, i2cReadBuffer, JOYSTICK_BYTE_SIZE);
		mutex_unlock(&rk3188_keys_data->my_mutex);
	}
	else
	{
		i2c_irq_err_flag = true;
	}
}

static irqreturn_t buttons_irq(int irq, void *data)
{
	i2c_irq_err_flag = false;
	schedule_work(&rk3188_keys_data->i2c_read_work);
	return IRQ_HANDLED;
}

static void rk3188_keys_early_suspend(struct early_suspend *h)
{
	disable_irq(rk3188_keys_data->irq_no);
	__cancel_delayed_work(&rk3188_keys_data->poll_key_work);
}

static void rk3188_keys_late_resume(struct early_suspend *h)
{
	enable_irq(rk3188_keys_data->irq_no);
	schedule_delayed_work(&rk3188_keys_data->poll_key_work, REPORT_KEY_DELAY);
	i2c_irq_err_flag = true;
	i2c_poll_delay = 0;
	i2c_poll_tryCnt = 0;
}

static int rk3188_keys_suspend(struct i2c_client *client, pm_message_t mesg)
{
	return 0;
}

static int rk3188_keys_resume(struct i2c_client *client)
{
	return 0;
}

static int rk3188_keys_detect(struct i2c_client *client, struct i2c_board_info *info)
{
	printk(KERN_NOTICE "rk3188_keys_detect : addr = 0x%x\n", client->addr);
	
	strlcpy(info->type, "rk3188_keys", I2C_NAME_SIZE);
	return 0;
}

static int rk3188_keys_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int error = 0, i = 0;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
	{
		printk(KERN_ERR "rk3188_keys_probe: need I2C_FUNC_I2C\n");
		error = -ENODEV;
		goto err_exit;
	}

	rk3188_keys_data = kzalloc(sizeof(st_rk3188_keys_data), GFP_KERNEL);
	if (!rk3188_keys_data)
	{
		printk(KERN_ERR "rk3188_keys_probe: Fail to kzalloc rk3188_keys_data\n");
		error = -ENOMEM;
		goto err_exit;
	}

	rk3188_keys_data->input = input_allocate_device();
	if (!rk3188_keys_data->input)
	{
		printk(KERN_ERR "rk3188_keys_probe: Fail to allocate input device\n");
		error = -ENOMEM;
		goto err_free_mem;
	}

	set_bit(EV_KEY, rk3188_keys_data->input->evbit);
	set_bit(EV_ABS, rk3188_keys_data->input->evbit);
	set_bit(EV_REL, rk3188_keys_data->input->evbit);

	for (i = 0; i < BUTTON_NUM_MAX; i++)
	{
		if (buttons_gpio_mode[i] != 0)
			iomux_set(buttons_gpio_mode[i]);
		set_bit(key_report_value[i], rk3188_keys_data->input->keybit);
	}
	set_bit(BTN_JOYSTICK, rk3188_keys_data->input->keybit);

	for (i = 0; i < JOYSTICK_NUM_MAX; i++)
	{
		input_set_abs_params(rk3188_keys_data->input, joy_report_value[i], 0, 255, 0, 0);
	}

	rk3188_keys_data->input->name = DRV_NAME;

	if (input_register_device(rk3188_keys_data->input))
	{
		printk(KERN_ERR "rk3188_keys_probe: Fail to register input device\n");
		error = -ENXIO;
		goto err_free_input;
	}

	if (sysfs_create_files(&rk3188_keys_data->input->dev.kobj, rk3188_keys_attribute))
	{
		printk(KERN_ERR "msp430_i2c_probe: Fail to create device attribute\n");
		error = -ENXIO;
		goto err_unregister_device;
	}

	rk3188_keys_data->client = client;
	mutex_init(&rk3188_keys_data->my_mutex);
	memset(&rk3188_keys_data->buttons_data, 0x00, sizeof(st_buttons_data));
	memset(&rk3188_keys_data->joystick_data, 0x80, sizeof(st_joystick_data));
	mutex_init(&rk3188_keys_data->my_mutex);
	INIT_WORK(&rk3188_keys_data->i2c_read_work, i2c_read_func);
	INIT_DELAYED_WORK(&rk3188_keys_data->poll_key_work, poll_key_func);
	schedule_delayed_work(&rk3188_keys_data->poll_key_work, REPORT_KEY_DELAY);

	iomux_set(I2C_IRQ_GPIO);
	gpio_direction_input(I2C_IRQ_NUM);
	gpio_pull_updown(I2C_IRQ_NUM, 1);
	rk3188_keys_data->irq_no = gpio_to_irq(I2C_IRQ_NUM);
	if (request_irq(rk3188_keys_data->irq_no, buttons_irq, IRQF_TRIGGER_FALLING, 
		rk3188_keys_data->input->name, (void *)rk3188_keys_data) < 0)
//	if (devm_request_irq(&rk3188_keys_data->input->dev, rk3188_keys_data->irq_no, buttons_irq, 
//		IRQF_TRIGGER_FALLING, rk3188_keys_data->input->name, (void *)rk3188_keys_data))
	{
		printk(KERN_ERR "rk3188_keys_probe: Fail to request irq\n");
		error = -ENOMEM;
		goto err_unregister_device;
	}

	rk3188_keys_data->early_suspend.suspend = rk3188_keys_early_suspend;
	rk3188_keys_data->early_suspend.resume = rk3188_keys_late_resume;
	rk3188_keys_data->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN;
	register_early_suspend(&rk3188_keys_data->early_suspend);
	
	return 0;

err_unregister_device:
	input_unregister_device(rk3188_keys_data->input);
err_free_input:
	if (rk3188_keys_data->input != NULL)
	{
		input_free_device(rk3188_keys_data->input);
		rk3188_keys_data->input = NULL;
	}
err_free_mem:
	if (rk3188_keys_data != NULL)
	{
		kfree(rk3188_keys_data);
		rk3188_keys_data = NULL;
	}
err_exit:
	return error;

}

static int rk3188_keys_remove(struct i2c_client *client)
{
	unregister_early_suspend(&rk3188_keys_data->early_suspend);

//	devm_free_irq(&rk3188_keys_data->input->dev, rk3188_keys_data->irq_no, (void *)rk3188_keys_data);
	free_irq(rk3188_keys_data->irq_no, (void *)rk3188_keys_data);
	cancel_work_sync(&rk3188_keys_data->i2c_read_work);
	__cancel_delayed_work(&rk3188_keys_data->poll_key_work);

	sysfs_remove_files(&rk3188_keys_data->input->dev.kobj, rk3188_keys_attribute);
	input_unregister_device(rk3188_keys_data->input);
	if (rk3188_keys_data->input != NULL)
	{
		input_free_device(rk3188_keys_data->input);
		rk3188_keys_data->input = NULL;
	}
	
	if (rk3188_keys_data != NULL)
	{
		kfree(rk3188_keys_data);
		rk3188_keys_data = NULL;
	}
	return 0;
}

static const struct i2c_device_id rk3188_keys_id[] = {
	{"rk3188_keys", 0},
	{}
};
MODULE_DEVICE_TABLE(i2c, rk3188_keys_id);

static struct i2c_board_info rk3188_keys_info[] = {
	{
		I2C_BOARD_INFO("rk3188_keys", KEYS_I2C_ADDR),
	},
};

static const unsigned short normal_i2c[] = {KEYS_I2C_ADDR, I2C_CLIENT_END};

static struct i2c_driver rk3188_keys_driver = {
	.driver = {
		.name	= "rk3188_keys",
	},
	.class		= I2C_CLASS_HWMON,
	.probe		= rk3188_keys_probe,
	.remove		= __devexit_p(rk3188_keys_remove),
	.suspend	= rk3188_keys_suspend,
	.resume		= rk3188_keys_resume,
	.id_table	= rk3188_keys_id,
	.detect     = rk3188_keys_detect,
//	.address_list	= normal_i2c,
};

#if 0
static int __init rk3188_keys_init(void)
{
	i2c_add_driver(&rk3188_keys_driver);
	i2c_register_board_info(2, rk3188_keys_info, ARRAY_SIZE(rk3188_keys_info));

	return 0;
}

static void __exit rk3188_keys_exit(void)
{
	i2c_del_driver(&rk3188_keys_driver);
}
#else
static struct i2c_client *rk3188_keys_client;
static int __init rk3188_keys_init(void)
{
	struct i2c_adapter *rk3188_keys_adap;

	
	i2c_add_driver(&rk3188_keys_driver);
	rk3188_keys_adap = i2c_get_adapter(2);
	if (rk3188_keys_adap)
	{
		rk3188_keys_client = i2c_new_device(rk3188_keys_adap, rk3188_keys_info);
		i2c_put_adapter(rk3188_keys_adap);
	}
	else
	{
		return -ENODEV;
	}

	return 0;
}

static void __exit rk3188_keys_exit(void)
{
	if (rk3188_keys_client != NULL)
		i2c_unregister_device(rk3188_keys_client);
	i2c_del_driver(&rk3188_keys_driver);
}
#endif

module_init(rk3188_keys_init);
module_exit(rk3188_keys_exit);
MODULE_LICENSE("GPL");

