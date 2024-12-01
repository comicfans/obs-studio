#include <arpa/inet.h>
#include <obs-module.h>
#include <graphics/image-file.h>
#include <graphics/matrix4.h>
#include <sys/socket.h>
#include <tobii/tobii.h>
#include <tobii/tobii_streams.h>
#include <util/threading.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <unistd.h>

#define blog(log_level, format, ...)            \
	blog(log_level, "[gaze: '%s'] " format, \
	     obs_source_get_name(context->source), ##__VA_ARGS__)

#define debug(format, ...) blog(LOG_DEBUG, format, ##__VA_ARGS__)
#define info(format, ...) blog(LOG_INFO, format, ##__VA_ARGS__)
#define warn(format, ...) blog(LOG_WARNING, format, ##__VA_ARGS__)

struct gaze {
	obs_source_t *source;

	char *file;
	bool persistent;
	bool is_slide;
	bool linear_alpha;
	time_t file_timestamp;
	float update_time_elapsed;
	uint64_t last_time;
	bool active;
	bool restart_gif;
	volatile bool file_decoded;
	volatile bool texture_loaded;

	gs_image_file4_t if4;
	tobii_api_t *api;
	tobii_device_t *device;
	bool subscribed;
	float x;
	float y;
};

static time_t get_modified_timestamp(const char *filename)
{
	struct stat stats;
	if (os_stat(filename, &stats) != 0)
		return -1;
	return stats.st_mtime;
}

static const char *gaze_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ImageInput");
}

void gaze_preload_image(void *data)
{
	struct gaze *context = data;
	if (os_atomic_load_bool(&context->file_decoded))
		return;

	context->file_timestamp = get_modified_timestamp(context->file);
	gs_image_file4_init(&context->if4, context->file,
			    context->linear_alpha
				    ? GS_IMAGE_ALPHA_PREMULTIPLY_SRGB
				    : GS_IMAGE_ALPHA_PREMULTIPLY);
	os_atomic_set_bool(&context->file_decoded, true);
}

static void gaze_load_texture(void *data)
{
	struct gaze *context = data;
	if (os_atomic_load_bool(&context->texture_loaded))
		return;

	debug("loading texture '%s'", context->file);

	obs_enter_graphics();
	gs_image_file4_init_texture(&context->if4);
	obs_leave_graphics();

	if (!context->if4.image3.image2.image.loaded)
		warn("failed to load texture '%s'", context->file);
	context->update_time_elapsed = 0;
	os_atomic_set_bool(&context->texture_loaded, true);
}

static void gaze_unload(void *data)
{
	struct gaze *context = data;
	os_atomic_set_bool(&context->file_decoded, false);
	os_atomic_set_bool(&context->texture_loaded, false);

	obs_enter_graphics();
	gs_image_file4_free(&context->if4);
	obs_leave_graphics();
}

static void gaze_load(struct gaze *context)
{
	gaze_unload(context);

	if (context->file && *context->file) {
		gaze_preload_image(context);
		gaze_load_texture(context);
	}
}

static void url_receiver(char const *url, void *user_data)
{
	char *buffer = (char *)user_data;
	if (*buffer != '\0')
		return; // only keep first value

	if (strlen(url) < 256)
		strcpy(buffer, url);
}

void gaze_point_callback(tobii_gaze_point_t const *gaze_point, void *user_data)
{
	struct gaze *ptr = (struct gaze *)user_data;
	if (gaze_point->validity == TOBII_VALIDITY_VALID) {
		ptr->x = gaze_point->position_xy[0];
		ptr->y = gaze_point->position_xy[1];
	}
}
static void gaze_update(void *data, obs_data_t *settings)
{
	struct gaze *context = data;
	const char *file = obs_data_get_string(settings, "file");
	const bool unload = obs_data_get_bool(settings, "unload");
	const bool linear_alpha = obs_data_get_bool(settings, "linear_alpha");
	const bool is_slide = obs_data_get_bool(settings, "is_slide");

	if (context->file)
		bfree(context->file);
	context->file = bstrdup(file);
	context->persistent = !unload;
	context->linear_alpha = linear_alpha;
	context->is_slide = is_slide;

	if (is_slide)
		return;

	/* Load the image if the source is persistent or showing */
	if (context->persistent || obs_source_showing(context->source))
		gaze_load(data);
	else
		gaze_unload(data);

	if (context->device) {
		tobii_device_destroy(context->device);
		context->device = NULL;
	}

	if (context->api) {
		tobii_api_destroy(context->api);
		context->api = NULL;
	}

	tobii_error_t error = tobii_api_create(&context->api, NULL, NULL);
	if (error != TOBII_ERROR_NO_ERROR) {
		return;
	}
	char url[256] = {0};
	error = tobii_enumerate_local_device_urls(context->api, url_receiver, url);
	assert(error == TOBII_ERROR_NO_ERROR && *url != '\0');

	if (error != TOBII_ERROR_NO_ERROR) {
		return;
	}
	error = tobii_device_create(context->api, url, &context->device);
	if (error != TOBII_ERROR_NO_ERROR) {
		return;
	}
	error = tobii_gaze_point_subscribe(context->device, gaze_point_callback, context);
}

