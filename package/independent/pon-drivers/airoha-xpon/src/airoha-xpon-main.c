// SPDX-License-Identifier: GPL-2.0-only

#include <airoha-pon-frontend.h>
#include <linux/airoha-eth.h>
#include <linux/atomic.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/pcs/pcs-airoha.h>
#include <linux/pcs/pcs.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/rtnetlink.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#include "airoha-xpon-private.h"
#include "airoha-xpon-epon.h"
#include "airoha-xpon-oam.h"
#include "airoha-xpon-omci.h"
#include "airoha-xpon-xgpon.h"

void airoha_xpon_set_data_path_link(struct airoha_xpon *xpon, bool ready)
{
	/*
	 * airoha_eth leaves TX queues stopped at ndo_open. Publish carrier and
	 * queue readiness together after GEM/T-CONT or LLID/channel metadata is
	 * installed, so VLAN upper devices observe a complete path.
	 */
	if (ready) {
		netif_carrier_on(xpon->pon_netdev);
		netif_tx_wake_all_queues(xpon->pon_netdev);
	} else {
		netif_carrier_off(xpon->pon_netdev);
		netif_tx_disable(xpon->pon_netdev);
	}
}

static irqreturn_t airoha_xpon_irq(int irq, void *data)
{
	struct airoha_xpon *xpon = data;
	bool ready = READ_ONCE(xpon->active_mode_valid) &&
	             READ_ONCE(xpon->mac_initialized);

	/*
	 * EPON Discovery grants have a strict start time, so hard IRQ submits the
	 * MPCP command. The threaded handler handles sleepable state changes and
	 * the XG-PON FIFO.
	 */
	if (!ready)
		return IRQ_NONE;
	if (airoha_xpon_mode_is_epon(READ_ONCE(xpon->active_mode)))
		return airoha_xpon_epon_irq_fast(xpon);

	return IRQ_WAKE_THREAD;
}

static irqreturn_t airoha_xpon_phy_irq_thread(int irq, void *data)
{
	struct airoha_xpon *xpon = data;
	enum airoha_xpon_mode mode = READ_ONCE(xpon->active_mode);
	enum airoha_pcs_pon_mode profile = READ_ONCE(xpon->pcs_profile);
	bool ready = READ_ONCE(xpon->active_mode_valid) &&
	             READ_ONCE(xpon->pcs_profile_valid) &&
	             airoha_xpon_pcs_profile_matches(mode, profile);

	/* PCS register banks follow the active hardware profile. */
	if (!ready)
		return IRQ_NONE;
	if (airoha_xpon_mode_is_epon(mode))
		return airoha_xpon_epon_phy_irq(xpon);

	return airoha_xpon_xgpon_phy_irq(xpon);
}

static irqreturn_t airoha_xpon_irq_thread(int irq, void *data)
{
	struct airoha_xpon *xpon = data;
	bool ready = READ_ONCE(xpon->active_mode_valid) &&
	             READ_ONCE(xpon->mac_initialized);

	/* Both MACs share this IRQ; the active bank is available after initialization. */
	if (!ready)
		return IRQ_NONE;
	if (airoha_xpon_mode_is_epon(READ_ONCE(xpon->active_mode)))
		return airoha_xpon_epon_irq(xpon);

	return airoha_xpon_xgpon_irq(xpon);
}

static int airoha_xpon_prepare_frontend_locked(struct airoha_xpon *xpon)
{
	const struct airoha_xpon_mode_info *mode;
	int ret;

	mode = airoha_xpon_mode_info(xpon->active_mode);
	if (!mode || !mode->mac_supported)
		return -EOPNOTSUPP;

	ret = airoha_pon_frontend_set_tx_enable(xpon->frontend, false);
	if (ret)
		return ret;
	ret = airoha_pon_frontend_get_link_config(
		xpon->frontend, mode->frontend_mode, &xpon->frontend_link);
	if (ret)
		return ret;
	ret = airoha_pon_frontend_prepare(xpon->frontend, mode->frontend_mode);
	if (ret)
		return ret;

	if (airoha_xpon_mode_is_epon(xpon->active_mode)) {
		airoha_xpon_epon_mask_irqs(xpon);
		xpon->lifecycle = AIROHA_XPON_WAIT_OPTICAL_SIGNAL;
		return 0;
	}

	return airoha_xpon_xgpon_prepare(xpon);
}

