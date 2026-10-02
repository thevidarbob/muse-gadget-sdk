/*
 * Copyright (c) 2023 Bosch Sensortec GmbH. All rights reserved.
 *
 * BSD-3-Clause: see bmi270_config.c.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Uploaded to the BMI270 through INIT_DATA after a soft reset. */
extern const uint8_t bmi270_config_file[];
extern const size_t bmi270_config_size;
