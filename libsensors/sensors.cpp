/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define ALOG_TAG "Sensors"

#include <android/api-level.h>
#include <hardware/sensors.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <cstring>

#include <linux/input.h>

#include <utils/Atomic.h>
#include <utils/Log.h>

#include "sensors.h"

#include "LightSensor.h"
#include "ProximitySensor.h"
#include "AkmSensor.h"
#include "GyroSensor.h"

/*****************************************************************************/

#define DELAY_OUT_TIME 0x7FFFFFFF

#define LIGHT_SENSOR_POLLTIME    2000000000


#define SENSORS_ACCELERATION     (1<<ID_A)
#define SENSORS_MAGNETIC_FIELD   (1<<ID_M)
#define SENSORS_ORIENTATION      (1<<ID_O)
#define SENSORS_LIGHT            (1<<ID_L)
#define SENSORS_PROXIMITY        (1<<ID_P)
#define SENSORS_GYROSCOPE        (1<<ID_GY)

#define SENSORS_ACCELERATION_HANDLE       0
#define SENSORS_MAGNETIC_FIELD_HANDLE     1
#define SENSORS_ORIENTATION_HANDLE        2
#define SENSORS_LIGHT_HANDLE              3
#define SENSORS_PROXIMITY_HANDLE          4
#define SENSORS_GYROSCOPE_HANDLE          5
#define SENSORS_SIGNIFICANT_MOTION_HANDLE 6

#define AKM_FTRACE 0
#define AKM_DEBUG 0
#define AKM_DATA 0

#define DEBUG 0

/*****************************************************************************/

/* The SENSORS Module */
static const struct sensor_t sSensorList[] = {
        { "KR3DM 3-axis Accelerometer",
          "STMicroelectronics",
          1, SENSORS_ACCELERATION_HANDLE,
          SENSOR_TYPE_ACCELEROMETER, RANGE_A, CONVERT_A, 0.23f, 20000, 0, 0,
          "", "", 0, 0, {0, 0},},
        { "AK8975 3-axis Magnetic field sensor",
          "Asahi Kasei Microdevices",
          1, SENSORS_MAGNETIC_FIELD_HANDLE,
          SENSOR_TYPE_MAGNETIC_FIELD, 2000.0f, CONVERT_M, 6.8f, 16667, 0, 0,
          "", "", 0, 0, {0, 0}},
        { "AK8973 Orientation sensor",
          "Asahi Kasei Microdevices",
          1, SENSORS_ORIENTATION_HANDLE,
          SENSOR_TYPE_ORIENTATION, 360.0f, CONVERT_O, 7.8f, 16667, 0, 0,
          "", "", 0, 0, {0, 0}},
        { "CM3663 Light sensor",
          "Capella Microsystems",
          1, SENSORS_LIGHT_HANDLE,
          SENSOR_TYPE_LIGHT, 10240.0f, 1.0f, 0.75f, 0, 0, 0,
          "", "", 0, 0, {0, 0},},
        { "CM3663 Proximity sensor",
          "Capella Microsystems",
          1, SENSORS_PROXIMITY_HANDLE,
          SENSOR_TYPE_PROXIMITY, 5.0f, 5.0f, 0.75f, 0, 0, 0,
          "", "", 0, SENSOR_FLAG_WAKE_UP | SENSOR_FLAG_ON_CHANGE_MODE, {0, 0},},
        { "K3G Gyroscope sensor",
          "STMicroelectronics",
          1, SENSORS_GYROSCOPE_HANDLE,
          SENSOR_TYPE_GYROSCOPE, RANGE_GYRO, CONVERT_GYRO, 6.1f, 1190, 0, 0,
          "", "", 0, 0, {0, 0},},
        { "Significant motion sensor (Accelerometer)",
          "STMicroelectronics",
          1, SENSORS_SIGNIFICANT_MOTION_HANDLE,
          SENSOR_TYPE_SIGNIFICANT_MOTION, 1.0f, 1.0f, 0.23f, 0, 0, 0,
          "", "", 0, SENSOR_FLAG_WAKE_UP, {0, 0},},
};


static int open_sensors(const struct hw_module_t* module, const char* id,
                        struct hw_device_t** device);

static int sensors__get_sensors_list(struct sensors_module_t* module,
                                     struct sensor_t const** list) 
{
        *list = sSensorList;
        return ARRAY_SIZE(sSensorList);
}

static struct hw_module_methods_t sensors_module_methods = {
        open: open_sensors
};