static int airoha_xpon_activate_rx_locked(struct airoha_xpon *xpon)
{
	if (airoha_xpon_mode_is_epon(xpon->active_mode))
		return airoha_xpon_epon_start_rx(xpon);

	return airoha_xpon_xgpon_start_rx(xpon);
}

static void airoha_xpon_deactivate_rx_locked(struct airoha_xpon *xpon)
{
	if (airoha_xpon_mode_is_epon(xpon->active_mode))
		airoha_xpon_epon_stop_rx(xpon);
	else
		airoha_xpon_xgpon_stop_rx(xpon);

	xpon->lifecycle = AIROHA_XPON_WAIT_OPTICAL_SIGNAL;
}

static void airoha_xpon_start_work(struct work_struct *work)
{
	struct airoha_xpon *xpon =
		container_of(work, struct airoha_xpon, start_work);
	bool start_monitor = false;
	int ret;

	mutex_lock(&xpon->state_lock);
	if (xpon->stopping)
		goto out_unlock;
	if (xpon->lifecycle != AIROHA_XPON_STOPPED &&
	    xpon->lifecycle != AIROHA_XPON_ERROR)
		goto out_unlock;

	xpon->last_start_error = 0;
	ret = airoha_xpon_prepare_frontend_locked(xpon);
	if (ret) {
		xpon->last_start_error = ret;
		xpon->lifecycle = AIROHA_XPON_ERROR;
		dev_err(xpon->dev, "PON RX startup failed: %d\n", ret);
	} else if (!xpon->stopping) {
		/*
		 * PMA and digital PHY initialization precede LOS/RX_RDY polling.
		 */
		ret = airoha_xpon_activate_rx_locked(xpon);
		if (ret) {
			xpon->last_start_error = ret;
			xpon->lifecycle = AIROHA_XPON_ERROR;
			dev_err(xpon->dev,
			        "initial PON line/PMA configuration failed: %d; BEN remains disabled\n",
			        ret);
		} else if (!xpon->stopping) {
			start_monitor = true;
		}
	}

out_unlock:
	mutex_unlock(&xpon->state_lock);
	if (start_monitor)
		schedule_delayed_work(&xpon->link_work, 0);
}

static int airoha_xpon_controller_start(void *priv)
{
	struct airoha_xpon *xpon = priv;
	const struct airoha_xpon_mode_info *mode;
	int ret = 0;

	/* ndo_open holds RTNL. PMA/I2C initialization runs in queued work outside
	 * that critical section.
	 */
	mutex_lock(&xpon->state_lock);
	if (xpon->lifecycle != AIROHA_XPON_STOPPED) {
		ret = -EBUSY;
		goto out_unlock;
	}
	mode = airoha_xpon_mode_info(xpon->configured_mode);
	if (!mode || !mode->mac_supported ||
	    !airoha_pon_frontend_supports(xpon->frontend,
	                                  mode->frontend_mode)) {
		ret = -EOPNOTSUPP;
		xpon->last_start_error = ret;
		goto out_unlock;
	}
	xpon->stopping = false;
	xpon->active_mode = xpon->configured_mode;
	xpon->active_mode_valid = true;
	xpon->identity.active_identity = xpon->identity.pending_identity;
	xpon->last_start_error = 0;
	atomic_set(&xpon->phy_events, 0);
	atomic_set(&xpon->mac_restart_pending, 0);
	atomic_set(&xpon->tx_resync_pending, 0);
	atomic_set(&xpon->data_path_retry_pending, 0);
	atomic_set(&xpon->epon.pending_events, 0);
	atomic_set(&xpon->epon.pending_error_events, 0);
out_unlock:
	mutex_unlock(&xpon->state_lock);
	if (ret)
		return ret;

	if (!schedule_work(&xpon->start_work))
		return -EBUSY;
	return 0;
}

