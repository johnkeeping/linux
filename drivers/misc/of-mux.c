/*
 * Copyright (C) 2016 InMusic Brands Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 */
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define PREFIX "state-"

struct mux_state {
	const char *name;
	struct device_node *tree;

	struct list_head list;
};

struct of_mux {
	struct device *dev;
	struct mutex lock;

	unsigned int switch_delay_ms;
	unsigned int post_switch_delay_ms;

	struct mux_state *state;
	int overlay_id;
	struct list_head head;
};

static int of_mux_set_state(struct of_mux *mux, const char *name)
{
	int ret;
	struct mux_state *state, *new_state = NULL;

	list_for_each_entry(state, &mux->head, list) {
		if (!strcmp(state->name, name)) {
			new_state = state;
			break;
		}
	}

	if (!new_state) {
		dev_err(mux->dev, "no such state: %s\n", name);
		return -EINVAL;
	}

	mutex_lock(&mux->lock);
	if (mux->state == new_state) {
		dev_dbg(mux->dev, "nothing to do, state remains %s\n",
			mux->state->name);
		ret = 0;
		goto unlock;
	}

	if (mux->overlay_id >= 0) {
		dev_dbg(mux->dev, "removing overlay %s\n", mux->state->name);
		ret = of_overlay_destroy(mux->overlay_id);
		if (ret < 0)
			goto unlock;
		mux->state = NULL;
	}

	if (mux->switch_delay_ms)
		msleep(mux->switch_delay_ms);

	dev_dbg(mux->dev, "adding overlay %s\n", new_state->name);
	mux->overlay_id = of_overlay_create(new_state->tree);
	if (mux->overlay_id < 0) {
		ret = mux->overlay_id;
		goto unlock;
	}

	mux->state = new_state;
	ret = 0;

	if (mux->post_switch_delay_ms)
		msleep(mux->post_switch_delay_ms);

unlock:
	mutex_unlock(&mux->lock);
	return ret;
}

static ssize_t available_states_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct of_mux *mux = dev_get_drvdata(dev);
	ssize_t i = 0;
	struct mux_state *state;

	list_for_each_entry(state, &mux->head, list) {
		size_t len = strlen(state->name);
		if (i >= ((ssize_t) (PAGE_SIZE - len - 3)))
			goto out;

		i += sprintf(&buf[i], "%s ", state->name);
	}

out:
	i += sprintf(&buf[i], "\n");
	return i;
}

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct of_mux *mux = dev_get_drvdata(dev);
	struct mux_state *state = READ_ONCE(mux->state);

	return scnprintf(buf, PAGE_SIZE, "%s\n", state ? state->name : "");
}

static ssize_t state_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct of_mux *mux = dev_get_drvdata(dev);
	int ret;
	char *name;
	size_t length = count;

	while (length && buf[length - 1] == '\n')
		length--;

	name = kstrndup(buf, length, GFP_KERNEL);
	if (!name)
		return -ENOMEM;

	ret = of_mux_set_state(mux, name);
	kfree(name);
	return ret < 0 ? ret : count;
}

static DEVICE_ATTR_RO(available_states);
static DEVICE_ATTR_RW(state);

static int of_mux_probe(struct platform_device *pdev)
{
	int ret;
	struct device_node *child;
	const char *default_state;
	size_t len = strlen(PREFIX);
	struct device *dev = &pdev->dev;
	struct of_mux *mux = devm_kzalloc(dev, sizeof(*mux),
					  GFP_KERNEL);
	if (!mux)
		return -ENOMEM;

	mux->dev = dev;
	mux->overlay_id = -1;
	mutex_init(&mux->lock);
	INIT_LIST_HEAD(&mux->head);
	for_each_child_of_node(dev->of_node, child) {
		struct mux_state *state;
		if (len > strlen(child->name) || strncmp(child->name, PREFIX, len))
			continue;

		state = devm_kzalloc(dev, sizeof(*state), GFP_KERNEL);
		if (!state) {
			of_node_put(child);
			return -ENOMEM;
		}

		state->name = devm_kstrdup(dev, child->name + len, GFP_KERNEL);
		if (!state->name) {
			of_node_put(child);
			return -ENOMEM;
		}

		dev_dbg(dev, "discovered state %s\n", state->name);

		/* state now owns the reference to child */
		state->tree = child;
		list_add_tail(&state->list, &mux->head);
	};

	if (list_empty(&mux->head)) {
		dev_err(dev, "no states found\n");
		return -EINVAL;
	}

	ret = device_create_file(dev, &dev_attr_state);
	if (ret < 0)
		return ret;
	ret = device_create_file(dev, &dev_attr_available_states);
	if (ret < 0) {
		device_remove_file(dev, &dev_attr_state);
		return ret;
	}

	/* If there is no default or we fail to set it, select the first state. */
	if (of_property_read_string(dev->of_node, "default-state", &default_state))
		default_state = list_first_entry(&mux->head, struct mux_state, list)->name;

	if (of_property_read_u32(dev->of_node, "switch-delay-ms",
				 &mux->switch_delay_ms))
		mux->switch_delay_ms = 0;

	if (of_property_read_u32(dev->of_node, "post-switch-delay-ms",
				 &mux->post_switch_delay_ms))
		mux->post_switch_delay_ms = 0;

	if (of_mux_set_state(mux, default_state) < 0)
		dev_err(dev, "failed to set default state %s\n", default_state);

	platform_set_drvdata(pdev, mux);
	return 0;
}

static int of_mux_remove(struct platform_device *pdev)
{
	struct mux_state *state;
	struct of_mux *mux = platform_get_drvdata(pdev);

	if (mux->overlay_id >= 0) {
		int ret = of_overlay_destroy(mux->overlay_id);
		if (ret < 0)
			return ret;
	}

	device_remove_file(&pdev->dev, &dev_attr_state);
	device_remove_file(&pdev->dev, &dev_attr_available_states);
	list_for_each_entry(state, &mux->head, list)
		of_node_put(state->tree);

	mutex_destroy(&mux->lock);

	return 0;
}

static const struct of_device_id of_mux_match[] = {
	{ .compatible = "virtual-mux", },
	{}
};
MODULE_DEVICE_TABLE(of, of_mux_match);

static struct platform_driver of_mux_driver = {
	.probe = of_mux_probe,
	.remove = of_mux_remove,
	.driver = {
		.name = "of-mux",
		.of_match_table = of_mux_match,
	},
};

module_platform_driver(of_mux_driver);

MODULE_AUTHOR("John Keeping <john@metanate.com>");
MODULE_DESCRIPTION("Device tree virtual multiplexer");
MODULE_LICENSE("GPL");
