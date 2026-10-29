/*
 * Copyright (C) 2019 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PAN_SAMPLES_H
#define PAN_SAMPLES_H

#include <genxml/gen_macros.h>

unsigned pan_sample_positions_buffer_size(void);

unsigned pan_sample_positions_pattern_size(void);

void pan_upload_sample_positions(void *buffer);

void pan_upload_custom_sample_positions(void *buffer,
                                       enum mali_sample_pattern pattern,
                                       const float *locations_xy,
                                       unsigned sample_count);

unsigned pan_sample_positions_offset(enum mali_sample_pattern pattern);

#endif