static void gaze_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "unload", false);
	obs_data_set_default_bool(settings, "linear_alpha", false);
}

static void gaze_show(void *data)
{
	struct gaze *context = data;

	if (!context->persistent && !context->is_slide)
		gaze_load(context);
}

static void gaze_hide(void *data)
{
	struct gaze *context = data;

	if (!context->persistent && !context->is_slide)
		gaze_unload(context);
}

static void restart_gif(void *data)
{
	struct gaze *context = data;

	if (context->if4.image3.image2.image.is_animated_gif) {
		context->if4.image3.image2.image.cur_frame = 0;
		context->if4.image3.image2.image.cur_loop = 0;
		context->if4.image3.image2.image.cur_time = 0;

		obs_enter_graphics();
		gs_image_file4_update_texture(&context->if4);
		obs_leave_graphics();

		context->restart_gif = false;
	}
}

static void gaze_activate(void *data)
{
	struct gaze *context = data;
	context->restart_gif = true;
}

static void *gaze_create(obs_data_t *settings, obs_source_t *source)
{
	struct gaze *context = bzalloc(sizeof(struct gaze));
	context->source = source;
	context->api = NULL;
	context->device = NULL;
	context->subscribed = false;
	context->x = NAN;
	context->y = NAN;

	gaze_update(context, settings);
	return context;
}

static void gaze_destroy(void *data)
{
	struct gaze *context = data;

	gaze_unload(context);

	if (context->file)
		bfree(context->file);
	bfree(context);

	if (context->device) {
		tobii_device_destroy(context->device);
		context->device = NULL;
	}
	if (context->api) {
		tobii_api_destroy(context->api);
		context->api = NULL;
	}
}

static uint32_t gaze_getwidth(void *data)
{
	(void)data;
	return 2560;
}

static uint32_t gaze_getheight(void *data)
{
	(void)data;
	return 1600;
}

static void gaze_render(void *data, gs_effect_t *effect)
{
	struct gaze *context = data;
	if (!os_atomic_load_bool(&context->texture_loaded))
		return;

	struct gs_image_file *const image = &context->if4.image3.image2.image;
	gs_texture_t *const texture = image->texture;
	if (!texture)
		return;

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	gs_eparam_t *const param = gs_effect_get_param_by_name(effect, "image");

	if (isnanf(context->x) || isnanf(context->y)) {
		return;
	}

	gs_effect_set_texture_srgb(param, texture);

	gs_matrix_push();

	gs_matrix_identity();

	float width = obs_source_get_width(context->source);
	float height = obs_source_get_height(context->source);
	gs_matrix_translate3f(context->x * width - image->cx / 2.0f, context->y * height - image->cy / 2.0f, 0);

	gs_draw_sprite(texture, 0, image->cx, image->cy);

	gs_matrix_pop();
	gs_blend_state_pop();

	gs_enable_framebuffer_srgb(previous);
}

static void gaze_tick(void *data, float seconds)
{
	struct gaze *context = data;
	if (!os_atomic_load_bool(&context->texture_loaded)) {
		if (os_atomic_load_bool(&context->file_decoded))
			gaze_load_texture(context);
		else
			return;
	}

	uint64_t frame_time = obs_get_video_frame_time();

	context->update_time_elapsed += seconds;

	if (obs_source_showing(context->source)) {
		if (context->update_time_elapsed >= 1.0f) {
			time_t t = get_modified_timestamp(context->file);
			context->update_time_elapsed = 0.0f;

			if (context->file_timestamp != t) {
				gaze_load(context);
			}
		}
	}

	if (obs_source_showing(context->source)) {
		if (!context->active) {
			if (context->if4.image3.image2.image.is_animated_gif)
				context->last_time = frame_time;
			context->active = true;
		}

		if (context->restart_gif)
			restart_gif(context);

	} else {
		if (context->active) {
			restart_gif(context);
			context->active = false;
		}

		return;
	}

	if (context->last_time &&
	    context->if4.image3.image2.image.is_animated_gif) {
		uint64_t elapsed = frame_time - context->last_time;
		bool updated = gs_image_file4_tick(&context->if4, elapsed);

		if (updated) {
			obs_enter_graphics();
			gs_image_file4_update_texture(&context->if4);
			obs_leave_graphics();
		}
	}

	context->last_time = frame_time;

	const int max_receive = 10;
	for (int i = 0; i < max_receive; ++i) {
		tobii_error_t error = tobii_device_process_callbacks(context->device);

		if (error != TOBII_ERROR_NO_ERROR) {
			context->subscribed = false;
			break;
		}
	}
}

