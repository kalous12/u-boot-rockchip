/*
 * Copyright (C) 2015 Samsung Electronics
 * Przemyslaw Marczak <p.marczak@samsung.com>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <dm.h>
#include <dm/lists.h>
#include <dm/device-internal.h>
#include <dm/uclass-internal.h>
#include <adc.h>
#include <power/regulator.h>

DECLARE_GLOBAL_DATA_PTR;

#define ADC_UCLASS_PLATDATA_SIZE	sizeof(struct adc_uclass_platdata)
#define CHECK_NUMBER			true
#define CHECK_MASK			(!CHECK_NUMBER)

/* TODO: add support for timer uclass (for early calls) */
#ifdef CONFIG_SANDBOX_ARCH
#define sdelay(x)	udelay(x)
#else
extern void sdelay(unsigned long loops);
#endif

static int check_channel(struct udevice *dev, int value, bool number_or_mask,
			 const char *caller_function)
{
	struct adc_uclass_platdata *uc_pdata = dev_get_uclass_platdata(dev);
	unsigned mask = number_or_mask ? (1 << value) : value;

	/* For the real ADC hardware, some ADC channels can be inactive.
	 * For example if device has 4 analog channels, and only channels
	 * 1-st and 3-rd are valid, then channel mask is: 0b1010, so request
	 * with mask 0b1110 should return an error.
	*/
	if ((uc_pdata->channel_mask >= mask) && (uc_pdata->channel_mask & mask))
		return 0;

	printf("Error in %s/%s().\nWrong channel selection for device: %s\n",
	       __FILE__, caller_function, dev->name);

	return -EINVAL;
}

#ifdef CONFIG_ADC_REQ_REGULATOR
static int adc_supply_enable(struct udevice *dev)
{
	struct adc_uclass_platdata *uc_pdata = dev_get_uclass_platdata(dev);
	const char *supply_type;
	int ret = 0;

	if (uc_pdata->vdd_supply) {
		supply_type = "vdd";
		ret = regulator_set_enable(uc_pdata->vdd_supply, true);
	}

	if (!ret && uc_pdata->vss_supply) {
		supply_type = "vss";
		ret = regulator_set_enable(uc_pdata->vss_supply, true);
	}

	if (ret)
		pr_err("%s: can't enable %s-supply!", dev->name, supply_type);

	return ret;
}
#else
static inline int adc_supply_enable(struct udevice *dev) { return 0; }
#endif

int adc_data_mask(struct udevice *dev, unsigned int *data_mask)
{
	struct adc_uclass_platdata *uc_pdata = dev_get_uclass_platdata(dev);

	if (!uc_pdata)
		return -ENOSYS;

	*data_mask = uc_pdata->data_mask;
	return 0;
}

int adc_stop(struct udevice *dev)
{
	const struct adc_ops *ops = dev_get_driver_ops(dev);

	if (!ops->stop)
		return -ENOSYS;

	return ops->stop(dev);
}

int adc_start_channel(struct udevice *dev, int channel)
{
	const struct adc_ops *ops = dev_get_driver_ops(dev);
	int ret;

	if (!ops->start_channel)
		return -ENOSYS;

	ret = check_channel(dev, channel, CHECK_NUMBER, __func__);
	if (ret)
		return ret;

	ret = adc_supply_enable(dev);
	if (ret)
		return ret;

	return ops->start_channel(dev, channel);
}

int adc_start_channels(struct udevice *dev, unsigned int channel_mask)
{
	const struct adc_ops *ops = dev_get_driver_ops(dev);
	int ret;

	if (!ops->start_channels)
		return -ENOSYS;

	ret = check_channel(dev, channel_mask, CHECK_MASK, __func__);
	if (ret)
		return ret;

	ret = adc_supply_enable(dev);
	if (ret)
		return ret;

	return ops->start_channels(dev, channel_mask);
}

int adc_channel_data(struct udevice *dev, int channel, unsigned int *data)
{
	struct adc_uclass_platdata *uc_pdata = dev_get_uclass_platdata(dev);
	const struct adc_ops *ops = dev_get_driver_ops(dev);
	unsigned int timeout_us = uc_pdata->data_timeout_us;
	int ret;

	if (!ops->channel_data)
		return -ENOSYS;

	ret = check_channel(dev, channel, CHECK_NUMBER, __func__);
	if (ret)
		return ret;

	do {
		ret = ops->channel_data(dev, channel, data);
		if (!ret || ret != -EBUSY)
			break;

		/* TODO: use timer uclass (for early calls). */
		sdelay(5);
	} while (timeout_us--);

	return ret;
}

