// SPDX-License-Identifier: MIT
/* Standard-DV transport through native AMD DC HDMI. The player owns pixels
 * and frame metadata; this adapter only protects and signals their transport.
 * No global color/debug bypass is used and ordinary streams are untouched.
 */
#include <linux/hdmi.h>
#include <linux/moduleparam.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_dvbridge.h>
#include <drm/drm_edid.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include "amdgpu.h"
#include "amdgpu_dm_dv.h"
#include "dc.h"
#include "dc_types.h"
#include "dc_types.h"
#ifdef CONFIG_X86_PS5
#include <linux/ps5.h>
#endif

static bool dv_standard = true;
module_param_named(dv_standard, dv_standard, bool, 0444);
MODULE_PARM_DESC(dv_standard, "Expose experimental byte-preserving Standard-DV on native DCN HDMI");

static bool dv_dcn_supported(enum dce_version version)
{
	/* Explicit families, not a >= comparison that enables unknown hardware. */
	switch (version) {
	case DCN_VERSION_1_0: case DCN_VERSION_1_01:
	case DCN_VERSION_2_0: case DCN_VERSION_2_01: case DCN_VERSION_2_1:
	case DCN_VERSION_3_0: case DCN_VERSION_3_01:
	case DCN_VERSION_3_02: case DCN_VERSION_3_03:
	case DCN_VERSION_3_1: case DCN_VERSION_3_14:
	case DCN_VERSION_3_15: case DCN_VERSION_3_16:
	case DCN_VERSION_3_2: case DCN_VERSION_3_21:
	case DCN_VERSION_3_5: case DCN_VERSION_3_51: case DCN_VERSION_3_6:
		return true;
	default:
		return false;
	}
}

/*
 * The PS5's DP output feeds an HDMI converter that can send the DV VSIF
 * (drivers/ps5/hdmi.c), so its DP connector stands in for native HDMI.
 */
static bool dv_ps5_hdmi(const struct drm_connector *connector)
{
#ifdef CONFIG_X86_PS5
	return connector->connector_type == DRM_MODE_CONNECTOR_DisplayPort &&
	       isHdmiHdrSupported();
#else
	return false;
#endif
}

static bool dv_hdmi_connector(const struct drm_connector *connector)
{
	return connector->connector_type == DRM_MODE_CONNECTOR_HDMIA ||
	       dv_ps5_hdmi(connector);
}

static bool dv_hdmi_stream(const struct drm_connector *connector,
			   const struct dc_stream_state *stream)
{
	if (dv_ps5_hdmi(connector))
		return dc_is_dp_signal(stream->signal);
	return stream->signal == SIGNAL_TYPE_HDMI_TYPE_A &&
	       stream->link && stream->link->connector_signal == SIGNAL_TYPE_HDMI_TYPE_A;
}

void amdgpu_dm_dv_attach(struct amdgpu_dm_connector *connector)
{
	struct amdgpu_device *adev = drm_to_adev(connector->base.dev);

	if (!adev->dm.dc ||
	    !dv_dcn_supported(adev->dm.dc->ctx->dce_version) ||
	    !connector->dc_link)
		return;
	if (!dv_ps5_hdmi(&connector->base) &&
	    (connector->base.connector_type != DRM_MODE_CONNECTOR_HDMIA ||
	     connector->dc_link->connector_signal != SIGNAL_TYPE_HDMI_TYPE_A))
		return;

	if (dv_standard) {
		connector->dv_standard_property = drm_property_create_bool(connector->base.dev,
				DRM_MODE_PROP_ATOMIC, "DVBRIDGE_DV_STANDARD_LAB");
		if (connector->dv_standard_property)
			drm_object_attach_property(&connector->base.base,
					connector->dv_standard_property, 0);
	}
	connector->hdr10_deep_color_property = drm_property_create_bool(connector->base.dev,
				DRM_MODE_PROP_ATOMIC, "DVBRIDGE_HDR10_DEEP_COLOR");
	if (connector->hdr10_deep_color_property)
		drm_object_attach_property(&connector->base.base,
				connector->hdr10_deep_color_property, 0);
}

