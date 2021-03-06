#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>
#include "wlr/interfaces/wlr_keyboard.h"
#include "wlr/types/wlr_keyboard.h"
#include "wlr/types/wlr_keyboard_group.h"
#include "wlr/util/log.h"

struct keyboard_group_device {
	struct wlr_keyboard *keyboard;
	struct wl_listener key;
	struct wl_listener modifiers;
	struct wl_listener keymap;
	struct wl_listener repeat_info;
	struct wl_listener destroy;
	struct wl_list link; // wlr_keyboard_group::devices
};

struct keyboard_group_key {
	uint32_t keycode;
	size_t count;
	struct wl_list link; // wlr_keyboard_group::keys
};

static void keyboard_set_leds(struct wlr_keyboard *kb, uint32_t leds) {
	struct wlr_keyboard_group *group = wlr_keyboard_group_from_wlr_keyboard(kb);
	struct keyboard_group_device *device;
	wl_list_for_each(device, &group->devices, link) {
		wlr_keyboard_led_update(device->keyboard, leds);
	}
}

static void keyboard_destroy(struct wlr_keyboard *kb) {
	// Just remove the event listeners. The keyboard will be freed as part of
	// the wlr_keyboard_group in wlr_keyboard_group_destroy.
	wl_list_remove(&kb->events.key.listener_list);
	wl_list_remove(&kb->events.modifiers.listener_list);
	wl_list_remove(&kb->events.keymap.listener_list);
	wl_list_remove(&kb->events.repeat_info.listener_list);
	wl_list_remove(&kb->events.destroy.listener_list);
}

static const struct wlr_keyboard_impl impl = {
	.destroy = keyboard_destroy,
	.led_update = keyboard_set_leds
};

struct wlr_keyboard_group *wlr_keyboard_group_create(void) {
	struct wlr_keyboard_group *group =
		calloc(1, sizeof(struct wlr_keyboard_group));
	if (!group) {
		wlr_log(WLR_ERROR, "Failed to allocate wlr_keyboard_group");
		return NULL;
	}

	group->input_device = calloc(1, sizeof(struct wlr_input_device));
	if (!group->input_device) {
		wlr_log(WLR_ERROR, "Failed to allocate wlr_input_device for group");
		free(group);
		return NULL;
	}
	wl_signal_init(&group->input_device->events.destroy);
	group->input_device->keyboard = &group->keyboard;

	wlr_keyboard_init(&group->keyboard, &impl);
	wl_list_init(&group->devices);
	wl_list_init(&group->keys);

	return group;
}

struct wlr_keyboard_group *wlr_keyboard_group_from_wlr_keyboard(
		struct wlr_keyboard *keyboard) {
	if (keyboard->impl != &impl) {
		return NULL;
	}
	return (struct wlr_keyboard_group *)keyboard;
}

static bool keymaps_match(struct xkb_keymap *km1, struct xkb_keymap *km2) {
	if (!km1 && !km2) {
		return true;
	}
	if (!km1 || !km2) {
		return false;
	}
	char *km1_str = xkb_keymap_get_as_string(km1, XKB_KEYMAP_FORMAT_TEXT_V1);
	char *km2_str = xkb_keymap_get_as_string(km2, XKB_KEYMAP_FORMAT_TEXT_V1);
	bool result = strcmp(km1_str, km2_str) == 0;
	free(km1_str);
	free(km2_str);
	return result;
}

