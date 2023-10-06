
#include "obs-module.h"
#include "version.h"

#define LOG_OFFSET_DB 6.0f
#define LOG_RANGE_DB 96.0f
enum matte_layout {
	MATTE_LAYOUT_HORIZONTAL,
	MATTE_LAYOUT_VERTICAL,
	MATTE_LAYOUT_MASK,
};

struct browser_transition {
	obs_source_t *source;
	obs_source_t *browser;
	bool transitioning;
	float transition_point;
	obs_transition_audio_mix_callback_t mix_a;
	obs_transition_audio_mix_callback_t mix_b;
	float transition_a_mul;
	float transition_b_mul;
	float duration;
	bool matte_rendered;
	bool track_matte_enabled;
	enum matte_layout matte_layout;
	float matte_width_factor;
	float matte_height_factor;

	gs_effect_t *matte_effect;
	gs_eparam_t *ep_a_tex;
	gs_eparam_t *ep_b_tex;
	gs_eparam_t *ep_matte_tex;
	gs_eparam_t *ep_invert_matte;

	gs_texrender_t *matte_tex;
	gs_texrender_t *stinger_tex;

	bool invert_matte;
	bool do_texrender;
};

static void *browser_transition_create(obs_data_t *settings,
				       obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	struct browser_transition *bt =
		bzalloc(sizeof(struct browser_transition));
	bt->source = source;
	bt->browser = obs_source_create_private(
		"browser_source", obs_source_get_name(source), NULL);
	char *effect_file = obs_module_file("effects/matte_transition.effect");
	char *error_string = NULL;
	obs_enter_graphics();
	bt->matte_effect =
		gs_effect_create_from_file(effect_file, &error_string);
	obs_leave_graphics();

	bfree(effect_file);

	if (!bt->matte_effect) {
		blog(LOG_ERROR, "Could not open matte_transition.effect: %s",
		     error_string);
		bfree(error_string);
		bfree(bt);
		return NULL;
	}

	bt->ep_a_tex = gs_effect_get_param_by_name(bt->matte_effect, "a_tex");
	bt->ep_b_tex = gs_effect_get_param_by_name(bt->matte_effect, "b_tex");
	bt->ep_matte_tex =
		gs_effect_get_param_by_name(bt->matte_effect, "matte_tex");
	bt->ep_invert_matte =
		gs_effect_get_param_by_name(bt->matte_effect, "invert_matte");

	obs_transition_enable_fixed(bt->source, true, 0);
	obs_source_update(source, NULL);
	return bt;
}

void browser_transition_destroy(void *data)
{
	const struct browser_transition *browser_transition = data;
	obs_source_release(browser_transition->browser);

	obs_enter_graphics();

	gs_texrender_destroy(browser_transition->matte_tex);
	gs_texrender_destroy(browser_transition->stinger_tex);
	gs_effect_destroy(browser_transition->matte_effect);

	obs_leave_graphics();
	bfree(data);
}

const char *browser_transition_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("Browser");
}

static inline float calc_fade(float t, float mul)
{
	t *= mul;
	return t > 1.0f ? 1.0f : t;
}

static float mix_a_fade_in_out(void *data, float t)
{
	struct browser_transition *s = data;
	return 1.0f - calc_fade(t, s->transition_a_mul);
}

static float mix_b_fade_in_out(void *data, float t)
{
	struct browser_transition *s = data;
	return 1.0f - calc_fade(1.0f - t, s->transition_b_mul);
}

static float mix_a_cross_fade(void *data, float t)
{
	UNUSED_PARAMETER(data);
	return 1.0f - t;
}

static float mix_b_cross_fade(void *data, float t)
{
	UNUSED_PARAMETER(data);
	return t;
}