static void airoha_xpon_controller_stop(void *priv)
{
	struct airoha_xpon *xpon = priv;
	bool epon = READ_ONCE(xpon->active_mode_valid) &&
	            airoha_xpon_mode_is_epon(READ_ONCE(xpon->active_mode));

	/* The stopping gate closes asynchronous entry and upstream bursts. */
	WRITE_ONCE(xpon->stopping, true);
	atomic_set(&xpon->mac_restart_pending, 0);
	atomic_set(&xpon->tx_resync_pending, 0);
	atomic_set(&xpon->data_path_retry_pending, 0);
	if (epon) {
		WRITE_ONCE(xpon->tx_armed, false);
		airoha_pon_frontend_set_tx_enable(xpon->frontend, false);
		airoha_xpon_epon_mask_irqs(xpon);
	} else {
		airoha_xpon_disarm_upstream(xpon, "netdev-stop-early");
		airoha_xpon_xgpon_mask_irqs(xpon);
	}
	synchronize_irq(xpon->mac_irq);
	synchronize_irq(xpon->phy_irq);

	/* Workers drain outside state_lock so their exit path can acquire it. */
	cancel_work_sync(&xpon->start_work);
	cancel_delayed_work_sync(&xpon->link_work);

	/* Mask IRQs again after workers that may have changed enable bits. */
	if (epon)
		airoha_xpon_epon_mask_irqs(xpon);
	else
		airoha_xpon_xgpon_mask_irqs(xpon);
	synchronize_irq(xpon->mac_irq);
	synchronize_irq(xpon->phy_irq);

	mutex_lock(&xpon->state_lock);
	if (!epon)
		airoha_xpon_disarm_upstream(xpon, "netdev-stop-final");
	if (xpon->rx_active)
		airoha_xpon_deactivate_rx_locked(xpon);
	else if (!epon && xpon->mac_initialized)
		airoha_xpon_clear_assignment(xpon);
	/* disarm_upstream() closes the board TX gate before provider teardown. */
	if (airoha_pon_frontend_unprepare(xpon->frontend))
		dev_warn_ratelimited(
			xpon->dev,
			"failed to unprepare optical frontend; physical TX gate remains disabled\n");
	xpon->optical_signal = false;
	xpon->rx_active = false;
	xpon->service_ready = false;
	xpon->lifecycle = AIROHA_XPON_STOPPED;
	xpon->active_mode_valid = false;
	airoha_xpon_leds_update(xpon);
	mutex_unlock(&xpon->state_lock);
}

static void
airoha_xpon_controller_rx_control(void *priv, struct sk_buff *skb,
                                  const struct airoha_pon_ctrl_rx_info *info)
{
	struct airoha_xpon *xpon = priv;

	if (READ_ONCE(xpon->active_mode_valid) &&
	    airoha_xpon_mode_is_epon(READ_ONCE(xpon->active_mode))) {
		airoha_xpon_oam_receive(xpon->oam, skb, info);
		return;
	}
	airoha_xpon_omci_receive(xpon->omci, skb, info);
}

static bool
airoha_xpon_controller_match_control(void *priv,
                                     const struct airoha_pon_ctrl_rx_info *info)
{
	struct airoha_xpon *xpon = priv;
	u16 onu_id = READ_ONCE(xpon->onu_id);

	/*
	 * QDMA marks EPON OAM in its descriptor. ITU-T control traffic also uses
	 * the active OMCC ID for classification.
	 */
	if (READ_ONCE(xpon->active_mode_valid) &&
	    airoha_xpon_mode_is_epon(READ_ONCE(xpon->active_mode)))
		return false;

	/* The default OMCC XGEM Port-ID follows the active ONU-ID. */
	return xpon->omci && onu_id != AIROHA_XGPON_BROADCAST_ID &&
	       info->id == onu_id;
}

static void airoha_xpon_controller_wake_control_tx(void *priv)
{
	struct airoha_xpon *xpon = priv;

	if (READ_ONCE(xpon->active_mode_valid) &&
	    airoha_xpon_mode_is_epon(READ_ONCE(xpon->active_mode)))
		airoha_xpon_oam_wake_tx(xpon->oam);
	else
		airoha_xpon_omci_wake_tx(xpon->omci);
}

static const struct airoha_pon_link_ops airoha_xpon_link_ops = {
	.start = airoha_xpon_controller_start,
	.stop = airoha_xpon_controller_stop,
	.match_control = airoha_xpon_controller_match_control,
	.rx_control = airoha_xpon_controller_rx_control,
	.wake_control_tx = airoha_xpon_controller_wake_control_tx,
};