int amdgpu_dm_hdr10_validate(const struct dm_connector_state *state)
{
	const struct drm_connector_state *con = &state->base;
	const struct hdr_output_metadata *hdr;

	if (!state->hdr10_deep_color)
		return 0;
	if (state->dv_standard || !con->crtc ||
	    con->connector->connector_type != DRM_MODE_CONNECTOR_HDMIA ||
	    !to_amdgpu_dm_connector(con->connector)->hdr10_deep_color_property ||
	    con->max_requested_bpc < 10 ||
	    (con->colorspace != DRM_MODE_COLORIMETRY_BT2020_RGB &&
	     con->colorspace != DRM_MODE_COLORIMETRY_BT2020_YCC) ||
	    !con->hdr_output_metadata || con->hdr_output_metadata->length < sizeof(*hdr))
		return -EINVAL;
	hdr = con->hdr_output_metadata->data;
	return hdr->metadata_type == 0 && hdr->hdmi_metadata_type1.metadata_type == 0 &&
		hdr->hdmi_metadata_type1.eotf == HDMI_EOTF_SMPTE_ST2084 ? 0 : -EINVAL;
}

static unsigned int hdr10_depth(enum dc_color_depth depth)
{
	/* DC enum order is not precision order (9-bit follows 16-bit). */
	switch (depth) {
	case COLOR_DEPTH_101010: return 10;
	case COLOR_DEPTH_111111: return 11;
	case COLOR_DEPTH_121212: return 12;
	case COLOR_DEPTH_141414: return 14;
	case COLOR_DEPTH_161616: return 16;
	default: return 0;
	}
}

int amdgpu_dm_hdr10_atomic_check(struct drm_atomic_commit *state)
{
	struct drm_connector *connector;
	struct drm_connector_state *con;
	int i, ret;

	for_each_new_connector_in_state(state, connector, con, i) {
		struct drm_crtc_state *cs;
		struct dc_stream_state *stream;

		if (connector->connector_type != DRM_MODE_CONNECTOR_HDMIA ||
		    !to_dm_connector_state(con)->hdr10_deep_color)
			continue;
		ret = amdgpu_dm_hdr10_validate(to_dm_connector_state(con));
		if (ret)
			return ret;
		cs = drm_atomic_get_new_crtc_state(state, con->crtc);
		stream = cs ? to_dm_crtc_state(cs)->stream : NULL;
		if (!cs || !cs->active || !stream || stream->dv_standard ||
		    connector->connector_type != DRM_MODE_CONNECTOR_HDMIA ||
		    stream->signal != SIGNAL_TYPE_HDMI_TYPE_A ||
		    !stream->link || stream->link->connector_signal != SIGNAL_TYPE_HDMI_TYPE_A ||
		    hdr10_depth(stream->timing.display_color_depth) < 10)
			return -EINVAL;
	}
	return 0;
}

static int dv_copy_packet(struct dc_info_packet *packet, const u8 *wire, size_t size)
{
	memset(packet, 0, sizeof(*packet));
	if (size < 4 || size > 3 + sizeof(packet->sb))
		return -EINVAL;
	packet->hb0 = wire[0];
	packet->hb1 = wire[1];
	packet->hb2 = wire[2];
	/* DC's sb starts with the checksum, unlike its separate three-byte header. */
	memcpy(packet->sb, wire + 3, size - 3);
	packet->valid = true;
	return 0;
}

/* Hades RC1 9a06cf50, d1e00b45: the stream owns this sink reference.
 * A connector EDID blob may be replaced independently during hotplug. Keep
 * our existing V2-only policy and reject lengths outside the owned buffer.
 */
static const struct dc_edid *dv_stream_edid(const struct dc_stream_state *stream)
{
	const struct dc_edid *edid;

	if (!stream || !stream->sink)
		return NULL;
	edid = &stream->sink->dc_edid;
	return edid->length && edid->length <= sizeof(edid->raw_edid) ? edid : NULL;
}

