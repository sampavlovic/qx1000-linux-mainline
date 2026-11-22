// SPDX-License-Identifier: GPL-2.0
/*
 * PMI8998 (SMB2) Type-C port backend for the Qualcomm PMIC TCPM driver.
 *
 * PMI8998 has no standalone Type-C peripheral: the Type-C state machine lives
 * in the SMB2 USBIN peripheral (0x1300), next to the charger input path that
 * qcom_smbx drives. Register layout and the sequences below come from the
 * vendor smb-lib.c / smb-reg.h / qpnp-smb2.c (msm-4.4).
 *
 * Register ownership (all offsets from the USBIN base):
 *   this driver: 0x67 TYPE_C_INTRPT_ENB, 0x68 TYPE_C_INTRPT_ENB_SOFTWARE_CTRL
 *                (power role cmd, VCONN, EXIT_SNK_BASED_ON_CC),
 *                0x59 TYPE_C_CFG_2 (VCONN soft-start), 0x5a TYPE_C_CFG_3
 *                (try.SNK, legacy cable irqs); read-only 0x0b-0x0f, 0x10.
 *   qcom_smbx:   everything else in USBIN (ICL, APSD, 0x58 TYPE_C_CFG ...);
 *                it skips its 0x68 init write when this node is enabled.
 */

#include <linux/bits.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/usb/tcpm.h>
#include <linux/devm-helpers.h>
#include <linux/workqueue.h>

#include "qcom_pmic_typec.h"
#include "qcom_pmic_typec_port.h"

#define TYPE_C_STATUS_1_REG			0x0b	/* we are UFP: partner's Rp */
#define UFP_TYPEC_RDSTD_BIT			BIT(7)
#define UFP_TYPEC_RD1P5_BIT			BIT(6)
#define UFP_TYPEC_RD3P0_BIT			BIT(5)

#define TYPE_C_STATUS_2_REG			0x0c	/* we are DFP: partner's Rd/Ra */
#define DFP_TYPEC_MASK				GENMASK(3, 0)
#define DFP_RD_OPEN_BIT				BIT(3)
#define DFP_RD_RA_VCONN_BIT			BIT(2)
#define DFP_RD_RD_BIT				BIT(1)
#define DFP_RA_RA_BIT				BIT(0)

#define TYPE_C_STATUS_4_REG			0x0e
#define UFP_DFP_MODE_STATUS_BIT			BIT(7)	/* 1: attached as DFP/source */
#define TYPEC_VCONN_OVERCURR_STATUS_BIT		BIT(2)
#define CC_ORIENTATION_BIT			BIT(1)	/* 1: CC2 */
#define CC_ATTACHED_BIT				BIT(0)

#define USBIN_INT_RT_STS_REG			0x10
#define USBIN_PLUGIN_RT_STS_BIT			BIT(4)

#define TYPE_C_CFG_2_REG			0x59
#define VCONN_SOFTSTART_CFG_MASK		GENMASK(5, 4)

#define TYPE_C_CFG_3_REG			0x5a
#define TYPEC_LEGACY_CABLE_INT_EN_BIT		BIT(6)
#define TYPEC_NONCOMPLIANT_LEGACY_CABLE_INT_EN_BIT	BIT(5)
#define EN_TRYSINK_MODE_BIT			BIT(2)

#define TYPE_C_INTRPT_ENB_REG			0x67
#define TYPEC_CCOUT_DETACH_INT_EN_BIT		BIT(7)
#define TYPEC_CCOUT_ATTACH_INT_EN_BIT		BIT(6)
#define TYPEC_CCSTATE_CHANGE_INT_EN_BIT		BIT(2)
#define TYPEC_VBUS_DEASSERT_INT_EN_BIT		BIT(1)
#define TYPEC_VBUS_ASSERT_INT_EN_BIT		BIT(0)

