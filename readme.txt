static void flush_slave_dev_sl_info(struct ubcore_device *slave_dev,
	struct ubcore_device_cap *master)
{
	int ret = 0;

	(void)memcpy(slave->attr.dev_cap.priority_info, master->priority_info,
		    sizeof(struct ubcore_sl_info) * UBCORE_MAX_PRIORITY_CNT);

	for (int i = 0; i < UBCORE_MAX_PRIORITY_CNT; i++) {
		ret = dev->ops->set_sl(slave_dev, i, master->priority_info[i].SL);
		if (ret != 0)
			ubcore_log_err("dev: %s pri: %d set sl: %u failed!\n",
			slave_dev->dev_name, i, master->priority_info[i].SL);
	}
}

static int init_ubagg_dev(struct ubagg_device *ubagg_dev,
			  struct ubagg_add_dev_by_uvs *arg)

flush_slave_dev_sl_info(slave_dev, &master_dev->attr.dev_cap);