void browser_transition_update(void *data, obs_data_t *settings)
{
	struct browser_transition *browser_transition = data;

	browser_transition->duration =
		(float)obs_data_get_double(settings, "duration");
	obs_transition_enable_fixed(browser_transition->source, true,
				    (uint32_t)browser_transition->duration);

	const bool time_based_transition_point =
		obs_data_get_int(settings, "tp_type") == 1;
	if (time_based_transition_point) {
		const float transition_point_ms = (float)obs_data_get_double(
			settings, "transition_point_ms");
		if (browser_transition->duration > 0.0f)
			browser_transition->transition_point =
				transition_point_ms /
				browser_transition->duration;
	} else {
		browser_transition->transition_point =
			(float)obs_data_get_double(settings,
						   "transition_point") /
			100.0f;
	}

	bool track_matte_was_enabled = browser_transition->track_matte_enabled;

	browser_transition->track_matte_enabled =
		obs_data_get_bool(settings, "track_matte_enabled");
	browser_transition->matte_layout =
		(int)obs_data_get_int(settings, "track_matte_layout");
	browser_transition->matte_width_factor =
		(browser_transition->track_matte_enabled &&
				 browser_transition->matte_layout ==
					 MATTE_LAYOUT_HORIZONTAL
			 ? 2.0f
			 : 1.0f);
	browser_transition->matte_height_factor =
		(browser_transition->track_matte_enabled &&
				 browser_transition->matte_layout ==
					 MATTE_LAYOUT_VERTICAL
			 ? 2.0f
			 : 1.0f);
	browser_transition->invert_matte =
		obs_data_get_bool(settings, "invert_matte");

	browser_transition->do_texrender =
		browser_transition->track_matte_enabled &&
		browser_transition->matte_layout < MATTE_LAYOUT_MASK;

	browser_transition->transition_a_mul =
		(1.0f / browser_transition->transition_point);
	browser_transition->transition_b_mul =
		(1.0f / (1.0f - browser_transition->transition_point));

	obs_source_set_monitoring_type(browser_transition->browser,
				       obs_data_get_int(settings,
							"audio_monitoring"));
	float def =
		(float)obs_data_get_double(settings, "audio_volume") / 100.0f;
	float db;
	if (def >= 1.0f)
		db = 0.0f;
	else if (def <= 0.0f)
		db = -INFINITY;
	else
		db = -(LOG_RANGE_DB + LOG_OFFSET_DB) *
			     powf((LOG_RANGE_DB + LOG_OFFSET_DB) /
					  LOG_OFFSET_DB,
				  -def) +
		     LOG_OFFSET_DB;
	const float mul = obs_db_to_mul(db);
	obs_source_set_volume(browser_transition->browser, mul);
	if (!obs_data_get_int(settings, "audio_fade_style")) {
		browser_transition->mix_a = mix_a_fade_in_out;
		browser_transition->mix_b = mix_b_fade_in_out;
	} else {
		browser_transition->mix_a = mix_a_cross_fade;
		browser_transition->mix_b = mix_b_cross_fade;
	}
	obs_source_update(browser_transition->browser, settings);

	obs_data_t *s = obs_source_get_settings(browser_transition->browser);
	if (s) {
		uint32_t cx = obs_source_get_width(browser_transition->source);
		uint32_t cy = obs_source_get_height(browser_transition->source);
		if (browser_transition->track_matte_enabled) {
			cx *= (uint32_t)browser_transition->matte_width_factor;
			cy *= (uint32_t)browser_transition->matte_height_factor;
		}

		const uint32_t x = (uint32_t)obs_data_get_int(s, "width");
		const uint32_t y = (uint32_t)obs_data_get_int(s, "height");
		if (cx != x || cy != y) {
			obs_data_set_int(s, "width", cx);
			obs_data_set_int(s, "height", cy);
			obs_source_update(browser_transition->browser, NULL);
		}
		obs_data_release(s);
	}

	if (browser_transition->track_matte_enabled !=
	    track_matte_was_enabled) {
		obs_enter_graphics();

		gs_texrender_destroy(browser_transition->matte_tex);
		gs_texrender_destroy(browser_transition->stinger_tex);
		browser_transition->matte_tex = NULL;
		browser_transition->stinger_tex = NULL;

		if (browser_transition->track_matte_enabled) {
			browser_transition->matte_tex =
				gs_texrender_create(GS_RGBA, GS_ZS_NONE);
			browser_transition->stinger_tex =
				gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		}

		obs_leave_graphics();
	}
}