#define TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG	0x68
#define EXIT_SNK_BASED_ON_CC_BIT		BIT(7)
#define VCONN_EN_ORIENTATION_BIT		BIT(6)
#define VCONN_EN_SRC_BIT			BIT(4)
#define VCONN_EN_VALUE_BIT			BIT(3)
#define TYPEC_POWER_ROLE_CMD_MASK		GENMASK(2, 0)
#define UFP_EN_CMD_BIT				BIT(2)
#define DFP_EN_CMD_BIT				BIT(1)
#define TYPEC_DISABLE_CMD_BIT			BIT(0)

/* Private to this file; the core only stores the pointer. */
struct pmic_typec_port {
	struct device		*dev;
	struct tcpm_port	*tcpm_port;
	struct regmap		*regmap;
	u32			base;
	int			irq;
	struct regulator	*vbus;		/* NULL: cannot source VBUS */
	bool			vbus_enabled;
	struct mutex		vbus_lock;	/* VBUS state serialization */

	/* VBUS edges raise no Type-C interrupt: poll USBIN plug-in */
	struct delayed_work	vbus_poll;
	bool			vbus_seen;
};

static struct pmic_typec_port *to_port(struct tcpc_dev *tcpc)
{
	return tcpc_to_tcpm(tcpc)->pmic_typec_port;
}

static int port_read(struct pmic_typec_port *port, unsigned int reg,
		     unsigned int *val)
{
	return regmap_read(port->regmap, port->base + reg, val);
}

static int port_update(struct pmic_typec_port *port, unsigned int reg,
		       unsigned int mask, unsigned int val)
{
	return regmap_update_bits(port->regmap, port->base + reg, mask, val);
}

static int port_set_role_cmd(struct pmic_typec_port *port, unsigned int cmd)
{
	return port_update(port, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
			   TYPEC_POWER_ROLE_CMD_MASK, cmd);
}

static irqreturn_t pmi8998_typec_isr(int irq, void *dev_id)
{
	struct pmic_typec_port *port = dev_id;
	unsigned int stat;

	if (!port_read(port, TYPE_C_STATUS_4_REG, &stat) &&
	    (stat & TYPEC_VCONN_OVERCURR_STATUS_BIT))
		/* TODO: vendor runs a VCONN OC recovery (smblib_vconn_oc_work) */
		dev_warn(port->dev, "VCONN over-current\n");

	/*
	 * One SPMI line ("type-c-change") covers CC and VBUS events; TCPM
	 * re-reads both, so report both.
	 */
	tcpm_cc_change(port->tcpm_port);
	tcpm_vbus_change(port->tcpm_port);

	return IRQ_HANDLED;
}

/*
 * "type-c-change" fires for CC changes but not for VBUS: after TCPM's port
 * reset the source re-applies VBUS and TCPM never hears of it, so it stays
 * in SNK_DEBOUNCED. Report USBIN plug-in changes from a cheap poll.
 */
#define PMI8998_VBUS_POLL_MS	100

static void pmi8998_typec_vbus_poll(struct work_struct *work)
{
	struct pmic_typec_port *port = container_of(to_delayed_work(work),
						    struct pmic_typec_port, vbus_poll);
	unsigned int stat;
	bool on;

	if (!port_read(port, USBIN_INT_RT_STS_REG, &stat)) {
		on = stat & USBIN_PLUGIN_RT_STS_BIT;
		if (on != port->vbus_seen) {
			port->vbus_seen = on;
			if (port->tcpm_port)
				tcpm_vbus_change(port->tcpm_port);
		}
	}
	schedule_delayed_work(&port->vbus_poll, msecs_to_jiffies(PMI8998_VBUS_POLL_MS));
}

static int pmi8998_typec_get_vbus(struct tcpc_dev *tcpc)
{
	struct pmic_typec_port *port = to_port(tcpc);
	unsigned int stat = 0;
	int ret;

	mutex_lock(&port->vbus_lock);
	port_read(port, USBIN_INT_RT_STS_REG, &stat);
	ret = port->vbus_enabled || (stat & USBIN_PLUGIN_RT_STS_BIT);
	mutex_unlock(&port->vbus_lock);

	return ret;
}