int amdgpu_dm_dv_configure_stream(struct dc_stream_state *stream,
				const struct dm_connector_state *state,
				const struct drm_display_mode *mode)
{
	struct drm_connector *connector = state->base.connector;
	struct amdgpu_dm_connector *aconnector = to_amdgpu_dm_connector(connector);
	const struct dc_edid *edid = dv_stream_edid(stream);
	struct hdmi_dolby_vendor_infoframe dv;
	struct hdmi_avi_infoframe avi;
	struct dc_info_packet avi_packet, vsp_packet;
	u8 wire[HDMI_INFOFRAME_SIZE(DOLBY_VENDOR)];
	struct drm_dvbridge_link link = {
		.requested = state->dv_standard,
		.lab_enabled = dv_standard && stream->ctx &&
			dv_dcn_supported(stream->ctx->dce_version),
		.sink_standard = edid && drm_dvbridge_edid(edid->raw_edid, edid->length),
		.hdmi = (stream->signal == SIGNAL_TYPE_HDMI_TYPE_A || dv_ps5_hdmi(connector)) &&
			connector->display_info.is_hdmi && connector->display_info.has_hdmi_infoframe,
		.native_hdmi = dv_hdmi_connector(connector) && dv_hdmi_stream(connector, stream),
		.rgb = stream->timing.pixel_encoding == PIXEL_ENCODING_RGB,
		.limited_range = state->base.hdmi.broadcast_rgb != DRM_HDMI_BROADCAST_RGB_FULL,
		.hdr_blob = !!state->base.hdr_output_metadata,
		.progressive = !(mode->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN |
				DRM_MODE_FLAG_DBLCLK | DRM_MODE_FLAG_3D_MASK)),
		.requested_bpc = state->base.max_requested_bpc,
		.pipe_bpp = stream->timing.display_color_depth == COLOR_DEPTH_888 ? 24 : 0,
		.colorspace = state->base.colorspace,
		.width = mode->hdisplay, .height = mode->vdisplay,
		.rounded_refresh = drm_mode_vrefresh(mode),
	};
	int ret;

	if (!state->dv_standard)
		return 0;
	if (state->hdr10_deep_color)
		return -EINVAL;
	/* Legacy sink signaling is qualified separately; do not widen it here. */
	if (dvbridge_parse_edid(edid ? edid->raw_edid : NULL,
				edid ? edid->length : 0).version != 2)
		return -EOPNOTSUPP;
	ret = drm_dvbridge_validate(&link);
	if (ret)
		return ret;
	if (!aconnector->dv_standard_property || state->scaling != RMX_OFF ||
	    state->underscan_enable || aconnector->timing_changed ||
	    stream->timing.flags.DSC ||
	    stream->timing.h_addressable != mode->hdisplay ||
	    stream->timing.v_addressable != mode->vdisplay ||
	    stream->timing.pix_clk_100hz != mode->clock * 10 ||
	    !drm_dvbridge_avi_vic(drm_match_cea_mode(mode)))
		return -EINVAL;

	ret = drm_hdmi_avi_infoframe_from_display_mode(&avi, connector, mode);
	if (ret)
		return ret;
	avi.video_code = drm_match_cea_mode(mode);
	avi.colorspace = HDMI_COLORSPACE_RGB;
	avi.colorimetry = HDMI_COLORIMETRY_NONE;
	avi.extended_colorimetry = 0;
	avi.quantization_range = HDMI_QUANTIZATION_RANGE_FULL;
	ret = hdmi_avi_infoframe_pack(&avi, wire, sizeof(wire));
	if (ret < 0)
		return ret;
	ret = dv_copy_packet(&avi_packet, wire, ret);
	if (ret)
		return ret;
	hdmi_dolby_vendor_infoframe_init(&dv);
	dv.signal = true;
	ret = hdmi_dolby_vendor_infoframe_pack(&dv, wire, sizeof(wire));
	if (ret < 0)
		return ret;
	ret = dv_copy_packet(&vsp_packet, wire, ret);
	if (ret)
		return ret;

	/* Publish the candidate only after every packet has been prepared. */
	stream->avi_infopacket = avi_packet;
	stream->vsp_infopacket = vsp_packet;
	stream->dv_standard = true;
	stream->timing.hdmi_vic = 0;
	stream->timing.vic = avi.video_code;
	stream->output_color_space = COLOR_SPACE_SRGB;
	stream->out_transfer_func.type = TF_TYPE_BYPASS;
	stream->out_transfer_func.tf = TRANSFER_FUNCTION_LINEAR;
	stream->dither_option = DITHER_OPTION_DISABLE;
	stream->qs_bit = 1;
	return 0;
}

/* Include unchanged planes/connectors: a cursor-only update must not evade
 * transport validation merely because userspace omitted the DV property.
 */