int adc_channels_data(struct udevice *dev, unsigned int channel_mask,
		      struct adc_channel *channels)
{
	struct adc_uclass_platdata *uc_pdata = dev_get_uclass_platdata(dev);
	unsigned int timeout_us = uc_pdata->multidata_timeout_us;
	const struct adc_ops *ops = dev_get_driver_ops(dev);
	int ret;

	if (!ops->channels_data)
		return -ENOSYS;

	ret = check_channel(dev, channel_mask, CHECK_MASK, __func__);
	if (ret)
		return ret;

	do {
		ret = ops->channels_data(dev, channel_mask, channels);
		if (!ret || ret != -EBUSY)
			break;

		/* TODO: use timer uclass (for early calls). */
		sdelay(5);
	} while (timeout_us--);

	return ret;
}

int adc_channel_single_shot(const char *name, int channel, unsigned int *data)
{
	struct udevice *dev;
	int ret;

	ret = uclass_get_device_by_name(UCLASS_ADC, name, &dev);
	if (ret)
		return ret;

	ret = adc_start_channel(dev, channel);
	if (ret)
		return ret;

	ret = adc_channel_data(dev, channel, data);
	if (ret)
		return ret;

	return 0;
}

static int _adc_channels_single_shot(struct udevice *dev,
				     unsigned int channel_mask,
				     struct adc_channel *channels)
{
	unsigned int data;
	int channel, ret;

	for (channel = 0; channel <= ADC_MAX_CHANNEL; channel++) {
		/* Check channel bit. */
		if (!((channel_mask >> channel) & 0x1))
			continue;

		ret = adc_start_channel(dev, channel);
		if (ret)
			return ret;

		ret = adc_channel_data(dev, channel, &data);
		if (ret)
			return ret;

		channels->id = channel;
		channels->data = data;
		channels++;
	}

	return 0;
}

int adc_channels_single_shot(const char *name, unsigned int channel_mask,
			     struct adc_channel *channels)
{
	struct udevice *dev;
	int ret;

	ret = uclass_get_device_by_name(UCLASS_ADC, name, &dev);
	if (ret)
		return ret;

	ret = adc_start_channels(dev, channel_mask);
	if (ret)
		goto try_manual;

	ret = adc_channels_data(dev, channel_mask, channels);
	if (ret)
		return ret;

	return 0;

try_manual:
	if (ret != -ENOSYS)
		return ret;

	return _adc_channels_single_shot(dev, channel_mask, channels);
}

#ifdef CONFIG_ADC_REQ_REGULATOR
static int adc_vdd_platdata_update(struct udevice *dev)
{
	struct adc_uclass_platdata *uc_pdata = dev_get_uclass_platdata(dev);
	int ret;

	/* Warning!
	 * This function can't return supply device before its bind.
	 * Please pay attention to proper fdt scan sequence. If ADC device
	 * will bind before its supply regulator device, then the below 'get'
	 * will return an error.
	 */
	ret = device_get_supply_regulator(dev, "vdd-supply",
					  &uc_pdata->vdd_supply);
	if (ret)
		return ret;

	ret = regulator_get_value(uc_pdata->vdd_supply);
	if (ret < 0)
		return ret;

	uc_pdata->vdd_microvolts = ret;

	return 0;
}
#else
static inline int adc_vdd_platdata_update(struct udevice *dev) { return 0; }
#endif

#ifdef CONFIG_ADC_REQ_REGULATOR
static int adc_vss_platdata_update(struct udevice *dev)
{
	struct adc_uclass_platdata *uc_pdata = dev_get_uclass_platdata(dev);
	int ret;

	ret = device_get_supply_regulator(dev, "vss-supply",
					  &uc_pdata->vss_supply);
	if (ret)
		return ret;

	ret = regulator_get_value(uc_pdata->vss_supply);
	if (ret < 0)
		return ret;

	uc_pdata->vss_microvolts = ret;

	return 0;
}
#else
static inline int adc_vss_platdata_update(struct udevice *dev) { return 0; }
#endif

int adc_vdd_value(struct udevice *dev, int *uV)
{
	struct adc_uclass_platdata *uc_pdata = dev_get_uclass_platdata(dev);
	int ret, value_sign = uc_pdata->vdd_polarity_negative ? -1 : 1;

	if (!uc_pdata->vdd_supply)
		goto nodev;

	/* Update the regulator Value. */
	ret = adc_vdd_platdata_update(dev);
	if (ret)
		return ret;
nodev:
	if (uc_pdata->vdd_microvolts == -ENODATA)
		return -ENODATA;

	*uV = uc_pdata->vdd_microvolts * value_sign;

	return 0;
}