struct sensors_module_t HAL_MODULE_INFO_SYM = {
        common: {
                tag: HARDWARE_MODULE_TAG,
                version_major: 1,
                version_minor: 0,
                id: SENSORS_HARDWARE_MODULE_ID,
                name: "Samsung Sensor module",
                author: "Samsung Electronic Company",
                methods: &sensors_module_methods,
        },
        get_sensors_list: sensors__get_sensors_list,
};

struct sensors_poll_context_t {
    sensors_poll_device_1_t device; // must be first

        sensors_poll_context_t();
        ~sensors_poll_context_t();
    int activate(int handle, int enabled);
    int setDelay(int handle, int64_t ns);
    int pollEvents(sensors_event_t* data, int count);
    int batch(int handle, int flags, int64_t period_ns, int64_t timeout);
    int flush(int handle);
private:
    enum {
        light           = 0,
        proximity       = 1,
        akm             = 2,
        gyro            = 3,
        numSensorDrivers,
        numFds,
    };

    static const size_t wake = numFds - 1;
    static const char WAKE_MESSAGE = 'W';
    struct pollfd mPollFds[numFds];
    int mWritePipeFd;
    SensorBase* mSensors[numSensorDrivers];

    int handleToDriver(int handle) const {
        switch (handle) {
            case ID_A:
            case ID_M:
            case ID_O:
            case ID_SM:
                return akm;
            case ID_P:
                return proximity;
            case ID_L:
                return light;
            case ID_GY:
                return gyro;
        }
        return -EINVAL;
    }
};

/*****************************************************************************/

sensors_poll_context_t::sensors_poll_context_t()
{
    mSensors[light] = new LightSensor();
    mPollFds[light].fd = mSensors[light]->getFd();
    mPollFds[light].events = POLLIN;
    mPollFds[light].revents = 0;

    mSensors[proximity] = new ProximitySensor();
    mPollFds[proximity].fd = mSensors[proximity]->getFd();
    mPollFds[proximity].events = POLLIN;
    mPollFds[proximity].revents = 0;

    mSensors[akm] = new AkmSensor();
    mPollFds[akm].fd = mSensors[akm]->getFd();
    mPollFds[akm].events = POLLIN;
    mPollFds[akm].revents = 0;

    mSensors[gyro] = new GyroSensor();
    mPollFds[gyro].fd = mSensors[gyro]->getFd();
    mPollFds[gyro].events = POLLIN;
    mPollFds[gyro].revents = 0;

    int wakeFds[2];
    int result = pipe(wakeFds);
    ALOGE_IF(result<0, "error creating wake pipe (%s)", strerror(errno));
    fcntl(wakeFds[0], F_SETFL, O_NONBLOCK);
    fcntl(wakeFds[1], F_SETFL, O_NONBLOCK);
    mWritePipeFd = wakeFds[1];

    mPollFds[wake].fd = wakeFds[0];
    mPollFds[wake].events = POLLIN;
    mPollFds[wake].revents = 0;
}

sensors_poll_context_t::~sensors_poll_context_t() {
    for (int i=0 ; i<numSensorDrivers ; i++) {
        delete mSensors[i];
    }
    close(mPollFds[wake].fd);
    close(mWritePipeFd);
}

int sensors_poll_context_t::activate(int handle, int enabled) {
    int index = handleToDriver(handle);
    if (index < 0) return index;
    if (index == gyro && enabled == 0) {
        usleep(200*1000);
    }
    int err =  mSensors[index]->enable(handle, enabled);
    if (enabled && !err) {
        const char wakeMessage(WAKE_MESSAGE);
        int result = write(mWritePipeFd, &wakeMessage, 1);
        ALOGE_IF(result<0, "error sending wake message (%s)", strerror(errno));
    }
    return err;
}

int sensors_poll_context_t::setDelay(int handle, int64_t ns) {

    int index = handleToDriver(handle);
    if (index < 0) return index;
    return mSensors[index]->setDelay(handle, ns);
}

