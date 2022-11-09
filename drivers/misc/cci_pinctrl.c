// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 */

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/idr.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of.h>
#include <linux/sort.h>
#include <linux/of_platform.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/vfio.h>
#include <linux/hashtable.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>

static int cci_pinctrl_probe(struct platform_device *pdev)
{
        struct pinctrl *pinctrl;
        struct pinctrl_state *cci_default;
        int gpio_arr[4];
        char gpioname[30];
        int ret = 0;

        pinctrl = devm_pinctrl_get(&pdev->dev);

        if (IS_ERR_OR_NULL(pinctrl)) {
                ret = PTR_ERR(pinctrl);
                dev_err(&pdev->dev, "Failed to get pinctrl, err = %d", ret);
                return ret;
        }

        dev_info(&pdev->dev, "get pinctrl succeed\n");

        cci_default = pinctrl_lookup_state(pinctrl, "default");
        if (IS_ERR_OR_NULL(cci_default)) {
                ret = PTR_ERR(cci_default);
                dev_err(&pdev->dev, "Failed to get pinctrl state, err = %d", ret);
                return ret;
        }

        ret = pinctrl_select_state(pinctrl, cci_default);

        if(ret)
                dev_err(&pdev->dev, "Failed to get pinctrl state, err = %d", ret);
        else
                dev_err(&pdev->dev, "Set pinctrl state succeeded");


        gpio_arr[0] = of_get_named_gpio(pdev->dev.of_node,
				 "sensor-gpio1", 0);
        gpio_arr[1] = of_get_named_gpio(pdev->dev.of_node,
				 "sensor-gpio2", 0);
        gpio_arr[2] = of_get_named_gpio(pdev->dev.of_node,
				 "sensor-gpio3", 0);
        gpio_arr[3] = of_get_named_gpio(pdev->dev.of_node,
				 "sensor-gpio4", 0);
        for(int i=0; i<4; i++)
        {
                snprintf(gpioname, 13 , "sensor-gpio%d", i);
                gpio_request_one(gpio_arr[i], GPIOF_OUT_INIT_LOW, gpioname);
        }
        return (ret);
}

static int cci_pinctrl_remove(struct platform_device *pdev)
{
    return 0;
}

static const struct of_device_id cci_pinctl_id[] = {
    {.compatible = "qcom,cci-pinctlr",},
    {},
};

static struct platform_driver cci_pinctrl = {
    .probe = cci_pinctrl_probe,
    .remove = cci_pinctrl_remove,
    .driver = {
            .name = "cci_pinctrl",
            .of_match_table = cci_pinctl_id,
            .owner = THIS_MODULE,
        }
};
static int cci_pinctrl_init(void)
{
    return platform_driver_register(&cci_pinctrl);
}
module_init(cci_pinctrl_init);
MODULE_DESCRIPTION("cci_pinctrl");
MODULE_LICENSE("GPL v2");
