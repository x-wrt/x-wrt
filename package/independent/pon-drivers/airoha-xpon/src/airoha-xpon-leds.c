// SPDX-License-Identifier: GPL-2.0-only

#include <linux/device.h>
#include <linux/leds.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/slab.h>

#include "airoha-xpon-epon.h"
#include "airoha-xpon-private.h"

#define AIROHA_XPON_LED_FAST_BLINK_MS 200
#define AIROHA_XPON_LED_SLOW_BLINK_MS 500

enum airoha_xpon_led_state {
	AIROHA_XPON_LED_UNKNOWN = -1,
	AIROHA_XPON_LED_OFF,
	AIROHA_XPON_LED_FAST_BLINK,
	AIROHA_XPON_LED_SLOW_BLINK,
	AIROHA_XPON_LED_ON,
};

struct airoha_xpon_leds {
	struct device *dev;
	struct led_trigger *registration;
	struct led_trigger *los;
	struct led_trigger *online;
	struct mutex lock;
	char registration_name[IFNAMSIZ + sizeof("-registration")];
	char los_name[IFNAMSIZ + sizeof("-los")];
	char online_name[IFNAMSIZ + sizeof("-online")];
	enum airoha_xpon_led_state registration_state;
	enum airoha_xpon_led_state los_state;
	enum airoha_xpon_led_state online_state;
};

static void airoha_xpon_led_apply(struct led_trigger *trigger,
                                  enum airoha_xpon_led_state state)
{
	/* The LED core treats delay_off=0 as steady on and stops the blink timer. */
	switch (state) {
	case AIROHA_XPON_LED_FAST_BLINK:
		led_trigger_blink(trigger, AIROHA_XPON_LED_FAST_BLINK_MS,
		                  AIROHA_XPON_LED_FAST_BLINK_MS);
		break;
	case AIROHA_XPON_LED_SLOW_BLINK:
		led_trigger_blink(trigger, AIROHA_XPON_LED_SLOW_BLINK_MS,
		                  AIROHA_XPON_LED_SLOW_BLINK_MS);
		break;
	case AIROHA_XPON_LED_ON:
		led_trigger_blink(trigger, 1, 0);
		break;
	case AIROHA_XPON_LED_OFF:
	default:
		led_trigger_event(trigger, LED_OFF);
		break;
	}
}

struct airoha_xpon_leds *airoha_xpon_leds_create(struct device *dev,
                                                 const char *pon_name)
{
	struct airoha_xpon_leds *leds;

	/* Triggers consume line state independently of the PON control path. */
	if (!IS_ENABLED(CONFIG_LEDS_TRIGGERS))
		return NULL;

	leds = kzalloc(sizeof(*leds), GFP_KERNEL);
	if (!leds)
		return NULL;

	leds->dev = dev;
	mutex_init(&leds->lock);
	leds->registration_state = AIROHA_XPON_LED_UNKNOWN;
	leds->los_state = AIROHA_XPON_LED_UNKNOWN;
	leds->online_state = AIROHA_XPON_LED_UNKNOWN;
	snprintf(leds->registration_name, sizeof(leds->registration_name),
	         "%s-registration", pon_name);
	snprintf(leds->los_name, sizeof(leds->los_name), "%s-los", pon_name);
	snprintf(leds->online_name, sizeof(leds->online_name), "%s-online",
	         pon_name);

	led_trigger_register_simple(leds->registration_name,
	                            &leds->registration);
	led_trigger_register_simple(leds->los_name, &leds->los);
	led_trigger_register_simple(leds->online_name, &leds->online);

	if (!leds->registration && !leds->los && !leds->online) {
		mutex_destroy(&leds->lock);
		kfree(leds);
		return NULL;
	}

	dev_info(dev, "PON LED triggers registered: %s, %s, %s\n",
	         leds->registration_name, leds->los_name, leds->online_name);
	return leds;
}

void airoha_xpon_leds_destroy(void *data)
{
	struct airoha_xpon_leds *leds = data;

	if (!leds)
		return;

	/* Line teardown publishes OFF before unregistering trigger consumers. */
	led_trigger_unregister_simple(leds->online);
	led_trigger_unregister_simple(leds->los);
	led_trigger_unregister_simple(leds->registration);
	mutex_destroy(&leds->lock);
	kfree(leds);
}

void airoha_xpon_leds_update(struct airoha_xpon *xpon)
{
	struct airoha_xpon_leds *leds = xpon->leds;
	enum airoha_xpon_led_state registration, los, online;
	bool line_running, optical_signal;

	if (!leds)
		return;

	/* Optical loss uses a 500 ms blink period. */
	line_running = READ_ONCE(xpon->lifecycle) != AIROHA_XPON_STOPPED;
	optical_signal = READ_ONCE(xpon->optical_signal);
	los = line_running && !optical_signal ? AIROHA_XPON_LED_SLOW_BLINK :
	                                        AIROHA_XPON_LED_OFF;
	online = line_running && optical_signal &&
	                         READ_ONCE(xpon->service_ready) ?
	                 AIROHA_XPON_LED_ON :
	                 AIROHA_XPON_LED_OFF;

	if (!line_running || !optical_signal) {
		registration = AIROHA_XPON_LED_OFF;
	} else if (READ_ONCE(xpon->active_mode_valid) &&
	           airoha_xpon_mode_is_epon(READ_ONCE(xpon->active_mode))) {
		/* A valid LLID marks completed EPON MPCP registration. */
		registration = READ_ONCE(xpon->epon.mpcp_state) ==
		                               AIROHA_EPON_MPCP_REGISTERED ?
		                       AIROHA_XPON_LED_ON :
		                       AIROHA_XPON_LED_FAST_BLINK;
	} else {
		switch (READ_ONCE(xpon->onu_state)) {
		case AIROHA_XGPON_O5:
			registration = AIROHA_XPON_LED_ON;
			break;
		case AIROHA_XGPON_O4:
			registration = AIROHA_XPON_LED_SLOW_BLINK;
			break;
		case AIROHA_XGPON_O1:
		case AIROHA_XGPON_O2_3:
		default:
			registration = AIROHA_XPON_LED_FAST_BLINK;
			break;
		}
	}

	/* Keep the LED mutex outside the state_lock/ploam_lock hardware order. */
	mutex_lock(&leds->lock);
	if (leds->los_state != los) {
		leds->los_state = los;
		airoha_xpon_led_apply(leds->los, los);
	}
	if (leds->registration_state != registration) {
		leds->registration_state = registration;
		airoha_xpon_led_apply(leds->registration, registration);
	}
	if (leds->online_state != online) {
		leds->online_state = online;
		airoha_xpon_led_apply(leds->online, online);
	}
	mutex_unlock(&leds->lock);
}