void browser_transition_matte_render(void *data, gs_texture_t *a,
				     gs_texture_t *b, float t, uint32_t cx,
				     uint32_t cy)
{
	struct browser_transition *s = data;
	struct vec4 background;
	vec4_zero(&background);

	obs_source_t *matte_source = s->browser;

	float matte_cx = (float)obs_source_get_width(matte_source) /
			 s->matte_width_factor;
	float matte_cy = (float)obs_source_get_height(matte_source) /
			 s->matte_height_factor;

	float width_offset = (s->matte_layout == MATTE_LAYOUT_HORIZONTAL
				      ? (-matte_cx)
				      : 0.0f);
	float height_offset =
		(s->matte_layout == MATTE_LAYOUT_VERTICAL ? (-matte_cy) : 0.0f);

	// Track matte media render
	if (matte_cx > 0 && matte_cy > 0) {
		float scale_x = (float)cx / matte_cx;
		float scale_y = (float)cy / matte_cy;

		const enum gs_color_space space =
			obs_source_get_color_space(matte_source, 0, NULL);
		enum gs_color_format format = gs_get_format_from_space(space);
		if (gs_texrender_get_format(s->matte_tex) != format) {
			gs_texrender_destroy(s->matte_tex);
			s->matte_tex = gs_texrender_create(format, GS_ZS_NONE);
		}

		if (gs_texrender_begin_with_color_space(s->matte_tex, cx, cy,
							space)) {
			gs_matrix_scale3f(scale_x, scale_y, 1.0f);
			gs_matrix_translate3f(width_offset, height_offset,
					      0.0f);
			gs_clear(GS_CLEAR_COLOR, &background, 0.0f, 0);
			gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f,
				 100.0f);

			obs_source_video_render(matte_source);

			gs_texrender_end(s->matte_tex);
		}
	}

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);

	/* texture setters look reversed, but they aren't */
	const char *tech_name = "StingerMatte";
	if (gs_get_color_space() == GS_CS_SRGB) {
		/* users want nonlinear fade */
		gs_effect_set_texture(s->ep_a_tex, a);
		gs_effect_set_texture(s->ep_b_tex, b);
	} else {
		/* nonlinear fade is too wrong, so use linear fade */
		gs_effect_set_texture_srgb(s->ep_a_tex, a);
		gs_effect_set_texture_srgb(s->ep_b_tex, b);
		tech_name = "StingerMatteLinear";
	}
	gs_effect_set_texture(s->ep_matte_tex,
			      gs_texrender_get_texture(s->matte_tex));
	gs_effect_set_bool(s->ep_invert_matte, s->invert_matte);

	while (gs_effect_loop(s->matte_effect, tech_name))
		gs_draw_sprite(NULL, 0, cx, cy);

	gs_enable_framebuffer_srgb(previous);

	UNUSED_PARAMETER(t);
}

static void stinger_texrender(struct browser_transition *s, uint32_t source_cx,
			      uint32_t source_cy, uint32_t media_cx,
			      uint32_t media_cy, enum gs_color_space space)
{
	enum gs_color_format format = gs_get_format_from_space(space);
	if (gs_texrender_get_format(s->stinger_tex) != format) {
		gs_texrender_destroy(s->stinger_tex);
		s->stinger_tex = gs_texrender_create(format, GS_ZS_NONE);
	}

	if (gs_texrender_begin_with_color_space(s->stinger_tex, source_cx,
						source_cy, space)) {
		float cx = (float)media_cx / s->matte_width_factor;
		float cy = (float)media_cy / s->matte_height_factor;

		gs_ortho(0.0f, cx, 0.0f, cy, -100.0f, 100.0f);

		gs_blend_state_push();
		gs_enable_blending(false);
		obs_source_video_render(s->browser);
		gs_blend_state_pop();

		gs_texrender_end(s->stinger_tex);
	}
}

