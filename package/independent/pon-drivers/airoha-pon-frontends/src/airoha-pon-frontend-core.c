// SPDX-License-Identifier: GPL-2.0-only

#include <airoha-pon-frontend.h>

#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/string.h>

static LIST_HEAD(airoha_pon_frontends);
static DEFINE_MUTEX(airoha_pon_frontends_lock);

void airoha_pon_frontend_init(struct airoha_pon_frontend *frontend,
                              struct device *dev, struct module *owner,
                              const struct airoha_pon_frontend_ops *ops,
                              void *priv, u32 supported_modes)
{
	frontend->dev = dev;
	frontend->owner = owner;
	frontend->ops = ops;
	frontend->priv = priv;
	frontend->supported_modes = supported_modes;
	INIT_LIST_HEAD(&frontend->node);
	frontend->registered = false;
}
EXPORT_SYMBOL_GPL(airoha_pon_frontend_init);

int airoha_pon_frontend_register(struct airoha_pon_frontend *frontend)
{
	struct airoha_pon_frontend *registered;
	int ret = 0;

	if (!frontend || !frontend->dev || !frontend->dev->of_node ||
	    !frontend->owner || !frontend->ops || !frontend->ops->prepare ||
	    !frontend->ops->unprepare || !frontend->ops->get_link_config ||
	    !frontend->ops->set_tx_enable)
		return -EINVAL;

	mutex_lock(&airoha_pon_frontends_lock);
	list_for_each_entry(registered, &airoha_pon_frontends, node) {
		if (registered->dev->of_node == frontend->dev->of_node) {
			ret = -EEXIST;
			goto out_unlock;
		}
	}

	list_add_tail(&frontend->node, &airoha_pon_frontends);
	frontend->registered = true;

out_unlock:
	mutex_unlock(&airoha_pon_frontends_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(airoha_pon_frontend_register);

void airoha_pon_frontend_unregister(struct airoha_pon_frontend *frontend)
{
	mutex_lock(&airoha_pon_frontends_lock);
	if (frontend->registered) {
		list_del_init(&frontend->node);
		frontend->registered = false;
	}
	mutex_unlock(&airoha_pon_frontends_lock);
}
EXPORT_SYMBOL_GPL(airoha_pon_frontend_unregister);

struct airoha_pon_frontend *airoha_pon_frontend_get(struct device *consumer,
                                                    const char *phandle_name)
{
	struct airoha_pon_frontend *frontend;
	struct device_node *node;
	struct device_link *link;

	node = of_parse_phandle(consumer->of_node, phandle_name, 0);
	if (!node)
		return ERR_PTR(-ENODEV);

	mutex_lock(&airoha_pon_frontends_lock);
	list_for_each_entry(frontend, &airoha_pon_frontends, node) {
		if (frontend->dev->of_node != node)
			continue;
		if (!try_module_get(frontend->owner)) {
			frontend = NULL;
			break;
		}
		get_device(frontend->dev);
		goto found;
	}
	frontend = NULL;

found:
	mutex_unlock(&airoha_pon_frontends_lock);
	of_node_put(node);
	if (!frontend)
		return ERR_PTR(-EPROBE_DEFER);

	/* The device link orders consumer removal before provider removal. */
	link = device_link_add(consumer, frontend->dev,
	                       DL_FLAG_AUTOREMOVE_CONSUMER);
	if (!link) {
		module_put(frontend->owner);
		put_device(frontend->dev);
		return ERR_PTR(-ENOMEM);
	}

	return frontend;
}
EXPORT_SYMBOL_GPL(airoha_pon_frontend_get);

void airoha_pon_frontend_put(struct airoha_pon_frontend *frontend)
{
	module_put(frontend->owner);
	put_device(frontend->dev);
}
EXPORT_SYMBOL_GPL(airoha_pon_frontend_put);

void *airoha_pon_frontend_priv(struct airoha_pon_frontend *frontend)
{
	return frontend->priv;
}
EXPORT_SYMBOL_GPL(airoha_pon_frontend_priv);

struct device *airoha_pon_frontend_device(struct airoha_pon_frontend *frontend)
{
	return frontend->dev;
}
EXPORT_SYMBOL_GPL(airoha_pon_frontend_device);

bool airoha_pon_frontend_supports(struct airoha_pon_frontend *frontend,
                                  enum airoha_pon_frontend_mode mode)
{
	return mode >= AIROHA_PON_FRONTEND_MODE_GPON &&
	       mode <= AIROHA_PON_FRONTEND_MODE_EPON_10G_10G &&
	       frontend->supported_modes & AIROHA_PON_FRONTEND_MODE_BIT(mode);
}
EXPORT_SYMBOL_GPL(airoha_pon_frontend_supports);

int airoha_pon_frontend_prepare(struct airoha_pon_frontend *frontend,
                                enum airoha_pon_frontend_mode mode)
{
	if (!airoha_pon_frontend_supports(frontend, mode))
		return -EOPNOTSUPP;

	return frontend->ops->prepare(frontend, mode);
}
EXPORT_SYMBOL_GPL(airoha_pon_frontend_prepare);

int airoha_pon_frontend_unprepare(struct airoha_pon_frontend *frontend)
{
	return frontend->ops->unprepare(frontend);
}
EXPORT_SYMBOL_GPL(airoha_pon_frontend_unprepare);

int airoha_pon_frontend_get_signal_status(
	struct airoha_pon_frontend *frontend,
	struct airoha_pon_frontend_signal_status *status)
{
	memset(status, 0, sizeof(*status));
	if (!frontend->ops->get_signal_status)
		return 0;

	return frontend->ops->get_signal_status(frontend, status);
}
EXPORT_SYMBOL_GPL(airoha_pon_frontend_get_signal_status);

int airoha_pon_frontend_get_diagnostics(
	struct airoha_pon_frontend *frontend,
	struct airoha_pon_frontend_diagnostics *diagnostics)
{
	memset(diagnostics, 0, sizeof(*diagnostics));
	diagnostics->calibration = AIROHA_PON_FRONTEND_CALIBRATION_UNKNOWN;
	if (!frontend->ops->get_diagnostics)
		return -EOPNOTSUPP;

	return frontend->ops->get_diagnostics(frontend, diagnostics);
}
EXPORT_SYMBOL_GPL(airoha_pon_frontend_get_diagnostics);

int airoha_pon_frontend_get_link_config(
	struct airoha_pon_frontend *frontend,
	enum airoha_pon_frontend_mode mode,
	struct airoha_pon_frontend_link_config *config)
{
	if (!airoha_pon_frontend_supports(frontend, mode))
		return -EOPNOTSUPP;

	memset(config, 0, sizeof(*config));
	return frontend->ops->get_link_config(frontend, mode, config);
}
EXPORT_SYMBOL_GPL(airoha_pon_frontend_get_link_config);

int airoha_pon_frontend_set_tx_enable(struct airoha_pon_frontend *frontend,
                                      bool enable)
{
	return frontend->ops->set_tx_enable(frontend, enable);
}
EXPORT_SYMBOL_GPL(airoha_pon_frontend_set_tx_enable);

MODULE_AUTHOR("pbs05 <27010143+pbs05@users.noreply.github.com>");
MODULE_DESCRIPTION("Airoha PON frontend provider core");
MODULE_LICENSE("GPL");