static bool airoha_xpon_handle_optical_loss_locked(struct airoha_xpon *xpon,
                                                   u32 phy_events,
                                                   unsigned long *delay)
{
	/* PMA PLUG_OUT precedes digital RX shutdown on RX_LOS. */
	if (!airoha_xpon_mode_is_epon(xpon->active_mode) &&
	    (phy_events & AIROHA_XGPON_PHY_INT_RX_LOS)) {
		if (xpon->rx_active) {
			/* Preserve the edge source separately from the next polled LOS level. */
			dev_info(
				xpon->dev,
				"PON RX stopping: LOS IRQ, sampled signal=%u, PHY events=0x%08x\n",
				xpon->optical_signal, phy_events);
			airoha_xpon_deactivate_rx_locked(xpon);
		}
		xpon->identity.active_identity =
			xpon->identity.pending_identity;
		xpon->no_sync_since = 0;
		*delay = msecs_to_jiffies(AIROHA_XPON_LINK_POLL_MS);
		return true;
	}

	if (xpon->optical_signal)
		return false;

	/*
	 * I2C LOS polling supplements SPI 43 edges. PLUG_OUT retains the first
	 * analog calibration so the next plug-in can use the short path.
	 */
	if (xpon->rx_active) {
		dev_info(
			xpon->dev,
			"PON RX stopping: optical signal absent, PHY events=0x%08x\n",
			phy_events);
		airoha_xpon_deactivate_rx_locked(xpon);
	}
	/* Latch pending identity on optical loss for the next O1 activation. */
	xpon->identity.active_identity = xpon->identity.pending_identity;
	if (xpon->lifecycle != AIROHA_XPON_ERROR)
		xpon->lifecycle = AIROHA_XPON_WAIT_OPTICAL_SIGNAL;
	xpon->no_sync_since = 0;
	xpon->stats.no_ready_recovery_count = 0;
	return true;
}

static void airoha_xpon_advance_rx_locked(struct airoha_xpon *xpon,
                                          u32 phy_events, unsigned long *delay)
{
	int ret;

	if (!airoha_xpon_mode_is_epon(xpon->active_mode)) {
		airoha_xpon_xgpon_advance(xpon, phy_events, delay);
		return;
	}

	if (!xpon->rx_active) {
		ret = airoha_xpon_epon_start_rx(xpon);
		if (ret) {
			xpon->last_start_error = ret;
			xpon->lifecycle = AIROHA_XPON_ERROR;
			*delay = msecs_to_jiffies(AIROHA_XPON_RETRAIN_RETRY_MS);
		}
		return;
	}

	ret = airoha_xpon_epon_advance(xpon, phy_events, delay);
	if (ret) {
		xpon->last_start_error = ret;
		xpon->lifecycle = AIROHA_XPON_ERROR;
		*delay = msecs_to_jiffies(AIROHA_XPON_RETRAIN_RETRY_MS);
	}
}

static void airoha_xpon_link_work(struct work_struct *work)
{
	struct airoha_xpon *xpon = container_of(to_delayed_work(work),
	                                        struct airoha_xpon, link_work);
	struct airoha_pon_frontend_signal_status optical = {};
	unsigned long delay = msecs_to_jiffies(AIROHA_XPON_LINK_POLL_MS);
	u32 phy_events = atomic_xchg(&xpon->phy_events, 0);
	bool requeue;
	int ret;

	/* Provider sampling may sleep and runs outside state_lock. */
	ret = airoha_pon_frontend_get_signal_status(xpon->frontend, &optical);
	mutex_lock(&xpon->state_lock);
	if (xpon->stopping)
		goto out_no_requeue;

	if (ret) {
		xpon->last_start_error = ret;
		xpon->lifecycle = AIROHA_XPON_ERROR;
		delay = msecs_to_jiffies(AIROHA_XPON_RETRAIN_RETRY_MS);
		dev_warn_ratelimited(
			xpon->dev,
			"failed to read optical frontend status: %d; retrying\n",
			ret);
		goto out_requeue;
	}

	/* The active digital PCS supplies LOS when provider signal validity is absent. */
	if (optical.signal_valid) {
		xpon->optical_signal = optical.signal_present;
	} else if (airoha_xpon_mode_is_epon(xpon->active_mode)) {
		xpon->optical_signal = !airoha_xpon_epon_los(xpon);
	} else {
		xpon->optical_signal = !airoha_xpon_xgpon_los(xpon);
	}
	airoha_xpon_leds_update(xpon);

	/* IRQ queues Deactivate; link_work commits the plain MAC reset under state_lock. */
	if (!airoha_xpon_mode_is_epon(xpon->active_mode)) {
		if (atomic_xchg(&xpon->mac_restart_pending, 0))
			airoha_xpon_xgpon_restart_mac(xpon, "OLT Deactivate");
		if (atomic_xchg(&xpon->data_path_retry_pending, 0))
			airoha_xpon_retry_data_paths(xpon);
	}

	if (airoha_xpon_handle_optical_loss_locked(xpon, phy_events, &delay))
		goto out_requeue;

	airoha_xpon_advance_rx_locked(xpon, phy_events, &delay);

out_requeue:
	if (!airoha_xpon_mode_is_epon(xpon->active_mode))
		airoha_xpon_xgpon_counters_update(xpon);
	requeue = !xpon->stopping;
	mutex_unlock(&xpon->state_lock);
	if (requeue)
		schedule_delayed_work(&xpon->link_work, delay);
	return;

out_no_requeue:
	mutex_unlock(&xpon->state_lock);
}