static const char *
get_tech_name_and_multiplier(enum gs_color_space current_space,
			     enum gs_color_space source_space,
			     float *multiplier)
{
	const char *tech_name = "Draw";
	*multiplier = 1.f;

	switch (source_space) {
	case GS_CS_SRGB:
	case GS_CS_SRGB_16F:
		if (current_space == GS_CS_709_SCRGB) {
			tech_name = "DrawMultiply";
			*multiplier = obs_get_video_sdr_white_level() / 80.0f;
		}
		break;
	case GS_CS_709_EXTENDED:
		if (current_space == GS_CS_SRGB ||
		    current_space == GS_CS_SRGB_16F) {
			tech_name = "DrawTonemap";
		} else if (current_space == GS_CS_709_SCRGB) {
			tech_name = "DrawMultiply";
			*multiplier = obs_get_video_sdr_white_level() / 80.0f;
		}
		break;
	case GS_CS_709_SCRGB:
		if (current_space == GS_CS_SRGB ||
		    current_space == GS_CS_SRGB_16F) {
			tech_name = "DrawMultiplyTonemap";
			*multiplier = 80.0f / obs_get_video_sdr_white_level();
		} else if (current_space == GS_CS_709_SCRGB) {
			tech_name = "DrawMultiply";
			*multiplier = 80.0f / obs_get_video_sdr_white_level();
		}
		break;
	}

	return tech_name;
}

void browser_transition_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct browser_transition *browser_transition = data;
	const uint32_t media_cx =
		obs_source_get_width(browser_transition->browser);
	const uint32_t media_cy =
		obs_source_get_height(browser_transition->browser);
	float t = obs_transition_get_time(browser_transition->source);
	if (browser_transition->track_matte_enabled) {
		const bool ready =
			obs_source_active(browser_transition->browser) &&
			!!media_cx && !!media_cy;
		if (ready) {
			if (!browser_transition->matte_rendered)
				browser_transition->matte_rendered = true;
			obs_transition_video_render(
				browser_transition->source,
				browser_transition_matte_render);
		} else {
			obs_transition_video_render_direct(
				browser_transition->source,
				browser_transition->matte_rendered
					? OBS_TRANSITION_SOURCE_B
					: OBS_TRANSITION_SOURCE_A);
		}
		if (t <= 0.0f || t >= 1.0f) {
			if (browser_transition->transitioning) {
				browser_transition->transitioning = false;
				obs_source_remove_active_child(
					browser_transition->source,
					browser_transition->browser);
			}
			return;
		}
		if (browser_transition->matte_layout == MATTE_LAYOUT_MASK)
			return;
	} else {

		const bool use_a = t < browser_transition->transition_point;

		enum obs_transition_target target =
			use_a ? OBS_TRANSITION_SOURCE_A
			      : OBS_TRANSITION_SOURCE_B;

		if (!obs_transition_video_render_direct(
			    browser_transition->source, target)) {
			if (browser_transition->transitioning) {
				browser_transition->transitioning = false;
				obs_source_remove_active_child(
					browser_transition->source,
					browser_transition->browser);
			}
			return;
		}
	}

	/* --------------------- */

	uint32_t source_cx = obs_source_get_width(browser_transition->source);
	uint32_t source_cy = obs_source_get_height(browser_transition->source);

	float source_cxf = (float)source_cx;
	float source_cyf = (float)source_cy;

	if (!media_cx || !media_cy)
		return;

	if (browser_transition->do_texrender) {
		const enum gs_color_space space = obs_source_get_color_space(
			browser_transition->browser, 0, NULL);
		stinger_texrender(browser_transition, source_cx, source_cy,
				  media_cx, media_cy, space);

		const bool previous = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(true);

		float multiplier;
		const char *technique = get_tech_name_and_multiplier(
			gs_get_color_space(), space, &multiplier);

		gs_effect_t *e = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *p_image = gs_effect_get_param_by_name(e, "image");
		gs_eparam_t *p_multiplier =
			gs_effect_get_param_by_name(e, "multiplier");
		gs_texture_t *tex = gs_texrender_get_texture(
			browser_transition->stinger_tex);

		gs_effect_set_texture_srgb(p_image, tex);
		gs_effect_set_float(p_multiplier, multiplier);
		while (gs_effect_loop(e, technique))
			gs_draw_sprite(NULL, 0, source_cx, source_cy);

		gs_enable_framebuffer_srgb(previous);
	} else {
		const bool previous = gs_set_linear_srgb(true);
		gs_matrix_push();
		gs_matrix_scale3f(source_cxf / (float)media_cx,
				  source_cyf / (float)media_cy, 1.0f);
		obs_source_video_render(browser_transition->browser);
		gs_matrix_pop();
		gs_set_linear_srgb(previous);
	}
	UNUSED_PARAMETER(effect);
}

