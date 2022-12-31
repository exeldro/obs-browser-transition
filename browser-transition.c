
#include "obs-module.h"
#include "version.h"

#define LOG_OFFSET_DB 6.0f
#define LOG_RANGE_DB 96.0f

struct browser_transition {
	obs_source_t *source;
	obs_source_t *browser;
	bool transitioning;
	float transition_point;
	obs_transition_audio_mix_callback_t mix_a;
	obs_transition_audio_mix_callback_t mix_b;
	float transition_a_mul;
	float transition_b_mul;
};

static void *browser_transition_create(obs_data_t *settings,
				       obs_source_t *source)
{
	struct browser_transition *bt =
		bzalloc(sizeof(struct browser_transition));
	bt->source = source;
	bt->browser = obs_source_create_private(
		"browser_source", obs_source_get_name(source), NULL);
	obs_source_update(source, settings);
	return bt;
}

void browser_transition_destroy(void *data)
{
	const struct browser_transition *browser_transition = data;
	obs_source_release(browser_transition->browser);
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

	browser_transition->transition_point =
		(float)obs_data_get_double(settings, "transition_point") /
		100.0f;
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
}

void browser_transition_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct browser_transition *browser_transition = data;

	float f = obs_transition_get_time(browser_transition->source);
	bool transitioning;
	if (f <= browser_transition->transition_point) {
		transitioning = obs_transition_video_render_direct(
			browser_transition->source, OBS_TRANSITION_SOURCE_A);
	} else {
		transitioning = obs_transition_video_render_direct(
			browser_transition->source, OBS_TRANSITION_SOURCE_B);
	}
	if (transitioning) {
		if (!browser_transition->transitioning) {
			browser_transition->transitioning = true;
			if (obs_source_showing(browser_transition->source))
				obs_source_inc_showing(
					browser_transition->browser);
			if (obs_source_active(browser_transition->source))
				obs_source_inc_active(
					browser_transition->source);
		}
		obs_source_video_render(browser_transition->browser);
	} else if (browser_transition->transitioning) {
		browser_transition->transitioning = false;
		if (obs_source_active(browser_transition->browser))
			obs_source_dec_active(browser_transition->source);
		if (obs_source_showing(browser_transition->browser))
			obs_source_dec_showing(browser_transition->browser);
	}
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

obs_properties_t *browser_transition_properties(void *data)
{
	struct browser_transition *browser_transition = data;
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p = obs_properties_add_float_slider(
		props, "transition_point", obs_module_text("TransitionPoint"),
		0, 100.0, 1.0);
	obs_property_float_set_suffix(p, "%");

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

	// audio output settings
	p = obs_properties_add_float_slider(bp, "audio_volume",
					    obs_module_text("Audio Volume"), 0,
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
	obs_data_set_default_double(settings, "transition_point", 50.0);
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

	const uint32_t cx = obs_source_get_width(browser_transition->source);
	const uint32_t cy = obs_source_get_height(browser_transition->source);
	if (!cx || !cy)
		return;
	obs_data_t *s = obs_source_get_settings(browser_transition->browser);
	if (!s)
		return;
	const uint32_t x = (uint32_t)obs_data_get_int(s, "width");
	const uint32_t y = (uint32_t)obs_data_get_int(s, "height");
	if (cx != x || cy != y) {
		obs_data_set_int(s, "width", cx);
		obs_data_set_int(s, "height", cy);
		obs_source_update(browser_transition->browser, NULL);
	}
	obs_data_release(s);
}

void browser_transition_stop(void *data)
{
	UNUSED_PARAMETER(data);
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
	.load = browser_transition_update};

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