static void airoha_xpon_put_frontend(void *data)
{
	airoha_pon_frontend_put(data);
}

static void airoha_xpon_put_data_path(void *data)
{
	struct net_device *netdev = data;

	put_device(&netdev->dev);
}

static void airoha_xpon_destroy_control(void *data)
{
	struct airoha_xpon *xpon = data;

	rtnl_lock();
	if (xpon->omci) {
		airoha_xpon_omci_destroy_rtnl(xpon->omci);
		xpon->omci = NULL;
	}
	if (xpon->oam) {
		airoha_xpon_oam_destroy_rtnl(xpon->oam);
		xpon->oam = NULL;
	}
	rtnl_unlock();
}

static void airoha_xpon_remove_device_links(void *data)
{
	struct airoha_xpon *xpon = data;

	/* Remove instance links before devm releases their device references. */
	sysfs_remove_link(&xpon->pon_netdev->dev.kobj, "xpon");
	sysfs_remove_link(&xpon->dev->kobj, "frontend");
}

static int airoha_xpon_create_device_links(struct airoha_xpon *xpon)
{
	struct device *frontend_dev =
		airoha_pon_frontend_device(xpon->frontend);
	int ret;

	if (!frontend_dev)
		return -ENODEV;

	/* Instance links connect the PON netdev, controller, and optical frontend. */
	ret = sysfs_create_link(&xpon->pon_netdev->dev.kobj, &xpon->dev->kobj,
	                        "xpon");
	if (ret)
		return ret;
	ret = sysfs_create_link(&xpon->dev->kobj, &frontend_dev->kobj,
	                        "frontend");
	if (ret) {
		sysfs_remove_link(&xpon->pon_netdev->dev.kobj, "xpon");
		return ret;
	}

	return devm_add_action_or_reset(xpon->dev,
	                                airoha_xpon_remove_device_links, xpon);
}

static int airoha_xpon_get_data_path(struct device *dev,
                                     struct airoha_xpon *xpon)
{
	struct device_node *node;
	int ret;

	node = of_parse_phandle(dev->of_node, "data-path", 0);
	if (!node)
		return dev_err_probe(
			dev, -EINVAL,
			"missing data-path phandle for PON GDM2\n");
	xpon->pon_netdev = of_find_net_device_by_node(node);
	of_node_put(node);
	if (!xpon->pon_netdev)
		return dev_err_probe(dev, -EPROBE_DEFER,
		                     "PON GDM2 netdev is not registered yet\n");

	ret = devm_add_action_or_reset(dev, airoha_xpon_put_data_path,
	                               xpon->pon_netdev);
	if (ret)
		return ret;

	/*
	 * The phandle identifies airoha_eth's PON GDM2 instance. Carrier remains
	 * down until O5 and a valid GEM mapping publish the data path.
	 */
	ret = airoha_eth_pon_configure(xpon->pon_netdev, 0, 0, false);
	if (ret)
		return dev_err_probe(
			dev, ret,
			"data-path does not reference a valid PON GDM2\n");
	airoha_xpon_set_data_path_link(xpon, false);

	return 0;
}