static bool browser_transition_audio_render(void *data, uint64_t *ts_out,
					    struct obs_source_audio_mix *audio,
					    uint32_t mixers, size_t channels,
					    size_t sample_rate)
{
	struct browser_transition *browser_transition = data;
	if (!browser_transition)
		return false;

	uint64_t ts = 0;
	if (!obs_source_audio_pending(browser_transition->browser)) {
		ts = obs_source_get_audio_timestamp(
			browser_transition->browser);
		if (!ts)
			return false;
	}

	const bool success = obs_transition_audio_render(
		browser_transition->source, ts_out, audio, mixers, channels,
		sample_rate, browser_transition->mix_a,
		browser_transition->mix_b);
	if (!ts)
		return success;

	if (!*ts_out || ts < *ts_out)
		*ts_out = ts;

	struct obs_source_audio_mix child_audio;
	obs_source_get_audio_mix(browser_transition->browser, &child_audio);
	for (size_t mix = 0; mix < MAX_AUDIO_MIXES; mix++) {
		if ((mixers & (1 << mix)) == 0)
			continue;

		for (size_t ch = 0; ch < channels; ch++) {
			register float *out = audio->output[mix].data[ch];
			register float *in = child_audio.output[mix].data[ch];
			register float *end = in + AUDIO_OUTPUT_FRAMES;

			while (in < end)
				*(out++) += *(in++);
		}
	}

	return true;
}

bool browser_reroute_audio_changed(void *data, obs_properties_t *props,
				   obs_property_t *property,
				   obs_data_t *settings)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(property);
	obs_property_t *audio_monitoring =
		obs_properties_get(props, "audio_monitoring");
	obs_property_t *audio_volume =
		obs_properties_get(props, "audio_volume");
	const bool reroute_audio = obs_data_get_bool(settings, "reroute_audio");
	obs_property_set_visible(audio_monitoring, reroute_audio);
	obs_property_set_visible(audio_volume, reroute_audio);
	return true;
}

static bool transition_point_type_modified(obs_properties_t *ppts,
					   obs_property_t *p, obs_data_t *s)
{
	int64_t type = obs_data_get_int(s, "tp_type");

	obs_property_t *prop_transition_point =
		obs_properties_get(ppts, "transition_point");
	obs_property_t *prop_transition_point_ms =
		obs_properties_get(ppts, "transition_point_ms");

	if (type == 1) {
		obs_property_set_visible(prop_transition_point, false);
		obs_property_set_visible(prop_transition_point_ms, true);
	} else {
		obs_property_set_visible(prop_transition_point, true);
		obs_property_set_visible(prop_transition_point_ms, false);
	}

	UNUSED_PARAMETER(p);
	return true;
}

static bool track_matte_enabled_modified(obs_properties_t *ppts,
					 obs_property_t *p, obs_data_t *s)
{
	bool track_matte_enabled = obs_data_get_bool(s, "track_matte_enabled");
	obs_property_t *prop_tp_type = obs_properties_get(ppts, "tp_type");

	if (track_matte_enabled) {
		obs_property_set_description(
			prop_tp_type,
			obs_module_text("AudioTransitionPointType"));
	} else {
		obs_property_set_description(
			prop_tp_type, obs_module_text("TransitionPointType"));
	}

	UNUSED_PARAMETER(p);
	return true;
}