static int pmi8998_typec_set_vbus(struct tcpc_dev *tcpc, bool on, bool sink)
{
	struct pmic_typec_port *port = to_port(tcpc);
	int ret = 0;

	mutex_lock(&port->vbus_lock);
	if (port->vbus_enabled == on)
		goto out;

	if (!port->vbus) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	/*
	 * TODO: no vSafe5V/vSafe0V poll like pm8150b: which PMI8998 status bit
	 * tracks our own OTG output is unverified.
	 */
	ret = on ? regulator_enable(port->vbus) : regulator_disable(port->vbus);
	if (ret)
		goto out;

	port->vbus_enabled = on;
	tcpm_vbus_change(port->tcpm_port);
out:
	mutex_unlock(&port->vbus_lock);

	return ret;
}

static int pmi8998_typec_get_cc(struct tcpc_dev *tcpc,
				enum typec_cc_status *cc1,
				enum typec_cc_status *cc2)
{
	struct pmic_typec_port *port = to_port(tcpc);
	enum typec_cc_status active, other = TYPEC_CC_OPEN;
	unsigned int stat, val;
	int ret;

	*cc1 = TYPEC_CC_OPEN;
	*cc2 = TYPEC_CC_OPEN;

	ret = port_read(port, TYPE_C_STATUS_4_REG, &stat);
	if (ret || !(stat & CC_ATTACHED_BIT))
		return ret;

	if (stat & UFP_DFP_MODE_STATUS_BIT) {
		/* We are the source: what does the partner terminate with? */
		ret = port_read(port, TYPE_C_STATUS_2_REG, &val);
		if (ret)
			return ret;

		switch (val & DFP_TYPEC_MASK) {
		case DFP_RD_OPEN_BIT:
			active = TYPEC_CC_RD;
			break;
		case DFP_RD_RA_VCONN_BIT:
			active = TYPEC_CC_RD;
			other = TYPEC_CC_RA;
			break;
		case DFP_RD_RD_BIT:	/* debug accessory */
			active = TYPEC_CC_RD;
			other = TYPEC_CC_RD;
			break;
		case DFP_RA_RA_BIT:	/* audio accessory */
			active = TYPEC_CC_RA;
			other = TYPEC_CC_RA;
			break;
		default:
			dev_dbg(port->dev, "unexpected DFP status %#x\n", val);
			return 0;
		}
	} else {
		/* We are the sink: which Rp does the source advertise? */
		ret = port_read(port, TYPE_C_STATUS_1_REG, &val);
		if (ret)
			return ret;

		if (val & UFP_TYPEC_RD3P0_BIT)
			active = TYPEC_CC_RP_3_0;
		else if (val & UFP_TYPEC_RD1P5_BIT)
			active = TYPEC_CC_RP_1_5;
		else if (val & UFP_TYPEC_RDSTD_BIT)
			active = TYPEC_CC_RP_DEF;
		else
			/* debounce still running, or a factory/legacy mode */
			return 0;
	}

	if (stat & CC_ORIENTATION_BIT) {
		*cc2 = active;
		*cc1 = other;
	} else {
		*cc1 = active;
		*cc2 = other;
	}

	dev_dbg(port->dev, "get_cc: status4 %#x cc1 %d cc2 %d\n", stat, *cc1, *cc2);

	return 0;
}

static int pmi8998_typec_set_cc(struct tcpc_dev *tcpc, enum typec_cc_status cc)
{
	struct pmic_typec_port *port = to_port(tcpc);

	/*
	 * The SMB2 block only takes a power role command; it runs attach and
	 * debounce itself. Map TCPM's terminations onto it the way the vendor
	 * policy engine did (set_power_role() on PR swap / hard reset).
	 *
	 * TODO: the Rp current (default/1.5A/3.0A) is not programmed. TYPE_C_CFG_2
	 * has TYPE_C_DFP_CURRSRC_MODE and EN_80UA_180UA_CUR_SOURCE but their
	 * encoding is undocumented, so as a source we always advertise the HW
	 * default and PD 3.0 collision avoidance (SinkTxOk/NG) is a no-op. The
	 * vendor stack also limited itself to PD 2.0 as a source.
	 */
	switch (cc) {
	case TYPEC_CC_OPEN:
		return port_set_role_cmd(port, TYPEC_DISABLE_CMD_BIT);
	case TYPEC_CC_RD:
		return port_set_role_cmd(port, UFP_EN_CMD_BIT);
	case TYPEC_CC_RP_DEF:
	case TYPEC_CC_RP_1_5:
	case TYPEC_CC_RP_3_0:
		return port_set_role_cmd(port, DFP_EN_CMD_BIT);
	default:
		return -EINVAL;
	}
}