static const char *image_filter =
#ifdef _WIN32
	"All formats (*.bmp *.tga *.png *.jpeg *.jpg *.jxr *.gif *.psd *.webp);;"
#else
	"All formats (*.bmp *.tga *.png *.jpeg *.jpg *.gif *.psd *.webp);;"
#endif
	"BMP Files (*.bmp);;"
	"Targa Files (*.tga);;"
	"PNG Files (*.png);;"
	"JPEG Files (*.jpeg *.jpg);;"
#ifdef _WIN32
	"JXR Files (*.jxr);;"
#endif
	"GIF Files (*.gif);;"
	"PSD Files (*.psd);;"
	"WebP Files (*.webp);;"
	"All Files (*.*)";

static obs_properties_t *gaze_properties(void *data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();

	obs_properties_add_path(props, "file", obs_module_text("File"),
				OBS_PATH_FILE, image_filter, NULL);
	obs_properties_add_bool(props, "unload",
				obs_module_text("UnloadWhenNotShowing"));
	obs_properties_add_bool(props, "linear_alpha",
				obs_module_text("LinearAlpha"));

	return props;
}

uint64_t gaze_get_memory_usage(void *data)
{
	struct gaze *s = data;
	return s->if4.image3.image2.mem_usage;
}

static void missing_file_callback(void *src, const char *new_path, void *data)
{
	struct gaze *s = src;

	obs_source_t *source = s->source;
	obs_data_t *settings = obs_source_get_settings(source);
	obs_data_set_string(settings, "file", new_path);
	obs_source_update(source, settings);
	obs_data_release(settings);

	UNUSED_PARAMETER(data);
}

static obs_missing_files_t *gaze_missingfiles(void *data)
{
	struct gaze *s = data;
	obs_missing_files_t *files = obs_missing_files_create();

	if (strcmp(s->file, "") != 0) {
		if (!os_file_exists(s->file)) {
			obs_missing_file_t *file = obs_missing_file_create(
				s->file, missing_file_callback,
				OBS_MISSING_FILE_SOURCE, s->source, NULL);

			obs_missing_files_add_file(files, file);
		}
	}

	return files;
}

static enum gs_color_space
gaze_get_color_space(void *data, size_t count,
		     const enum gs_color_space *preferred_spaces)
{
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(preferred_spaces);

	struct gaze *const s = data;
	gs_image_file4_t *const if4 = &s->if4;
	return if4->image3.image2.image.texture ? if4->space : GS_CS_SRGB;
}

static struct obs_source_info gaze_info = {
	.id = "gaze",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = gaze_get_name,
	.create = gaze_create,
	.destroy = gaze_destroy,
	.update = gaze_update,
	.get_defaults = gaze_defaults,
	.show = gaze_show,
	.hide = gaze_hide,
	.get_width = gaze_getwidth,
	.get_height = gaze_getheight,
	.video_render = gaze_render,
	.video_tick = gaze_tick,
	.missing_files = gaze_missingfiles,
	.get_properties = gaze_properties,
	.icon_type = OBS_ICON_TYPE_IMAGE,
	.activate = gaze_activate,
	.video_get_color_space = gaze_get_color_space,
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("gaze", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "gaze";
}

extern struct obs_source_info color_source_gaze_info_v1;
extern struct obs_source_info color_source_gaze_info_v2;
extern struct obs_source_info color_source_gaze_info_v3;

bool obs_module_load(void)
{
	obs_register_source(&gaze_info);
	obs_register_source(&color_source_gaze_info_v1);
	obs_register_source(&color_source_gaze_info_v2);
	obs_register_source(&color_source_gaze_info_v3);
	return true;
}
