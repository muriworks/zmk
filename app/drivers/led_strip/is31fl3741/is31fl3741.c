/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT issi_is31fl3741

#include <zephyr.h>
#include <device.h>
#include <drivers/gpio.h>
#include <drivers/i2c.h>
#include <drivers/led_strip.h>
#include <logging/log.h>
#include <sys/math_extras.h>
#include <sys/util.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define IS31FL3741_BUFFER_SIZE (39 * 9)
#define IS31FL3741_BUFFER_PAGE_BREAK (0xb4)

#define IS31FL3741_REG_PS (0xfd)
#define IS31FL3741_REG_PSWL (0xfe)

#define IS31FL3741_PSWL_ENABLE (0xc5)
#define IS31FL3741_PSWL_DISABLE (0x00)

#define IS31FL3741_PAGE_PWM_A (0x00)
#define IS31FL3741_PAGE_PWM_B (0x01)
#define IS31FL3741_PAGE_SCALING_A (0x02)
#define IS31FL3741_PAGE_SCALING_B (0x03)
#define IS31FL3741_PAGE_FUNCTION (0x04)

struct is31fl3741_config {
    char *bus;
    int reg;
    char *label;
    char *sdb_port;
    gpio_pin_t sdb_pin;
    gpio_dt_flags_t sdb_flags;
    size_t px_buffer_size;
    uint8_t gcc;
    uint8_t sws;
    uint16_t *rgb_map;
    uint8_t *gamma;
    uint8_t scaling_red;
    uint8_t scaling_blue;
    uint8_t scaling_green;
};

struct is31fl3741_data {
    const struct device *i2c;
    const struct device *gpio;
    uint8_t *px_buffer;
};

static int is31fl3741_reg_write(const struct device *dev, uint8_t addr, uint8_t value) {
    const struct is31fl3741_data *data = dev->data;
    const struct is31fl3741_config *config = dev->config;

    if (i2c_reg_write_byte(data->i2c, config->reg, addr, value)) {
        LOG_ERR("Failed writing value %x to register address %x on device %x.", value, addr,
                config->reg);
        return -EIO;
    }

    return 0;
}

static int is31fl3741_reg_burst_write(const struct device *dev, uint8_t start_addr,
                                      const uint8_t *buffer, size_t num_bytes) {
    const struct is31fl3741_data *data = dev->data;
    const struct is31fl3741_config *config = dev->config;

    if (i2c_burst_write(data->i2c, config->reg, start_addr, buffer, num_bytes)) {
        LOG_ERR("Failed burst write with starting address %x", start_addr);
        return -EIO;
    }

    return 0;
}

static int is31fl3741_set_page(const struct device *dev, uint8_t page_addr) {
    if (is31fl3741_reg_write(dev, IS31FL3741_REG_PSWL, IS31FL3741_PSWL_ENABLE)) {
        return -EIO;
    }

    if (is31fl3741_reg_write(dev, IS31FL3741_REG_PS, page_addr)) {
        return -EIO;
    }

    return 0;
}

static inline bool num_pixels_ok(const struct is31fl3741_config *config, size_t num_pixels) {
    size_t num_bytes;

    const bool overflow = size_mul_overflow(num_pixels, 3, &num_bytes);

    return !overflow && (num_bytes <= config->px_buffer_size);
}

/**
 * Updates individual LED channels without an RGB interpretation.
 */
static int is31fl3741_strip_update_channels(const struct device *dev, uint8_t *channels,
                                            size_t num_channels) {
    const struct is31fl3741_config *config = dev->config;

    if (config->px_buffer_size < num_channels) {
        return -ENOMEM;
    }

    is31fl3741_set_page(dev, IS31FL3741_PAGE_PWM_A);

    int result;

    result = is31fl3741_reg_burst_write(dev, 0x00, channels,
                                        (num_channels <= IS31FL3741_BUFFER_PAGE_BREAK)
                                            ? num_channels
                                            : IS31FL3741_BUFFER_PAGE_BREAK);

    if (result || num_channels <= IS31FL3741_BUFFER_PAGE_BREAK) {
        return result;
    }

    is31fl3741_set_page(dev, IS31FL3741_PAGE_PWM_B);

    return is31fl3741_reg_burst_write(dev, 0x00, channels + 0xb4, num_channels - 0xb4);
}

/*
 * Updates the RGB LED matrix according to devicetree's map property.
 */
static int is31fl3741_strip_update_rgb(const struct device *dev, struct led_rgb *pixels,
                                       size_t num_pixels) {
    const struct is31fl3741_config *config = dev->config;
    struct is31fl3741_data *data = dev->data;

    size_t i = 0;
    size_t j = 0;

    if (!num_pixels_ok(config, num_pixels)) {
        return -ENOMEM;
    }

    while (i < num_pixels) {
        data->px_buffer[config->rgb_map[j++]] = config->gamma[pixels[i].r];
        data->px_buffer[config->rgb_map[j++]] = config->gamma[pixels[i].g];
        data->px_buffer[config->rgb_map[j++]] = config->gamma[pixels[i].b];

        ++i;
    }

    return is31fl3741_strip_update_channels(dev, data->px_buffer, config->px_buffer_size);
}

/*
 * Initiates a driver instance for IS31FL3741.
 *
 * SDB is pulled high to enable chip operation followed
 * by a reset to clear out all previous values.
 *
 * Function and scaling registers are then pre-configured based on devicetree settings.
 */