static void handle_keyboard_key(struct wl_listener *listener, void *data) {
	struct keyboard_group_device *group_device =
		wl_container_of(listener, group_device, key);
	struct wlr_keyboard_group *group = group_device->keyboard->group;
	struct wlr_event_keyboard_key *event = data;

	struct keyboard_group_key *key, *tmp;
	wl_list_for_each_safe(key, tmp, &group->keys, link) {
		if (key->keycode != event->keycode) {
			continue;
		}
		if (event->state == WLR_KEY_PRESSED) {
			key->count++;
			return;
		}
		if (event->state == WLR_KEY_RELEASED) {
			key->count--;
			if (key->count > 0) {
				return;
			}
			wl_list_remove(&key->link);
			free(key);
		}
		break;
	}

	if (event->state == WLR_KEY_PRESSED) {
		struct keyboard_group_key *key =
			calloc(1, sizeof(struct keyboard_group_key));
		if (!key) {
			wlr_log(WLR_ERROR, "Failed to allocate keyboard_group_key");
			return;
		}
		key->keycode = event->keycode;
		key->count = 1;
		wl_list_insert(&group->keys, &key->link);
	}

	wlr_keyboard_notify_key(&group_device->keyboard->group->keyboard, data);
}

static void handle_keyboard_modifiers(struct wl_listener *listener,
		void *data) {
	// Sync the effective layout (group modifier) to all keyboards. The rest of
	// the modifiers will be derived from the wlr_keyboard_group's key state
	struct keyboard_group_device *group_device =
		wl_container_of(listener, group_device, modifiers);
	struct wlr_keyboard_modifiers mods = group_device->keyboard->modifiers;

	struct keyboard_group_device *device;
	wl_list_for_each(device, &group_device->keyboard->group->devices, link) {
		if (mods.depressed != device->keyboard->modifiers.depressed ||
				mods.latched != device->keyboard->modifiers.latched ||
				mods.locked != device->keyboard->modifiers.locked ||
				mods.group != device->keyboard->modifiers.group) {
			wlr_keyboard_notify_modifiers(device->keyboard,
					mods.depressed, mods.latched, mods.locked, mods.group);
			return;
		}
	}

	wlr_keyboard_notify_modifiers(&group_device->keyboard->group->keyboard,
			mods.depressed, mods.latched, mods.locked, mods.group);
}

static void handle_keyboard_keymap(struct wl_listener *listener, void *data) {
	struct keyboard_group_device *group_device =
		wl_container_of(listener, group_device, keymap);
	struct wlr_keyboard *keyboard = group_device->keyboard;

	if (!keymaps_match(keyboard->group->keyboard.keymap, keyboard->keymap)) {
		struct keyboard_group_device *device;
		wl_list_for_each(device, &keyboard->group->devices, link) {
			if (!keymaps_match(keyboard->keymap, device->keyboard->keymap)) {
				wlr_keyboard_set_keymap(device->keyboard, keyboard->keymap);
				return;
			}
		}
	}

	wlr_keyboard_set_keymap(&keyboard->group->keyboard, keyboard->keymap);
}

static void handle_keyboard_repeat_info(struct wl_listener *listener,
		void *data) {
	struct keyboard_group_device *group_device =
		wl_container_of(listener, group_device, repeat_info);
	struct wlr_keyboard *keyboard = group_device->keyboard;

	struct keyboard_group_device *device;
	wl_list_for_each(device, &keyboard->group->devices, link) {
		struct wlr_keyboard *devkb = device->keyboard;
		if (devkb->repeat_info.rate != keyboard->repeat_info.rate ||
				devkb->repeat_info.delay != keyboard->repeat_info.delay) {
			wlr_keyboard_set_repeat_info(devkb, keyboard->repeat_info.rate,
					keyboard->repeat_info.delay);
			return;
		}
	}

	wlr_keyboard_set_repeat_info(&keyboard->group->keyboard,
			keyboard->repeat_info.rate, keyboard->repeat_info.delay);
}

static void refresh_state(struct keyboard_group_device *device,
		enum wlr_key_state state) {
	for (size_t i = 0; i < device->keyboard->num_keycodes; i++) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		struct wlr_event_keyboard_key event = {
			.time_msec = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000,
			.keycode = device->keyboard->keycodes[i],
			.update_state = true,
			.state = state
		};
		handle_keyboard_key(&device->key, &event);
	}
}

