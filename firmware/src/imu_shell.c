/* LSM6DSV16X accelerometer diagnostics.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/shell/shell.h>

static int cmd_imu(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(lsm6dsv16x));
	struct sensor_value xyz[3];

	if (!device_is_ready(dev)) {
		shell_error(sh, "IMU device not ready");
		return -ENODEV;
	}
	int rc = sensor_sample_fetch_chan(dev, SENSOR_CHAN_ACCEL_XYZ);
	if (rc == 0) {
		rc = sensor_channel_get(dev, SENSOR_CHAN_ACCEL_XYZ, xyz);
	}
	if (rc != 0) {
		shell_error(sh, "IMU read failed: %d", rc);
		return rc;
	}
	shell_print(sh, "accel: x=%d y=%d z=%d mg",
		    sensor_ms2_to_ug(&xyz[0]) / 1000,
		    sensor_ms2_to_ug(&xyz[1]) / 1000,
		    sensor_ms2_to_ug(&xyz[2]) / 1000);
	return 0;
}

SHELL_CMD_REGISTER(imu, NULL, "Read accelerometer XYZ in milli-g", cmd_imu);