static int airoha_xpon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct airoha_xpon *xpon;
	int ret;

	xpon = devm_kzalloc(dev, sizeof(*xpon), GFP_KERNEL);
	if (!xpon)
		return -ENOMEM;

	xpon->dev = dev;
	mutex_init(&xpon->state_lock);
	mutex_init(&xpon->ploam_lock);
	INIT_LIST_HEAD(&xpon->netlink_node);
	INIT_WORK(&xpon->start_work, airoha_xpon_start_work);
	INIT_DELAYED_WORK(&xpon->link_work, airoha_xpon_link_work);
	atomic_set(&xpon->phy_events, 0);
	atomic_set(&xpon->mac_restart_pending, 0);
	atomic_set(&xpon->tx_resync_pending, 0);
	atomic_set(&xpon->data_path_retry_pending, 0);
	atomic_set(&xpon->epon.discovery_gate_count, 0);
	atomic_set(&xpon->epon.register_request_count, 0);
	xpon->lifecycle = AIROHA_XPON_STOPPED;
	ret = airoha_xpon_mode_get_default(dev, &xpon->configured_mode);
	if (ret)
		return ret;
	xpon->active_mode = xpon->configured_mode;
	xpon->stopping = true;
	xpon->onu_state = AIROHA_XGPON_O1;
	xpon->onu_id = AIROHA_XGPON_BROADCAST_ID;
	ret = airoha_xpon_crypto_init(xpon);
	if (ret)
		return dev_err_probe(
			dev, ret, "failed to allocate the AES-ECB transform\n");
	ret = devm_add_action_or_reset(dev, airoha_xpon_crypto_cleanup, xpon);
	if (ret)
		return ret;
	ret = airoha_xpon_identity_init(dev, xpon);
	if (ret)
		return ret;

	xpon->xgpon_base = devm_platform_ioremap_resource_byname(pdev, "xgpon");
	if (IS_ERR(xpon->xgpon_base))
		return dev_err_probe(dev, PTR_ERR(xpon->xgpon_base),
		                     "failed to map XG-PON MAC registers\n");

	xpon->epon_base = devm_platform_ioremap_resource_byname(pdev, "epon");
	if (IS_ERR(xpon->epon_base))
		return dev_err_probe(dev, PTR_ERR(xpon->epon_base),
		                     "failed to map EPON MAC registers\n");

	xpon->pon_phy_base =
		devm_platform_ioremap_resource_byname(pdev, "pon-phy");
	if (IS_ERR(xpon->pon_phy_base))
		return dev_err_probe(
			dev, PTR_ERR(xpon->pon_phy_base),
			"failed to map digital PON PHY registers\n");

	xpon->scu = syscon_regmap_lookup_by_phandle(dev->of_node, "airoha,scu");
	if (IS_ERR(xpon->scu))
		return dev_err_probe(dev, PTR_ERR(xpon->scu),
		                     "failed to obtain the SCU regmap\n");

	/* pcs-handle identifies the provider that owns PCS/PMA registers. */
	xpon->pcs = fwnode_pcs_get(dev_fwnode(dev), 0);
	if (IS_ERR(xpon->pcs))
		return dev_err_probe(dev, PTR_ERR(xpon->pcs),
		                     "failed to obtain the PON PCS provider\n");

	/* optical-front-end identifies the provider owning the BOSA I2C address. */
	xpon->frontend = airoha_pon_frontend_get(dev, "optical-front-end");
	if (IS_ERR(xpon->frontend))
		return dev_err_probe(
			dev, PTR_ERR(xpon->frontend),
			"failed to obtain the optical frontend provider\n");

	ret = devm_add_action_or_reset(dev, airoha_xpon_put_frontend,
	                               xpon->frontend);
	if (ret)
		return ret;

	ret = airoha_xpon_get_data_path(dev, xpon);
	if (ret)
		return ret;
	ret = airoha_xpon_create_device_links(xpon);
	if (ret)
		return dev_err_probe(
			dev, ret,
			"failed to publish PON instance device links\n");

	xpon->leds = airoha_xpon_leds_create(dev, xpon->pon_netdev->name);
	if (xpon->leds) {
		ret = devm_add_action_or_reset(dev, airoha_xpon_leds_destroy,
		                               xpon->leds);
		if (ret) {
			xpon->leds = NULL;
			dev_warn(dev, "failed to retain PON LED triggers\n");
		}
	}
	airoha_xpon_leds_update(xpon);

	/* OMCI/OAM netdevs persist across mode changes; carrier reflects line state. */
	rtnl_lock();
	ret = airoha_xpon_create_control_rtnl(xpon);
	rtnl_unlock();
	if (ret)
		return dev_err_probe(
			dev, ret, "failed to register PON control interface\n");
	ret = devm_add_action_or_reset(dev, airoha_xpon_destroy_control, xpon);
	if (ret)
		return ret;

	/* DTS separates the MAC IRQ from the PCS/PMA IRQ. */
	xpon->mac_irq = platform_get_irq_byname(pdev, "mac");
	if (xpon->mac_irq < 0)
		return dev_err_probe(dev, xpon->mac_irq,
		                     "failed to obtain the PON MAC IRQ\n");
	xpon->phy_irq = platform_get_irq_byname(pdev, "phy");
	if (xpon->phy_irq < 0)
		return dev_err_probe(dev, xpon->phy_irq,
		                     "failed to obtain the PON PHY IRQ\n");

	/*
	 * Clear bootloader IRQ state before enabling either MAC. The EPON hard IRQ
	 * meets Discovery Register Request timing; IRQF_ONESHOT threads consume
	 * other W1C events in order.
	 */
	airoha_xpon_xgpon_mask_irqs(xpon);
	airoha_xpon_epon_mask_irqs(xpon);
	ret = devm_request_threaded_irq(dev, xpon->mac_irq, airoha_xpon_irq,
	                                airoha_xpon_irq_thread, IRQF_ONESHOT,
	                                dev_name(dev), xpon);
	if (ret)
		return dev_err_probe(dev, ret,
		                     "failed to register the PON MAC IRQ\n");

	/*
	 * Digital PON PHY IRQ status is W1C. start_work enables RX events after
	 * frontend and PCS preparation.
	 */
	ret = devm_request_threaded_irq(dev, xpon->phy_irq, NULL,
	                                airoha_xpon_phy_irq_thread,
	                                IRQF_ONESHOT, "airoha-xpon-phy", xpon);
	if (ret)
		return dev_err_probe(dev, ret,
		                     "failed to register the PON PHY IRQ\n");

	platform_set_drvdata(pdev, xpon);
	ret = airoha_eth_pon_register_controller(xpon->pon_netdev,
	                                         &airoha_xpon_link_ops, xpon);
	if (ret)
		return dev_err_probe(
			dev, ret,
			"failed to bind the PON controller to the GDM2 netdev\n");
	airoha_xpon_netlink_register(xpon);
	airoha_xpon_debugfs_init(xpon);
	dev_info(dev,
	         "PON resources bound to %s; mode=%s, MAC irq=%d, PHY irq=%d\n",
	         xpon->pon_netdev->name,
	         airoha_xpon_mode_name(xpon->configured_mode), xpon->mac_irq,
	         xpon->phy_irq);

	return 0;
}