static int pmi8998_typec_set_polarity(struct tcpc_dev *tcpc,
				      enum typec_cc_polarity pol)
{
	/*
	 * The HW picks the CC line for the PD PHY (HW controlled CC_OUT) and
	 * the QMP PHY lanes are flipped by the TCPM orientation switch.
	 *
	 * TODO: while PD is active the vendor stack froze CC_OUT in software
	 * (TAPER_TIMER_SEL_CFG 0x64 bit 7, TYPEC_SPARE_CFG) with the pin taken
	 * from VCONN_EN_ORIENTATION; add that here if PD drops during hard
	 * reset or role swaps.
	 */
	return 0;
}

static int pmi8998_typec_set_vconn(struct tcpc_dev *tcpc, bool on)
{
	struct pmic_typec_port *port = to_port(tcpc);
	unsigned int stat, val = 0;
	int ret;

	if (on) {
		ret = port_read(port, TYPE_C_STATUS_4_REG, &stat);
		if (ret)
			return ret;
		/* VCONN goes on the CC pin that is not carrying CC */
		val = VCONN_EN_VALUE_BIT |
		      ((stat & CC_ORIENTATION_BIT) ? 0 : VCONN_EN_ORIENTATION_BIT);
	}

	return port_update(port, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
			   on ? VCONN_EN_VALUE_BIT | VCONN_EN_ORIENTATION_BIT :
				VCONN_EN_VALUE_BIT, val);
}

static int pmi8998_typec_start_toggling(struct tcpc_dev *tcpc,
					enum typec_port_type port_type,
					enum typec_cc_status cc)
{
	struct pmic_typec_port *port = to_port(tcpc);
	unsigned int cmd;

	switch (port_type) {
	case TYPEC_PORT_SRC:
		cmd = DFP_EN_CMD_BIT;
		break;
	case TYPEC_PORT_SNK:
		cmd = UFP_EN_CMD_BIT;
		break;
	default:
		/*
		 * Plain DRP. HW try.SNK stays off (vendor disabled it too); TCPM
		 * runs Try.SNK itself through set_cc() when try-power-role is set.
		 */
		cmd = 0;
		break;
	}

	return port_set_role_cmd(port, cmd);
}

static int pmi8998_typec_port_start(struct pmic_typec *tcpm,
				    struct tcpm_port *tcpm_port)
{
	struct pmic_typec_port *port = tcpm->pmic_typec_port;
	int ret;

	port->tcpm_port = tcpm_port;

	/* Raise "type-c-change" on attach/detach, CC state and VBUS edges */
	ret = regmap_write(port->regmap, port->base + TYPE_C_INTRPT_ENB_REG,
			   TYPEC_CCOUT_DETACH_INT_EN_BIT |
			   TYPEC_CCOUT_ATTACH_INT_EN_BIT |
			   TYPEC_CCSTATE_CHANGE_INT_EN_BIT |
			   TYPEC_VBUS_DEASSERT_INT_EN_BIT |
			   TYPEC_VBUS_ASSERT_INT_EN_BIT);
	if (ret)
		return ret;

	/* Vendor smb2_configure_typec(): slower VCONN soft-start */
	ret = port_update(port, TYPE_C_CFG_2_REG, VCONN_SOFTSTART_CFG_MASK,
			  VCONN_SOFTSTART_CFG_MASK);
	if (ret)
		return ret;

