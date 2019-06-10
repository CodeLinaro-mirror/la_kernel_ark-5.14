// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2021, The Linux Foundation. All rights reserved.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/phy.h>
#include <linux/regulator/consumer.h>
#include <linux/of_gpio.h>

#include "stmmac.h"
#include "dwmac-qcom-ethqos.h"

#define EMAC_VREG_RGMII_NAME "vreg_rgmii"
#define EMAC_VREG_EMAC_PHY_NAME "vreg_emac_phy"
#define EMAC_VREG_RGMII_IO_PADS_NAME "vreg_rgmii_io_pads"

int ethqos_init_reqgulators(struct qcom_ethqos *ethqos)
{
	int ret = 0;

	if (of_property_read_bool(ethqos->pdev->dev.of_node,
				  "vreg_rgmii-supply")) {
		ethqos->reg_rgmii =
		devm_regulator_get(&ethqos->pdev->dev, EMAC_VREG_RGMII_NAME);
		if (IS_ERR(ethqos->reg_rgmii)) {
			ETHQOSERR("Can not get <%s>\n", EMAC_VREG_RGMII_NAME);
			return PTR_ERR(ethqos->reg_rgmii);
		}

		ret = regulator_enable(ethqos->reg_rgmii);
		if (ret) {
			ETHQOSERR("Can not enable <%s>\n",
				  EMAC_VREG_RGMII_NAME);
			goto reg_error;
		}

		ETHQOSDBG("Enabled <%s>\n", EMAC_VREG_RGMII_NAME);
	}

	if (of_property_read_bool(ethqos->pdev->dev.of_node,
				  "vreg_emac_phy-supply")) {
		ethqos->reg_emac_phy =
		devm_regulator_get(&ethqos->pdev->dev, EMAC_VREG_EMAC_PHY_NAME);
		if (IS_ERR(ethqos->reg_emac_phy)) {
			ETHQOSERR("Can not get <%s>\n",
				  EMAC_VREG_EMAC_PHY_NAME);
			return PTR_ERR(ethqos->reg_emac_phy);
		}

		ret = regulator_enable(ethqos->reg_emac_phy);
		if (ret) {
			ETHQOSERR("Can not enable <%s>\n",
				  EMAC_VREG_EMAC_PHY_NAME);
			goto reg_error;
		}

		ETHQOSDBG("Enabled <%s>\n", EMAC_VREG_EMAC_PHY_NAME);
	}

	if (of_property_read_bool(ethqos->pdev->dev.of_node,
				  "vreg_rgmii_io_pads-supply")) {
		ethqos->reg_rgmii_io_pads = devm_regulator_get
		(&ethqos->pdev->dev, EMAC_VREG_RGMII_IO_PADS_NAME);
		if (IS_ERR(ethqos->reg_rgmii_io_pads)) {
			ETHQOSERR("Can not get <%s>\n",
				  EMAC_VREG_RGMII_IO_PADS_NAME);
			return PTR_ERR(ethqos->reg_rgmii_io_pads);
		}

		ret = regulator_enable(ethqos->reg_rgmii_io_pads);
		if (ret) {
			ETHQOSERR("Can not enable <%s>\n",
				  EMAC_VREG_RGMII_IO_PADS_NAME);
			goto reg_error;
		}

		ETHQOSDBG("Enabled <%s>\n", EMAC_VREG_RGMII_IO_PADS_NAME);
	}

	return ret;

reg_error:
	ETHQOSERR("%s failed\n", __func__);
	ethqos_disable_regulators(ethqos);
	return ret;
}
EXPORT_SYMBOL(ethqos_init_reqgulators);

void ethqos_disable_regulators(struct qcom_ethqos *ethqos)
{
	if (ethqos->reg_rgmii) {
		regulator_disable(ethqos->reg_rgmii);
		ethqos->reg_rgmii = NULL;
	}

	if (ethqos->reg_emac_phy) {
		regulator_disable(ethqos->reg_emac_phy);
		ethqos->reg_emac_phy = NULL;
	}

	if (ethqos->reg_rgmii_io_pads) {
		regulator_disable(ethqos->reg_rgmii_io_pads);
		ethqos->reg_rgmii_io_pads = NULL;
	}
}
EXPORT_SYMBOL(ethqos_disable_regulators);

static int ethqos_init_pinctrl(struct device *dev)
{
	struct pinctrl *pinctrl;
	struct pinctrl_state *pinctrl_state;
	int i = 0;
	int num_names;
	const char *name;
	int ret = 0;

	pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR_OR_NULL(pinctrl)) {
		ret = PTR_ERR(pinctrl);
		ETHQOSERR("Failed to get pinctrl, err = %d\n", ret);
		return ret;
	}

	num_names = of_property_count_strings(dev->of_node, "pinctrl-names");
	if (num_names < 0) {
		dev_err(dev, "Cannot parse pinctrl-names: %d\n",
			num_names);
		devm_pinctrl_put(pinctrl);
		return num_names;
	}

	for (i = 0; i < num_names; i++) {
		ret = of_property_read_string_index(dev->of_node,
						    "pinctrl-names",
						    i, &name);

		if (!strcmp(name, PINCTRL_STATE_DEFAULT))
			continue;

		pinctrl_state = pinctrl_lookup_state(pinctrl, name);
		if (IS_ERR_OR_NULL(pinctrl_state)) {
			ret = PTR_ERR(pinctrl_state);
			ETHQOSERR("lookup_state %s failed %d\n", name, ret);
			goto err_pinctrl;
		}

		ETHQOSDBG("pinctrl_lookup_state %s succeeded\n", name);

		ret = pinctrl_select_state(pinctrl, pinctrl_state);
		if (ret) {
			ETHQOSERR("select_state %s failed %d\n", name, ret);
			goto err_pinctrl;
		}

		ETHQOSDBG("pinctrl_select_state %s succeeded\n", name);
	}

	return 0;

err_pinctrl:
	devm_pinctrl_put(pinctrl);
	return ret;
}

int ethqos_init_gpio(struct qcom_ethqos *ethqos)
{
	int ret = 0;

	ret = ethqos_init_pinctrl(&ethqos->pdev->dev);
	if (ret) {
		ETHQOSERR("ethqos_init_pinctrl failed");
		return ret;
	}

	return ret;
}
EXPORT_SYMBOL(ethqos_init_gpio);