static bool refresh_browser_source(obs_properties_t *props,
				   obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	obs_source_t *browser = data;
	if (!browser)
		return false;
	obs_properties_t *browser_props = obs_source_properties(browser);
	if (!browser_props)
		return false;
	obs_property_t *refresh =
		obs_properties_get(browser_props, "refreshnocache");
	bool result =
		obs_property_button_clicked(refresh, data);
	obs_properties_destroy(browser_props);
	return result;
}

obs_properties_t *browser_transition_properties(void *data)
{
	struct browser_transition *browser_transition = data;
	obs_properties_t *props = obs_properties_create();
	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	obs_property_t *p = obs_properties_add_float(
		props, "duration", obs_module_text("Duration"), 0.0, 30000.0,
		100.0);
	obs_property_float_set_suffix(p, " ms");

	obs_properties_t *track_matte_group = obs_properties_create();

	p = obs_properties_add_list(track_matte_group, "track_matte_layout",
				    obs_module_text("TrackMatteLayout"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p,
				  obs_module_text("TrackMatteLayoutHorizontal"),
				  MATTE_LAYOUT_HORIZONTAL);
	obs_property_list_add_int(p,
				  obs_module_text("TrackMatteLayoutVertical"),
				  MATTE_LAYOUT_VERTICAL);

	obs_property_list_add_int(p, obs_module_text("TrackMatteLayoutMask"),
				  MATTE_LAYOUT_MASK);

	obs_properties_add_bool(track_matte_group, "invert_matte",
				obs_module_text("InvertTrackMatte"));

	p = obs_properties_add_group(props, "track_matte_enabled",
				     obs_module_text("TrackMatteEnabled"),
				     OBS_GROUP_CHECKABLE, track_matte_group);

	obs_property_set_modified_callback(p, track_matte_enabled_modified);

	p = obs_properties_add_list(props, "tp_type",
				    obs_module_text("TransitionPointType"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(
		p, obs_module_text("TransitionPointTypePercentage"), 0);
	obs_property_list_add_int(p, obs_module_text("TransitionPointTypeTime"),
				  1);
	obs_property_set_modified_callback(p, transition_point_type_modified);
	p = obs_properties_add_float_slider(props, "transition_point",
					    obs_module_text("TransitionPoint"),
					    0, 100.0, 1.0);
	obs_property_float_set_suffix(p, "%");

	p = obs_properties_add_float(props, "transition_point_ms",
				     obs_module_text("TransitionPoint"), 0,
				     30000.0, 100.0);
	obs_property_float_set_suffix(p, " ms");

	// audio fade settings
	obs_property_t *audio_fade_style = obs_properties_add_list(
		props, "audio_fade_style", obs_module_text("AudioFadeStyle"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(audio_fade_style,
				  obs_module_text("FadeOutFadeIn"), 0);
	obs_property_list_add_int(audio_fade_style,
				  obs_module_text("CrossFade"), 1);

	obs_properties_t *bp =
		obs_source_properties(browser_transition->browser);
	obs_properties_remove_by_name(bp, "width");
	obs_properties_remove_by_name(bp, "height");
	obs_properties_remove_by_name(bp, "refreshnocache");
	obs_properties_add_button2(bp, "refreshnocache",
				   obs_module_text("RefreshNoCache"),
				   refresh_browser_source,
				   browser_transition->browser);

	// audio output settings
	p = obs_properties_add_float_slider(bp, "audio_volume",
					    obs_module_text("AudioVolume"), 0,
					    100.0, 1.0);
	obs_property_float_set_suffix(p, "%");
	obs_property_t *monitor_list = obs_properties_add_list(
		bp, "audio_monitoring", obs_module_text("AudioMonitoring"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(monitor_list, obs_module_text("None"),
				  OBS_MONITORING_TYPE_NONE);
	obs_property_list_add_int(monitor_list, obs_module_text("MonitorOnly"),
				  OBS_MONITORING_TYPE_MONITOR_ONLY);
	obs_property_list_add_int(monitor_list, obs_module_text("Both"),
				  OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT);

	obs_properties_add_group(props, "browser_group",
				 obs_module_text("Browser"), OBS_GROUP_NORMAL,
				 bp);

	p = obs_properties_get(bp, "reroute_audio");
	if (p) {
		obs_property_set_modified_callback2(
			p, browser_reroute_audio_changed, data);
	}
	obs_properties_add_text(
		props, "plugin_info",
		"<a href=\"https://obsproject.com/forum/resources/browser-transition.1653/\">Browser Transition</a> (" PROJECT_VERSION
		") by <a href=\"https://www.exeldro.com\">Exeldro</a>",
		OBS_TEXT_INFO);
	return props;
}

void browser_transition_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "duration", 500.0);
	obs_data_set_default_double(settings, "transition_point", 50.0);
	obs_data_set_default_double(settings, "transition_point_ms", 250.0);
	obs_data_set_default_double(settings, "audio_volume", 100.0);
	obs_data_t *d = obs_get_source_defaults("browser_source");
	obs_data_item_t *i = obs_data_first(d);
	while (i) {
		const enum obs_data_type t = obs_data_item_gettype(i);
		if (t == OBS_DATA_STRING) {
			obs_data_set_default_string(
				settings, obs_data_item_get_name(i),
				obs_data_item_get_default_string(i));
		} else if (t == OBS_DATA_NUMBER) {
			const enum obs_data_number_type nt =
				obs_data_item_numtype(i);
			if (nt == OBS_DATA_NUM_INT) {
				obs_data_set_default_int(
					settings, obs_data_item_get_name(i),
					obs_data_item_get_default_int(i));
			} else if (nt == OBS_DATA_NUM_DOUBLE) {
				obs_data_set_default_double(
					settings, obs_data_item_get_name(i),
					obs_data_item_get_default_double(i));
			}
		} else if (t == OBS_DATA_BOOLEAN) {
			obs_data_set_default_bool(
				settings, obs_data_item_get_name(i),
				obs_data_item_get_default_bool(i));
		} else if (t == OBS_DATA_OBJECT) {
			obs_data_t *o = obs_data_item_get_default_obj(i);
			obs_data_set_default_obj(settings,
						 obs_data_item_get_name(i), o);
			obs_data_release(o);
		} else if (t == OBS_DATA_ARRAY) {
			obs_data_array_t *a =
				obs_data_item_get_default_array(i);
			obs_data_set_default_array(
				settings, obs_data_item_get_name(i), a);
			obs_data_array_release(a);
		}
		obs_data_item_next(&i);
	}

	obs_data_release(d);
}

void browser_transition_start(void *data)
{
	struct browser_transition *browser_transition = data;

	uint32_t cx = obs_source_get_width(browser_transition->source);
	uint32_t cy = obs_source_get_height(browser_transition->source);
	if (!cx || !cy) {
		obs_source_t *s = obs_transition_get_active_source(
			browser_transition->source);
		if (s) {
			cx = obs_source_get_width(s);
			cy = obs_source_get_height(s);
			obs_source_release(s);
		}
	}
	if (!cx || !cy) {
		obs_source_t *s = obs_transition_get_source(
			browser_transition->source, OBS_TRANSITION_SOURCE_A);
		if (s) {

			cx = obs_source_get_width(s);
			cy = obs_source_get_height(s);
			obs_source_release(s);
		}
	}
	if (!cx || !cy) {
		obs_source_t *s = obs_transition_get_source(
			browser_transition->source, OBS_TRANSITION_SOURCE_B);
		if (s) {

			cx = obs_source_get_width(s);
			cy = obs_source_get_height(s);
			obs_source_release(s);
		}
	}
	if (!cx || !cy)
		return;
	obs_data_t *s = obs_source_get_settings(browser_transition->browser);
	if (!s)
		return;
	if (browser_transition->track_matte_enabled) {
		cx *= (uint32_t)browser_transition->matte_width_factor;
		cy *= (uint32_t)browser_transition->matte_height_factor;
	}

	const uint32_t x = (uint32_t)obs_data_get_int(s, "width");
	const uint32_t y = (uint32_t)obs_data_get_int(s, "height");
	if (cx != x || cy != y) {
		obs_data_set_int(s, "width", cx);
		obs_data_set_int(s, "height", cy);
		obs_source_update(browser_transition->browser, NULL);
	}
	obs_data_release(s);

	browser_transition->matte_rendered = false;

	obs_transition_enable_fixed(browser_transition->source, true,
				    (uint32_t)browser_transition->duration);

	if (!browser_transition->transitioning) {
		browser_transition->transitioning = true;
		obs_source_add_active_child(browser_transition->source,
					    browser_transition->browser);
	}
	proc_handler_t *ph =
		obs_source_get_proc_handler(browser_transition->browser);
	if (!ph)
		return;
	obs_data_t *json = obs_data_create();
	obs_data_set_string(json, "transition",
			    obs_source_get_name(browser_transition->source));
	obs_data_set_bool(json, "trackMatte",
			  browser_transition->track_matte_enabled);
	obs_data_set_double(json, "duration", browser_transition->duration);
	obs_data_set_double(json, "transitionPoint",
			    browser_transition->transition_point);
	struct calldata cd = {0};
	calldata_set_string(&cd, "eventName", "transitionStart");
	calldata_set_string(&cd, "jsonString", obs_data_get_json(json));
	proc_handler_call(ph, "javascript_event", &cd);
	calldata_free(&cd);
	obs_data_release(json);
}

void browser_transition_stop(void *data)
{
	struct browser_transition *browser_transition = data;
	if (!browser_transition->browser)
		return;
	if (browser_transition->transitioning) {
		browser_transition->transitioning = false;
		obs_source_remove_active_child(browser_transition->source,
					       browser_transition->browser);
	}
	proc_handler_t *ph =
		obs_source_get_proc_handler(browser_transition->browser);
	if (!ph)
		return;
	struct calldata cd = {0};
	calldata_set_string(&cd, "eventName", "transitionStop");
	proc_handler_call(ph, "javascript_event", &cd);
	calldata_free(&cd);
}

static void browser_transition_enum_active_sources(
	void *data, obs_source_enum_proc_t enum_callback, void *param)
{
	struct browser_transition *s = data;
	if (s->browser && s->transitioning)
		enum_callback(s->source, s->browser, param);
}

static void browser_transition_enum_all_sources(
	void *data, obs_source_enum_proc_t enum_callback, void *param)
{
	struct browser_transition *s = data;
	if (s->browser)
		enum_callback(s->source, s->browser, param);
}

static void browser_transition_tick(void *data, float seconds)
{
	struct browser_transition *s = data;

	if (s->track_matte_enabled) {
		gs_texrender_reset(s->stinger_tex);
		gs_texrender_reset(s->matte_tex);
	}

	UNUSED_PARAMETER(seconds);
}

static enum gs_color_space
browser_transition_get_color_space(void *data, size_t count,
				   const enum gs_color_space *preferred_spaces)
{
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(preferred_spaces);

	struct browser_transition *s = data;
	return obs_transition_video_get_color_space(s->source);
}

struct obs_source_info browser_transition_info = {
	.id = "browser_transition",
	.type = OBS_SOURCE_TYPE_TRANSITION,
	.get_name = browser_transition_get_name,
	.create = browser_transition_create,
	.destroy = browser_transition_destroy,
	.update = browser_transition_update,
	.video_render = browser_transition_video_render,
	.audio_render = browser_transition_audio_render,
	.get_properties = browser_transition_properties,
	.get_defaults = browser_transition_defaults,
	.transition_start = browser_transition_start,
	.transition_stop = browser_transition_stop,
	.enum_active_sources = browser_transition_enum_active_sources,
	.enum_all_sources = browser_transition_enum_all_sources,
	.load = browser_transition_update,
	.video_tick = browser_transition_tick,
	.video_get_color_space = browser_transition_get_color_space,
};

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("Exeldro")
OBS_MODULE_USE_DEFAULT_LOCALE("browser-transition", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Description");
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return obs_module_text("BrowserTransition");
}

bool obs_module_load(void)
{
	blog(LOG_INFO, "[Browser Transition] loaded version %s",
	     PROJECT_VERSION);
	obs_register_source(&browser_transition_info);
	return true;
}