	/* Vendor: no HW try.SNK, no legacy cable irqs */
	ret = port_update(port, TYPE_C_CFG_3_REG,
			  EN_TRYSINK_MODE_BIT | TYPEC_LEGACY_CABLE_INT_EN_BIT |
			  TYPEC_NONCOMPLIANT_LEGACY_CABLE_INT_EN_BIT, 0);
	if (ret)
		return ret;

	/*
	 * VCONN under software control, off. Leave the sink attached on CC
	 * rather than on VBUS, so a PD hard reset (VBUS drops to 0 V) does not
	 * detach us.
	 * TODO: vendor set EXIT_SNK_BASED_ON_CC only for the duration of a hard
	 * reset; keeping it on is untested on hardware.
	 */
	ret = port_update(port, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
			  EXIT_SNK_BASED_ON_CC_BIT | VCONN_EN_SRC_BIT |
			  VCONN_EN_VALUE_BIT,
			  EXIT_SNK_BASED_ON_CC_BIT | VCONN_EN_SRC_BIT);
	if (ret)
		return ret;

	enable_irq(port->irq);

	return 0;
}

static void pmi8998_typec_port_stop(struct pmic_typec *tcpm)
{
	disable_irq(tcpm->pmic_typec_port->irq);
}

int qcom_pmic_typec_port_pmi8998_probe(struct platform_device *pdev,
				       struct pmic_typec *tcpm,
				       struct regmap *regmap, u32 base)
{
	struct device *dev = &pdev->dev;
	struct pmic_typec_port *port;
	struct fwnode_handle *connector;
	int ret;

	port = devm_kzalloc(dev, sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;

	port->dev = dev;
	port->regmap = regmap;
	port->base = base;
	mutex_init(&port->vbus_lock);

	connector = device_get_named_child_node(dev, "connector");
	if (!connector)
		return -EINVAL;

	/*
	 * PMI8998 sources VBUS from the SMB2 OTG boost (0x1100). There is no
	 * mainline regulator for it yet: qcom_usb_vbus-regulator's pm8150b
	 * layout matches CMD_OTG/OTG_CFG but PMI8998's current-limit table
	 * starts at 250 mA and the vendor needs the ENG_BUCKBOOST_HALT1_8 and
	 * soft-start workarounds. Without connector/vbus-supply we cannot be
	 * a source; use power-role = "sink".
	 */
	port->vbus = devm_of_regulator_get_optional(dev, to_of_node(connector),
						    "vbus");
	fwnode_handle_put(connector);
	if (port->vbus == ERR_PTR(-ENODEV))
		port->vbus = NULL;
	if (IS_ERR(port->vbus))
		return PTR_ERR(port->vbus);

	port->irq = platform_get_irq_byname(pdev, "type-c-change");
	if (port->irq < 0)
		return port->irq;

	ret = devm_delayed_work_autocancel(dev, &port->vbus_poll,
					  pmi8998_typec_vbus_poll);
	if (ret)
		return ret;
	schedule_delayed_work(&port->vbus_poll, msecs_to_jiffies(PMI8998_VBUS_POLL_MS));

	ret = devm_request_threaded_irq(dev, port->irq, NULL, pmi8998_typec_isr,
					IRQF_ONESHOT | IRQF_NO_AUTOEN,
					"type-c-change", port);
	if (ret)
		return ret;

	tcpm->pmic_typec_port = port;

	tcpm->tcpc.get_vbus = pmi8998_typec_get_vbus;
	tcpm->tcpc.set_vbus = pmi8998_typec_set_vbus;
	tcpm->tcpc.get_cc = pmi8998_typec_get_cc;
	tcpm->tcpc.set_cc = pmi8998_typec_set_cc;
	tcpm->tcpc.set_polarity = pmi8998_typec_set_polarity;
	tcpm->tcpc.set_vconn = pmi8998_typec_set_vconn;
	tcpm->tcpc.start_toggling = pmi8998_typec_start_toggling;

	tcpm->port_start = pmi8998_typec_port_start;
	tcpm->port_stop = pmi8998_typec_port_stop;

	return 0;
}