int sensors_poll_context_t::pollEvents(sensors_event_t* data, int count)
{
    int nbEvents = 0;
    int n = 0;

    do {
        // see if we have some leftover from the last poll()
        for (int i=0 ; count && i<numSensorDrivers ; i++) {
            SensorBase* const sensor(mSensors[i]);
            if ((mPollFds[i].revents & POLLIN) || (sensor->hasPendingEvents())) {
                int nb = sensor->readEvents(data, count);
                if (nb < count) {
                    // no more data for this sensor
                    mPollFds[i].revents = 0;
                }
                count -= nb;
                nbEvents += nb;
                data += nb;
            }
        }
        switch (data->type) {
            case SENSOR_TYPE_ACCELEROMETER:
                ALOGD_IF(DEBUG, "Sensors: Accl x:%f y:%f z:%f",
                    data->acceleration.x,
                    data->acceleration.y,
                    data->acceleration.z);
                break;
            case SENSOR_TYPE_MAGNETIC_FIELD:
                ALOGD_IF(DEBUG, "Sensors: Magn x:%f y:%f z:%f",
                    data->magnetic.x,
                    data->magnetic.y,
                    data->magnetic.z);
                break;
            case SENSOR_TYPE_ORIENTATION:
                ALOGD_IF(DEBUG, "Sensors: Orie x:%f y:%f z:%f",
                    data->orientation.x,
                    data->orientation.y,
                    data->orientation.z);
                break;
            case SENSOR_TYPE_GYROSCOPE:
                ALOGD_IF(DEBUG, "Sensors: Gyro x:%f y:%f z:%f",
                    data->gyro.x,
                    data->gyro.y,
                    data->gyro.z);
                break;
            default:
                break;
        }
        if (count) {
            // we still have some room, so try to see if we can get
            // some events immediately or just wait if we don't have
            // anything to return
            n = poll(mPollFds, numFds, nbEvents ? 0 : -1);
            if (n<0) {
                ALOGE("poll() failed (%s)", strerror(errno));
                return nbEvents;
            }
            if (mPollFds[wake].revents & POLLIN) {
                char msg;
                int result = read(mPollFds[wake].fd, &msg, 1);
                ALOGE_IF(result<0, "error reading from wake pipe (%s)", strerror(errno));
                ALOGE_IF(msg != WAKE_MESSAGE, "unknown message on wake queue (0x%02x)", int(msg));
                mPollFds[wake].revents = 0;
            }
        }
        // if we have events and space, go read them
    } while (n && count);

    return nbEvents;
}
int sensors_poll_context_t::batch(int handle, int flags, int64_t period_ns, int64_t timeout) {
    int index = handleToDriver(handle);
    if (index < 0) return index;

    return mSensors[index]->batch(handle, flags, period_ns, timeout);
}

int sensors_poll_context_t::flush(int handle) {
    int index = handleToDriver(handle);
    if (index < 0) return index;

    return mSensors[index]->flush(handle);
}
/*****************************************************************************/

static int device__close(struct hw_device_t *dev)
{
    sensors_poll_context_t *ctx = (sensors_poll_context_t *)dev;
    if (ctx) {
        delete ctx;
    }
    return 0;
}

static int device__activate(sensors_poll_device_t *dev,
        int handle, int enabled) {
    sensors_poll_context_t *ctx = (sensors_poll_context_t *)dev;
    return ctx->activate(handle, enabled);
}

static int device__setDelay(sensors_poll_device_t *dev,
        int handle, int64_t ns) {
    sensors_poll_context_t *ctx = (sensors_poll_context_t *)dev;
    return ctx->setDelay(handle, ns);
}

static int device__poll(sensors_poll_device_t *dev,
        sensors_event_t* data, int count) {
    sensors_poll_context_t *ctx = (sensors_poll_context_t *)dev;
    return ctx->pollEvents(data, count);
}

static int device__batch(struct sensors_poll_device_1 *dev, int handle,
        int flags, int64_t period_ns, int64_t timeout) {
    sensors_poll_context_t* ctx = (sensors_poll_context_t*) dev;
    return ctx->batch(handle, flags, period_ns, timeout);
}

static int device__flush(struct sensors_poll_device_1 *dev, int handle) {
    sensors_poll_context_t* ctx = (sensors_poll_context_t*) dev;
    return ctx->flush(handle);
}

/*****************************************************************************/
extern "C" {
extern uint32_t android_get_application_target_sdk_version();
extern void android_set_application_target_sdk_version(uint32_t target);
}

/** Open a new instance of a sensor device using name */
static int open_sensors(const struct hw_module_t* module, const char* id,
                        struct hw_device_t** device)
{
        android_set_application_target_sdk_version(__ANDROID_API_L_MR1__);
        int status = -EINVAL;
        sensors_poll_context_t *dev = new sensors_poll_context_t();

        memset(&dev->device, 0, sizeof(sensors_poll_device_1_t));

        dev->device.common.tag      = HARDWARE_DEVICE_TAG;
        dev->device.common.version  = SENSORS_DEVICE_API_VERSION_1_3;
        dev->device.common.module   = const_cast<hw_module_t*>(module);
        dev->device.common.close    = device__close;
        dev->device.activate        = device__activate;
        dev->device.setDelay        = device__setDelay;
        dev->device.poll            = device__poll;
        dev->device.batch           = device__batch;
        dev->device.flush           = device__flush;

        *device = &dev->device.common;
        status = 0;

        return status;
}