static void remove_keyboard_group_device(struct keyboard_group_device *device) {
	refresh_state(device, WLR_KEY_RELEASED);
	device->keyboard->group = NULL;
	wl_list_remove(&device->link);
	wl_list_remove(&device->key.link);
	wl_list_remove(&device->modifiers.link);
	wl_list_remove(&device->keymap.link);
	wl_list_remove(&device->repeat_info.link);
	wl_list_remove(&device->destroy.link);
	free(device);
}

static void handle_keyboard_destroy(struct wl_listener *listener, void *data) {
	struct keyboard_group_device *device =
		wl_container_of(listener, device, destroy);
	remove_keyboard_group_device(device);
}

bool wlr_keyboard_group_add_keyboard(struct wlr_keyboard_group *group,
		struct wlr_keyboard *keyboard) {
	if (keyboard->group) {
		wlr_log(WLR_ERROR, "A wlr_keyboard can only belong to one group");
		return false;
	}

	if (keyboard->impl == &impl) {
		wlr_log(WLR_ERROR, "Cannot add a group's keyboard to a group");
		return false;
	}

	if (!keymaps_match(group->keyboard.keymap, keyboard->keymap)) {
		wlr_log(WLR_ERROR, "Device keymap does not match keyboard group's");
		return false;
	}

	struct keyboard_group_device *device =
		calloc(1, sizeof(struct keyboard_group_device));
	if (!device) {
		wlr_log(WLR_ERROR, "Failed to allocate keyboard_group_device");
		return false;
	}

	device->keyboard = keyboard;
	keyboard->group = group;
	wl_list_insert(&group->devices, &device->link);

	wl_signal_add(&keyboard->events.key, &device->key);
	device->key.notify = handle_keyboard_key;

	wl_signal_add(&keyboard->events.modifiers, &device->modifiers);
	device->modifiers.notify = handle_keyboard_modifiers;

	wl_signal_add(&keyboard->events.keymap, &device->keymap);
	device->keymap.notify = handle_keyboard_keymap;

	wl_signal_add(&keyboard->events.repeat_info, &device->repeat_info);
	device->repeat_info.notify = handle_keyboard_repeat_info;

	wl_signal_add(&keyboard->events.destroy, &device->destroy);
	device->destroy.notify = handle_keyboard_destroy;

	struct wlr_keyboard *group_kb = &group->keyboard;
	if (keyboard->modifiers.group != group_kb->modifiers.group) {
		wlr_keyboard_notify_modifiers(keyboard, keyboard->modifiers.depressed,
				keyboard->modifiers.latched, keyboard->modifiers.locked,
				group_kb->modifiers.group);
	}
	if (keyboard->repeat_info.rate != group_kb->repeat_info.rate ||
			keyboard->repeat_info.delay != group_kb->repeat_info.delay) {
		wlr_keyboard_set_repeat_info(keyboard, group_kb->repeat_info.rate,
				group_kb->repeat_info.delay);
	}

	refresh_state(device, WLR_KEY_PRESSED);
	return true;
}

void wlr_keyboard_group_remove_keyboard(struct wlr_keyboard_group *group,
		struct wlr_keyboard *keyboard) {
	struct keyboard_group_device *device, *tmp;
	wl_list_for_each_safe(device, tmp, &group->devices, link) {
		if (device->keyboard == keyboard) {
			remove_keyboard_group_device(device);
			return;
		}
	}
	wlr_log(WLR_ERROR, "keyboard not found in group");
}

void wlr_keyboard_group_destroy(struct wlr_keyboard_group *group) {
	struct keyboard_group_device *device, *tmp;
	wl_list_for_each_safe(device, tmp, &group->devices, link) {
		wlr_keyboard_group_remove_keyboard(group, device->keyboard);
	}
	wlr_keyboard_destroy(&group->keyboard);
	wl_list_remove(&group->input_device->events.destroy.listener_list);
	free(group->input_device);
	free(group);
}