int static is31fl3741_init(const struct device *dev) {
    const struct is31fl3741_config *config = dev->config;
    struct is31fl3741_data *data = dev->data;

    data->i2c = device_get_binding(config->bus);

    if (data->i2c == NULL) {
        LOG_ERR("I2C device %s not found", config->bus);
        return -ENODEV;
    }

    data->gpio = device_get_binding(config->sdb_port);

    if (data->gpio == NULL) {
        LOG_ERR("GPIO device %s not found", config->sdb_port);
        return -ENODEV;
    }

    gpio_pin_configure(data->gpio, config->sdb_pin, (GPIO_OUTPUT | config->sdb_flags));

    if (gpio_pin_set(data->gpio, config->sdb_pin, 1)) {
        LOG_ERR("SDB pin for %s cannot be pulled high", config->label);
        return -EIO;
    }

    // Set configuration registers
    if (is31fl3741_set_page(dev, IS31FL3741_PAGE_FUNCTION)) {
        LOG_ERR("Couldn't switch to function registers on %s", config->label);
        return -EIO;
    }

    // Reset
    is31fl3741_reg_write(dev, 0x3f, 0xae); // Reset

    // Re-set configuration registers
    if (is31fl3741_set_page(dev, IS31FL3741_PAGE_FUNCTION)) {
        LOG_ERR("Couldn't switch to function registers on %s", config->label);
        return -EIO;
    }

    // Configure LED driver operation mode
    is31fl3741_reg_write(dev, 0x00,
                         (config->sws << 4) | (0x01 << 3) | 0x01); // SWS, H logic, Normal operation
    is31fl3741_reg_write(dev, 0x01, config->gcc);                  // Set GCC

    // Set scaling registers
    uint8_t *px_buffer = data->px_buffer;
    uint16_t *rgb_map = config->rgb_map;

    for (int i = 0; i < config->px_buffer_size; i += 3) {
        px_buffer[rgb_map[i]] = config->scaling_red;
        px_buffer[rgb_map[i + 1]] = config->scaling_green;
        px_buffer[rgb_map[i + 2]] = config->scaling_blue;
    }

    is31fl3741_set_page(dev, IS31FL3741_PAGE_SCALING_A);
    is31fl3741_reg_burst_write(dev, 0x00, px_buffer, 0xb4);

    is31fl3741_set_page(dev, IS31FL3741_PAGE_SCALING_B);
    is31fl3741_reg_burst_write(dev, 0x00, px_buffer + 0xb4, 0xab);

    // Re-initialize px_buffer to prevent any scaling values from being sent
    // to PWM registers during normal operation.
    for (size_t i = 0; i < config->px_buffer_size; ++i) {
        px_buffer[i] = 0;
    }

    // for (int i = 0; i<128; ++i) {
    //     if (!i2c_write(data->i2c, NULL, 0, i)) {
    //         LOG_DBG( "I2C DEVICE @ %x", i );
    //     }
    // }

    // uint8_t value = 0xff;
    // if (i2c_write(data->i2c, &value, 1, 0x2f)) {
    //     LOG_DBG("CANT WRITE TO POT");
    // } else {
    //     LOG_DBG("SUCCESS!");
    // }

    return 0;
}

static const struct led_strip_driver_api is31fl3741_api = {
    .update_rgb = is31fl3741_strip_update_rgb,
    .update_channels = is31fl3741_strip_update_channels,
};

#define IS31FL3741_GCC(idx)                                                                        \
    (DT_INST_PROP(idx, r_ext) * DT_INST_PROP(idx, led_max_current) * 256 * 256) / (383 * 255)

#define IS31FL3741_DEVICE(idx)                                                                     \
                                                                                                   \
    static uint8_t is31fl3741_##idx##_px_buffer[IS31FL3741_BUFFER_SIZE];                           \
                                                                                                   \
    static struct is31fl3741_data is31fl3741_##idx##_data = {                                      \
        .px_buffer = is31fl3741_##idx##_px_buffer,                                                 \
    };                                                                                             \
                                                                                                   \
    static uint16_t is31fl3741_##idx##_rgb_map[IS31FL3741_BUFFER_SIZE] = DT_INST_PROP(idx, map);   \
                                                                                                   \
    static uint8_t is31fl3741_##idx##_gamma[] = DT_INST_PROP(idx, gamma);                          \
                                                                                                   \
    static const struct is31fl3741_config is31fl3741_##idx##_config = {                            \
        .bus = DT_INST_BUS_LABEL(idx),                                                             \
        .reg = DT_INST_REG_ADDR(idx),                                                              \
        .label = DT_INST_LABEL(idx),                                                               \
        .sdb_port = DT_INST_GPIO_LABEL(idx, sdb_gpios),                                            \
        .sdb_pin = DT_INST_GPIO_PIN(idx, sdb_gpios),                                               \
        .sdb_flags = DT_INST_GPIO_FLAGS(idx, sdb_gpios),                                           \
        .px_buffer_size = IS31FL3741_BUFFER_SIZE,                                                  \
        .gcc = IS31FL3741_GCC(idx),                                                                \
        .sws = DT_INST_PROP(idx, sw_setting),                                                      \
        .rgb_map = is31fl3741_##idx##_rgb_map,                                                     \
        .gamma = is31fl3741_##idx##_gamma,                                                         \
        .scaling_red = DT_INST_PROP(idx, red_scaling),                                             \
        .scaling_green = DT_INST_PROP(idx, green_scaling),                                         \
        .scaling_blue = DT_INST_PROP(idx, blue_scaling),                                           \
    };                                                                                             \
                                                                                                   \
    DEVICE_DT_INST_DEFINE(idx, &is31fl3741_init, NULL, &is31fl3741_##idx##_data,                   \
                          &is31fl3741_##idx##_config, POST_KERNEL, CONFIG_LED_STRIP_INIT_PRIORITY, \
                          &is31fl3741_api);

DT_INST_FOREACH_STATUS_OKAY(IS31FL3741_DEVICE);