int amdgpu_dm_dv_add_affected(struct drm_atomic_commit *state)
{
	struct drm_crtc *crtc;
	struct drm_crtc_state *crtc_state;
	struct drm_connector *connector;
	struct drm_connector_state *con_state;
	int i, j, ret;

	for_each_new_crtc_in_state(state, crtc, crtc_state, i) {
		struct dc_stream_state *stream = to_dm_crtc_state(crtc_state)->stream;
		bool requested = stream && stream->dv_standard;
		bool hdr10 = false;
		struct drm_connector_list_iter iter;

		/* Connector state is protected by the native connection lock. */
		ret = drm_modeset_lock(&state->dev->mode_config.connection_mutex,
				       state->acquire_ctx);
		if (ret)
			return ret;
		/* Plane-only updates can omit a connector with an active depth guard. */
		drm_connector_list_iter_begin(state->dev, &iter);
		drm_for_each_connector_iter(connector, &iter) {
			if (!(crtc_state->connector_mask & drm_connector_mask(connector)))
				continue;
			con_state = connector->state;
			if (connector->connector_type == DRM_MODE_CONNECTOR_HDMIA &&
			    con_state && con_state->crtc == crtc &&
			    to_dm_connector_state(con_state)->hdr10_deep_color)
				hdr10 = true;
		}
		drm_connector_list_iter_end(&iter);

		for_each_new_connector_in_state(state, connector, con_state, j) {
			if (dv_hdmi_connector(connector) &&
			    con_state->crtc == crtc && to_dm_connector_state(con_state)->dv_standard)
				requested = true;
			if (connector->connector_type == DRM_MODE_CONNECTOR_HDMIA &&
			    con_state->crtc == crtc && to_dm_connector_state(con_state)->hdr10_deep_color)
				hdr10 = true;
		}
		if (!requested && !hdr10)
			continue;
		ret = drm_atomic_add_affected_connectors(state, crtc);
		if (ret)
			return ret;
		if (!requested)
			continue;
		ret = drm_atomic_add_affected_planes(state, crtc);
		if (ret)
			return ret;
	}
	return 0;
}

static bool dv_identity_tf(enum amdgpu_transfer_function tf)
{
	return tf == AMDGPU_TRANSFER_FUNCTION_DEFAULT || tf == AMDGPU_TRANSFER_FUNCTION_IDENTITY;
}

static bool dv_plane_neutral(const struct drm_plane_state *p,
			     const struct drm_display_mode *mode)
{
	const struct dm_plane_state *dm = to_dm_plane_state(p);
	const struct dc_plane_state *dc = dm->dc_state;

	return p->plane->type != DRM_PLANE_TYPE_CURSOR && p->fb &&
		(p->fb->format->format == DRM_FORMAT_XRGB8888 ||
		 p->fb->format->format == DRM_FORMAT_XBGR8888) &&
		p->rotation == DRM_MODE_ROTATE_0 && p->alpha == DRM_BLEND_ALPHA_OPAQUE &&
		/* Primary planes do not expose pixel blend mode on AMD. XRGB/XBGR
		 * have no alpha channel: upstream disables per-pixel blending even
		 * when the property's implicit default is PREMULTI.
		 */
		!p->color_pipeline && p->visible &&
		p->fb->width == mode->hdisplay && p->fb->height == mode->vdisplay &&
		!p->src_x && !p->src_y && !p->crtc_x && !p->crtc_y &&
		p->src_w == (mode->hdisplay << 16) && p->src_h == (mode->vdisplay << 16) &&
		p->crtc_w == mode->hdisplay && p->crtc_h == mode->vdisplay &&
		!dm->degamma_lut && !dm->ctm && !dm->shaper_lut && !dm->lut3d &&
		!dm->blend_lut && dv_identity_tf(dm->degamma_tf) &&
		dv_identity_tf(dm->shaper_tf) && dv_identity_tf(dm->blend_tf) &&
		dm->hdr_mult == AMDGPU_HDR_MULT_DEFAULT && dc &&
		dc->visible && !dc->horizontal_mirror && dc->rotation == ROTATION_ANGLE_0 &&
		!dc->per_pixel_alpha && !dc->global_alpha &&
		dc->gamma_correction.is_identity &&
		dc->in_transfer_func.type == TF_TYPE_BYPASS &&
		dc->in_shaper_func.type == TF_TYPE_BYPASS &&
		dc->blend_tf.type == TF_TYPE_BYPASS && !dc->lut3d_func.state.bits.initialized &&
		dc->hdr_mult.value == dc_fixpt_one.value &&
		!dc->gamut_remap_matrix.enable_remap &&
		!dc->input_csc_color_matrix.enable_adjustment;
}