int adc_vss_value(struct udevice *dev, int *uV)
{
	struct adc_uclass_platdata *uc_pdata = dev_get_uclass_platdata(dev);
	int ret, value_sign = uc_pdata->vss_polarity_negative ? -1 : 1;

	if (!uc_pdata->vss_supply)
		goto nodev;

	/* Update the regulator Value. */
	ret = adc_vss_platdata_update(dev);
	if (ret)
		return ret;
nodev:
	if (uc_pdata->vss_microvolts == -ENODATA)
		return -ENODATA;

	*uV = uc_pdata->vss_microvolts * value_sign;

	return 0;
}

static int adc_vdd_platdata_set(struct udevice *dev)
{
	struct adc_uclass_platdata *uc_pdata = dev_get_uclass_platdata(dev);
	int ret;
	char *prop;

	prop = "vdd-polarity-negative";
	uc_pdata->vdd_polarity_negative = dev_read_bool(dev, prop);

	ret = adc_vdd_platdata_update(dev);
	if (ret != -ENOENT)
		return ret;

	/* No vdd-supply phandle. */
	prop  = "vdd-microvolts";
	uc_pdata->vdd_microvolts = dev_read_u32_default(dev, prop, -ENODATA);

	return 0;
}

static int adc_vss_platdata_set(struct udevice *dev)
{
	struct adc_uclass_platdata *uc_pdata = dev_get_uclass_platdata(dev);
	int ret;
	char *prop;

	prop = "vss-polarity-negative";
	uc_pdata->vss_polarity_negative = dev_read_bool(dev, prop);

	ret = adc_vss_platdata_update(dev);
	if (ret != -ENOENT)
		return ret;

	/* No vss-supply phandle. */
	prop = "vss-microvolts";
	uc_pdata->vss_microvolts = dev_read_u32_default(dev, prop, -ENODATA);

	return 0;
}

static int adc_pre_probe(struct udevice *dev)
{
	int ret;

	/* Set ADC VDD platdata: polarity, uV, regulator (phandle). */
	ret = adc_vdd_platdata_set(dev);
	if (ret)
		pr_err("%s: Can't update Vdd. Error: %d", dev->name, ret);

	/* Set ADC VSS platdata: polarity, uV, regulator (phandle). */
	ret = adc_vss_platdata_set(dev);
	if (ret)
		pr_err("%s: Can't update Vss. Error: %d", dev->name, ret);

	return 0;
}


static int do_rockchip_adc_read(cmd_tbl_t *cmdtp, int flag, int argc,
			char *const argv[])
{
	int ret,channel;
	u32 val; 
	char adc_index[11];
	char *board;
	char *rk3588="evb_rk3588";
	char *rk3568="evb_rk3568";
	int adc_flag=0;
	int index=10;
	int i;
	int rk3568_index[8]={229, 344, 460, 595, 732, 858, 975, 1024};
	int rk3588_index[8]={916, 1376, 1840, 2380, 2928, 3432, 3900, 4096};

	if (argc != 2) {
		printf("argc=%d\n",argc);
		return CMD_RET_USAGE;
	}

	channel=atoi(argv[1]);

	ret = adc_channel_single_shot("saradc", channel, &val);
	if (ret) {
		printf("%s: Failed to read saradc, ret=%d\n", __func__, ret);
		return 0;
	}

	board = env_get("board");
	ret = strcmp(rk3588 , board);	
	if(ret == 0){
		printf("board is rk3588\n");
		adc_flag=1;
	}
	else{
		if(!strcmp(rk3568,board)){
			printf("board is rk3588\n");
			adc_flag=2;
		}
	}

	for (i=0;i<8;i++){
		if(adc_flag==1){
			if(val < rk3588_index[i]){
				index=i;
				break;
			}
		}
		if(adc_flag==2){
			if(val < rk3568_index[i]){
				index=i;
				break;
			}
		}
	}

	char *str=simple_itoa(index);
	sprintf(adc_index, "adc_index_%d", channel);
	env_set(adc_index,str);	

	printf("val=%d,index=%d\n",val,index);
	return 0;
}

UCLASS_DRIVER(adc) = {
	.id	= UCLASS_ADC,
	.name	= "adc",
	.pre_probe =  adc_pre_probe,
	.per_device_platdata_auto_alloc_size = ADC_UCLASS_PLATDATA_SIZE,
};

U_BOOT_CMD(
	adc_read, 2, 1, do_rockchip_adc_read,
	"load and display log from resource partition",
	NULL
);
