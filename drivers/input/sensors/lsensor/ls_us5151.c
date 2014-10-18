/* drivers/input/sensors/access/kxtik.c
 *
 * Copyright (C) 2012-2015 ROCKCHIP.
 * Author: luowei <lw@rock-chips.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/miscdevice.h>
#include <linux/gpio.h>
#include <asm/uaccess.h>
#include <asm/atomic.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/freezer.h>
#include <mach/gpio.h>
#include <mach/board.h> 
#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif
#include <linux/sensor-dev.h>

#if 0
#define SENSOR_DEBUG_TYPE SENSOR_TYPE_LIGHT
#define DBG(x...)  printk(x)
#else
#define DBG(x...)
#endif

#define CONFIG_REG		  (0x01)//(0x00)
#define ALS_DATA_H_REG   (0x02)
#define ALS_DATA_L_REG   (0x03)


#define ALS_SD_ENABLE 	  	  (1<<7)
#define ALS_SD_DISABLE	      (0<<7)


/****************operate according to sensor chip:start************/

static int sensor_active(struct i2c_client *client, int enable, int rate)
{
	struct sensor_private_data *sensor =
		(struct sensor_private_data *) i2c_get_clientdata(client);	
	int result = 0;
	char value = 0;
	value = sensor_read_reg(client, sensor->ops->ctrl_reg);
	
	//register setting according to chip datasheet		
	if(enable)
	{	
		value = ALS_SD_ENABLE;	
		sensor->ops->ctrl_data |= value;	
	}
	else
	{
		value = ALS_SD_DISABLE;
		sensor->ops->ctrl_data &= value;
	}

	
	sensor->ops->ctrl_data = value;
	
	DBG("%s:reg=0x%x,reg_ctrl=0x%x,enable=%d\n",__func__,sensor->ops->ctrl_reg, sensor->ops->ctrl_data, enable);
	result = sensor_write_reg(client, sensor->ops->ctrl_reg, sensor->ops->ctrl_data);
	if(result)
		printk("%s:fail to active sensor\n",__func__);

	return result;

}


static int sensor_init(struct i2c_client *client)
{	
	struct sensor_private_data *sensor =
		(struct sensor_private_data *) i2c_get_clientdata(client);	
	int result = 0;

	result = sensor->ops->active(client,0,0);
	if(result)
	{
		printk("%s:line=%d,error\n",__func__,__LINE__);
		return result;
	}

	return result;
}


static int light_report_value(struct input_dev *input, int data)
{
	int index = 0;
#if 1
	if(data <= 30){
		index = 0;goto report;
	}
	else if(data <= 100){
		index = 1;goto report;
	}
	else if(data <= 150){
		index = 2;goto report;
	}
	else if(data <= 220){
		index = 3;goto report;
	}
	else if(data <= 280){
		index = 4;goto report;
	}
	else if(data <= 350){
		index = 5;goto report;
	}
	else if(data <= 420){
		index = 6;goto report;
	}
	else{
		index = 7;goto report;
	}
#endif
report:
	input_report_abs(input, ABS_MISC, index);
	input_sync(input);

	return index;
}


static int sensor_report_value(struct i2c_client *client)
{
	struct sensor_private_data *sensor =
		(struct sensor_private_data *) i2c_get_clientdata(client);	
	int result = 0;
	int adc_value = 35541;
	char index = 0;
	char buffer[2] = {0x02,0x03};
	memset(buffer, 0, 2);
	buffer[0] = sensor->ops->read_reg;
	result = sensor_rx_data(client, buffer, sensor->ops->read_len); 

	adc_value = (buffer[1]>>7)+buffer[0]*2;
	
	index = light_report_value(sensor->input_dev, adc_value); 
	DBG("%s:%s result=%d %d %d,index=%d\n",__func__,sensor->ops->name, buffer[0],buffer[1],adc_value,index);
	
	return result;
}

struct sensor_operate light_us5151_ops = {
	.name				= "ls_us5151",
	.type				= SENSOR_TYPE_LIGHT,	//sensor type and it should be correct
	.id_i2c 			= LIGHT_ID_US5151,	//i2c id number
	.read_reg			= ALS_DATA_H_REG,	//read data
	.read_len			= 2,			//data length
	.id_reg 			= SENSOR_UNKNOW_DATA,	//read device id from this register
	.id_data			= SENSOR_UNKNOW_DATA,	//device id
	.precision			= 8,			//8 bits
	.ctrl_reg			= CONFIG_REG,		//enable or disable 
	.int_status_reg 		= SENSOR_UNKNOW_DATA,	//intterupt status register
	.range				= {100,65535},		//range
	.brightness 									   ={10,255},						   // brightness
	.trig				= IRQF_TRIGGER_LOW | IRQF_ONESHOT | IRQF_SHARED,		
	.active 			= sensor_active,	
	.init				= sensor_init,
	.report 			= sensor_report_value,
};

/****************operate according to sensor chip:end************/

//function name should not be changed
static struct sensor_operate *light_get_ops(void)
{
	return &light_us5151_ops;
}


static int __init light_us5151_init(void)
{
	struct sensor_operate *ops = light_get_ops();
	int result = 0;
	int type = ops->type;
	result = sensor_register_slave(type, NULL, NULL, light_get_ops);
	DBG("%s\n",__func__);
	return result;
}

static void __exit light_us5151_exit(void)
{
	struct sensor_operate *ops = light_get_ops();
	int type = ops->type;
	sensor_unregister_slave(type, NULL, NULL, light_get_ops);
}


module_init(light_us5151_init);
module_exit(light_us5151_exit);

