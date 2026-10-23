/* SPDX-License-Identifier: MIT */
#ifndef AMDGPU_DM_DV_H
#define AMDGPU_DM_DV_H

#include "amdgpu_dm.h"

void amdgpu_dm_dv_attach(struct amdgpu_dm_connector *connector);
int amdgpu_dm_dv_add_affected(struct drm_atomic_commit *state);
int amdgpu_dm_dv_atomic_check(struct drm_atomic_commit *state);
int amdgpu_dm_hdr10_validate(const struct dm_connector_state *state);
int amdgpu_dm_hdr10_atomic_check(struct drm_atomic_commit *state);
int amdgpu_dm_dv_configure_stream(struct dc_stream_state *stream,
				const struct dm_connector_state *state,
				const struct drm_display_mode *mode);
#endif