static void airoha_xpon_remove(struct platform_device *pdev)
{
	struct airoha_xpon *xpon = platform_get_drvdata(pdev);

	/* Stop drains work and closes BEN before devm releases IRQ/MMIO. */
	airoha_xpon_debugfs_remove(xpon);
	airoha_xpon_netlink_unregister(xpon);
	airoha_eth_pon_unregister_controller(xpon->pon_netdev, xpon);
	airoha_xpon_controller_stop(xpon);
}

static const struct of_device_id airoha_xpon_of_match[] = {
	{ .compatible = "airoha,an7581-xpon-mac" },
	/*
	 * AN7583 shares the AN7581 XG-PON MAC and digital PHY register layout.
	 * Its DTB supplies the IRQ mapping and SoC-specific PCS provider.
	 */
	{ .compatible = "airoha,an7583-xpon-mac" },
	{}
};
MODULE_DEVICE_TABLE(of, airoha_xpon_of_match);

static struct platform_driver airoha_xpon_driver = {
	.probe = airoha_xpon_probe,
	.remove = airoha_xpon_remove,
	.driver = {
		.name = "airoha-xpon",
		.of_match_table = airoha_xpon_of_match,
		.dev_groups = airoha_xpon_groups,
	},
};
static int __init airoha_xpon_init(void)
{
	int ret;

	ret = airoha_xpon_netlink_init();
	if (ret)
		return ret;
	ret = platform_driver_register(&airoha_xpon_driver);
	if (ret)
		airoha_xpon_netlink_exit();
	return ret;
}

static void __exit airoha_xpon_exit(void)
{
	platform_driver_unregister(&airoha_xpon_driver);
	airoha_xpon_netlink_exit();
}

module_init(airoha_xpon_init);
module_exit(airoha_xpon_exit);

MODULE_AUTHOR("pbs05 <27010143+pbs05@users.noreply.github.com>");
MODULE_DESCRIPTION("Airoha AN7581/AN7583 xPON MAC driver");
MODULE_LICENSE("GPL");