int amdgpu_dm_dv_atomic_check(struct drm_atomic_commit *state)
{
	struct drm_crtc *crtc;
	struct drm_crtc_state *cs;
	struct drm_plane *plane;
	struct drm_plane_state *ps;
	struct drm_connector *connector;
	struct drm_connector_state *con;
	int i, j;

	for_each_new_crtc_in_state(state, crtc, cs, i) {
		struct dm_crtc_state *dm = to_dm_crtc_state(cs);
		struct dc_stream_state *stream = dm->stream;
		unsigned int planes = 0, connectors = 0;

		if (!cs->active || !stream || !stream->dv_standard)
			continue;
		if (dm->freesync_config.state == VRR_STATE_ACTIVE_FIXED ||
		    dm->freesync_config.state == VRR_STATE_ACTIVE_VARIABLE ||
		    cs->ctm || cs->gamma_lut || cs->degamma_lut || cs->vrr_enabled ||
		    cs->async_flip || !dv_identity_tf(dm->regamma_tf) ||
		    stream->dither_option != DITHER_OPTION_DISABLE ||
		    stream->out_transfer_func.type != TF_TYPE_BYPASS ||
		    stream->gamut_remap_matrix.enable_remap || stream->csc_color_matrix.enable_adjustment ||
		    stream->output_color_space != COLOR_SPACE_SRGB ||
		    !stream->avi_infopacket.valid || !stream->vsp_infopacket.valid ||
		    /* Recheck the DC raster after global validation, not only the plane. */
		    stream->timing.flags.INTERLACE ||
		    stream->timing.timing_3d_format != TIMING_3D_FORMAT_NONE ||
		    stream->timing.h_border_left || stream->timing.h_border_right ||
		    stream->timing.v_border_top || stream->timing.v_border_bottom ||
		    stream->timing.h_addressable != cs->mode.hdisplay ||
		    stream->timing.v_addressable != cs->mode.vdisplay ||
		    stream->src.x || stream->src.y ||
		    stream->src.width != cs->mode.hdisplay || stream->src.height != cs->mode.vdisplay ||
		    stream->dst.x || stream->dst.y ||
		    stream->dst.width != cs->mode.hdisplay || stream->dst.height != cs->mode.vdisplay ||
		    stream->timing.pixel_encoding != PIXEL_ENCODING_RGB ||
		    stream->timing.display_color_depth != COLOR_DEPTH_888 || stream->timing.flags.DSC)
			return -EINVAL;
		for_each_new_connector_in_state(state, connector, con, j) {
			const struct dc_edid *edid = dv_stream_edid(stream);

			if (con->crtc != crtc)
				continue;
			if (!dv_hdmi_stream(connector, stream))
				return -EINVAL;

			if (!edid || !drm_dvbridge_edid(edid->raw_edid, edid->length) ||
			    dvbridge_parse_edid(edid->raw_edid, edid->length).version != 2 ||
			    !dv_hdmi_connector(connector) ||
			    !to_dm_connector_state(con)->dv_standard ||
			    to_dm_connector_state(con)->hdr10_deep_color ||
			    con->hdr_output_metadata || con->colorspace ||
			    con->max_requested_bpc != 8 ||
			    con->hdmi.broadcast_rgb != DRM_HDMI_BROADCAST_RGB_FULL ||
			    to_dm_connector_state(con)->scaling != RMX_OFF ||
			    to_dm_connector_state(con)->underscan_enable)
				return -EINVAL;
			connectors++;
		}
		for_each_new_plane_in_state(state, plane, ps, j) {
			if (ps->crtc != crtc || !ps->fb)
				continue;
			if (!dv_plane_neutral(ps, &cs->mode))
				return -EINVAL;
			planes++;
		}
		if (connectors != 1 || planes != 1 || hweight32(cs->connector_mask) != 1 ||
		    hweight32(cs->plane_mask) != 1)
			return -EINVAL;
	}
	return 0;
}
