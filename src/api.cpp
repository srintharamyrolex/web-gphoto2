/*
 * Copyright 2021 Google LLC
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
 */
#include <emscripten.h>
#include <emscripten/bind.h>
#include <emscripten/val.h>
#include <gphoto2/gphoto2-camera.h>
#include <gphoto2/gphoto2-abilities-list.h>
#include <gphoto2/gphoto2-port-info-list.h>
#include <gphoto2/gphoto2-list.h>
#include <gphoto2/gphoto2-port.h>
#include <stdlib.h>

#include <iostream>

using emscripten::val;

template <typename T, auto Deleter>
using gpp_unique_ptr =
    std::unique_ptr<T, std::integral_constant<decltype(Deleter), Deleter>>;

using GPPWidget = gpp_unique_ptr<CameraWidget, gp_widget_unref>;

void gpp_try(int status) {
  if (status != GP_OK) {
    printf("error: %s\n", gp_result_as_string(status));
    throw std::runtime_error(gp_result_as_string(status));
  }
}

void gpp_log_error(GPContext *context, const char *text, void *data) {
  std::cerr << text << std::endl;
}

#define GPP_CALL(RET, EXPR) \
  ({                        \
    RET OUT;                \
    RET *_ = &OUT;          \
    gpp_try(EXPR);          \
    OUT;                    \
  })

static GPPortInfoList *portinfolist = NULL;
static CameraAbilitiesList *abilities = NULL;

/*
 * This detects all currently attached cameras and returns
 * them in a list. It avoids the generic usb: entry.
 *
 * This function does not open nor initialize the cameras yet.
 */
int init_autodetect(CameraList *list, GPContext *context) {
  gp_list_reset(list);
  gp_camera_autodetect(list, context);
  return gp_list_count(list);
}

const thread_local val Uint8Array = val::global("Uint8Array");
const thread_local val Blob = val::global("Blob");
const thread_local val File = val::global("File");
const thread_local val arrayOf = val::global("Array")["of"];

class Context {
 public:
  Context() : Context(GPP_CALL(Camera *, gp_camera_new(_))) {}
  Context(Camera *camera) : camera(camera) { }

  bool destroyContext() {
    	CameraEventType	evttype;
			CameraFilePath	*path;
			void    	*evtdata;
			int		fd;
			evtdata = NULL;
      int i = 0;
      while (1)
      {
        printf("waiting %i event\n", i++);
        gp_camera_exit(camera.get(), Context::getContext());
        int ret;
        ret = gp_camera_wait_for_event (camera.get(), 2000, &evttype, &evtdata, Context::getContext());
        if (evttype == GP_EVENT_TIMEOUT || ret != GP_OK)
        {
          if (evttype == GP_EVENT_TIMEOUT)
              printf("timeout");
            return true;
        }
      }
      return false;
  }

  val supportedOps() {
    auto ops =
        GPP_CALL(CameraAbilities, gp_camera_get_abilities(camera.get(), _))
            .operations;

    val result = val::object();
    result.set("captureImage", (ops & GP_OPERATION_CAPTURE_IMAGE) != 0);
    result.set("captureVideo", (ops & GP_OPERATION_CAPTURE_VIDEO) != 0);
    result.set("captureAudio", (ops & GP_OPERATION_CAPTURE_AUDIO) != 0);
    result.set("capturePreview", (ops & GP_OPERATION_CAPTURE_PREVIEW) != 0);
    result.set("config", (ops & GP_OPERATION_CONFIG) != 0);
    result.set("triggerCapture", (ops & GP_OPERATION_TRIGGER_CAPTURE) != 0);
    return result;
  }

  bool consumeEvents() {
    bool had_events = false;
    for (;;) {
      CameraEventType event_type = GP_EVENT_UNKNOWN;
      gpp_unique_ptr<void, free> event_data(GPP_CALL(
          void *, gp_camera_wait_for_event(camera.get(), 10000, &event_type, _,
                                           Context::getContext())));
      if (event_type == GP_EVENT_TIMEOUT) {
        break;
      }
      if (event_type == GP_EVENT_UNKNOWN) {
        // EM_ASM({ console.log(UTF8ToString($0)); }, event_data.get());
      }
      had_events = true;
    }
    return had_events;
  }

  val configToJS() {
    GPPWidget config(
        GPP_CALL(CameraWidget *,
                 gp_camera_get_config(camera.get(), _, Context::getContext())));
    return walk_config(config.get()).second;
  }

  void setConfigValue(std::string name, val value) {
    GPPWidget widget(GPP_CALL(
        CameraWidget *, gp_camera_get_single_config(camera.get(), name.c_str(),
                                                    _, Context::getContext())));
    auto type = GPP_CALL(CameraWidgetType, gp_widget_get_type(widget.get(), _));
    switch (type) {
      case GP_WIDGET_RANGE: {
        float number = value.as<float>();
        gpp_try(gp_widget_set_value(widget.get(), &number));
        break;
      }
      case GP_WIDGET_MENU:
      case GP_WIDGET_RADIO:
      case GP_WIDGET_TEXT: {
        auto str = value.as<std::string>();
        gpp_try(gp_widget_set_value(widget.get(), str.c_str()));
        break;
      }
      case GP_WIDGET_TOGGLE: {
        int on = value.as<bool>() ? 1 : 0;
        gpp_try(gp_widget_set_value(widget.get(), &on));
        break;
      }
      case GP_WIDGET_DATE: {
        auto seconds = static_cast<int>(value.as<double>() / 1000);
        gpp_try(gp_widget_set_value(widget.get(), &seconds));
        break;
      }
      default: {
        throw std::logic_error("unimplemented");
      }
    }
    gpp_try(gp_camera_set_single_config(camera.get(), name.c_str(),
                                        widget.get(), Context::getContext()));
  }

  val capturePreviewAsBlob() {
    auto &file = get_file();

    gpp_try(
        gp_camera_capture_preview(camera.get(), &file, Context::getContext()));

    auto params = blob_chunks_and_opts(file);
    return Blob.new_(std::move(params.first), std::move(params.second));
  }

  val captureImageAsFile() {
    auto &file = get_file();

    {
      struct TempCameraFile {
        Camera &camera;
        GPContext &context;
        CameraFilePath path;

        ~TempCameraFile() {
          gp_camera_file_delete(&camera, path.folder, path.name,
                                Context::getContext());
        }
      };

      TempCameraFile camera_file{
          .camera = *camera,
          .context = *(Context::getContext()),
      };

      strcpy(camera_file.path.folder, "/");
      strcpy(camera_file.path.name, "web-gphoto2");

      gpp_try(gp_camera_capture(camera.get(), GP_CAPTURE_IMAGE,
                                &camera_file.path, Context::getContext()));

      gpp_try(gp_camera_file_get(camera.get(), camera_file.path.folder,
                                 camera_file.path.name, GP_FILE_TYPE_NORMAL,
                                 &file, Context::getContext()));
    }

    gpp_try(gp_file_set_name(&file, "image."));
    gpp_try(gp_file_adjust_name_for_mime_type(&file));
    auto name = val(GPP_CALL(const char *, gp_file_get_name(&file, _)));

    auto params = blob_chunks_and_opts(file);
    return File.new_(std::move(params.first), std::move(name),
                     std::move(params.second));
  }

  static val listAvailableCameras() {
    CameraList *list;
    int i;
    const char *name, *value;

    GPContext *context = Context::getContext();
    gp_list_new(&list);
    gp_camera_autodetect(list, context);
    int count = gp_list_count(list);
    val cameras = val::array();
    Camera *camera1;
    CameraAbilities cam_abilities;
    CameraAbilitiesList *abilities;
    GPPortInfoList *portinfolist;
    GPPortInfo port_info;

    gp_port_info_list_new(&portinfolist);
    gp_port_info_list_load(portinfolist);


    gp_abilities_list_new(&abilities);
    gp_abilities_list_load(abilities, context);

    for (i = count - 1; i >= 0; i--) {
      gp_list_get_name(list, i, &name);
      gp_list_get_value(list, i, &value);

      int portIndex = gp_port_info_list_lookup_path(portinfolist, value);
      gp_port_info_list_get_info(portinfolist, portIndex, &port_info);

      gpp_try(gp_camera_new(&camera1));
      gpp_try(gp_camera_set_port_info(camera1, port_info));
      gpp_try(gp_camera_init(camera1, context));

      Context *c = new Context(camera1);
      cameras.call<void>("push", c);
    }

    return cameras;
  }

 private:
  gpp_unique_ptr<Camera, gp_camera_unref> camera;
  gpp_unique_ptr<CameraFile, gp_file_unref> file;

  static GPContext *context;
  static GPContext *getContext() {
    if (!context) {
      context = gp_context_new();
      gp_context_set_error_func(context, gpp_log_error, nullptr);
    }
    return context;
  }

  static std::pair<val, val> walk_config(CameraWidget *widget) {
    val result = val::object();

    val name(GPP_CALL(const char *, gp_widget_get_name(widget, _)));
    result.set("name", name);
    result.set("info", GPP_CALL(const char *, gp_widget_get_info(widget, _)));
    result.set("label", GPP_CALL(const char *, gp_widget_get_label(widget, _)));
    result.set("readonly",
               GPP_CALL(int, gp_widget_get_readonly(widget, _)) != 0);

    auto type = GPP_CALL(CameraWidgetType, gp_widget_get_type(widget, _));

    switch (type) {
      case GP_WIDGET_RANGE: {
        result.set("type", "range");
        result.set("value", GPP_CALL(float, gp_widget_get_value(widget, _)));

        float min, max, step;
        gpp_try(gp_widget_get_range(widget, &min, &max, &step));
        result.set("min", min);
        result.set("max", max);
        result.set("step", step);

        break;
      }
      case GP_WIDGET_MENU:
      case GP_WIDGET_RADIO: {
        result.set("type", type == GP_WIDGET_MENU ? "menu" : "radio");
        result.set("value",
                   GPP_CALL(const char *, gp_widget_get_value(widget, _)));

        val choices = val::array();
        for (int i = 0, n = gp_widget_count_choices(widget); i < n; i++) {
          choices.call<void>(
              "push",
              val(GPP_CALL(const char *, gp_widget_get_choice(widget, i, _))));
        }
        result.set("choices", choices);

        break;
      }
      case GP_WIDGET_TOGGLE: {
        result.set("type", "toggle");
        int value = GPP_CALL(int, gp_widget_get_value(widget, _));
        // Note: explicitly not adding `value` for any other values
        // (e.g. camera actions often would return 2 here...)
        switch (value) {
          case 0:
            result.set("value", false);
            break;
          case 1:
            result.set("value", true);
            break;
        }

        break;
      }
      case GP_WIDGET_TEXT: {
        result.set("type", "text");
        result.set("value",
                   GPP_CALL(const char *, gp_widget_get_value(widget, _)));

        break;
      }
      case GP_WIDGET_WINDOW:
      case GP_WIDGET_SECTION: {
        result.set("type", type == GP_WIDGET_WINDOW ? "window" : "section");

        val children = val::object();
        for (int i = 0, n = gp_widget_count_children(widget); i < n; i++) {
          auto child =
              GPP_CALL(CameraWidget *, gp_widget_get_child(widget, i, _));
          auto kv = walk_config(child);
          children.set(kv.first, kv.second);
        }
        result.set("children", children);

        break;
      }
      case GP_WIDGET_DATE: {
        auto seconds = GPP_CALL(int, gp_widget_get_value(widget, _));
        result.set("type", "datetime");
        result.set("value", static_cast<double>(seconds) * 1000);

        break;
      }
      default: {
        throw std::logic_error("unimplemented");
      }
    }

    return {name, result};
  }

  CameraFile &get_file() {
    if (file.get() == nullptr) {
      file.reset(GPP_CALL(CameraFile *, gp_file_new(_)));
    }
    return *file;
  }

  std::pair<val, val> blob_chunks_and_opts(CameraFile &file) {
    auto mime_type = GPP_CALL(const char *, gp_file_get_mime_type(&file, _));

    const char *data;
    unsigned long size;
    gpp_try(gp_file_get_data_and_size(&file, &data, &size));

    val blob_opts = val::object();
    blob_opts.set("type", mime_type);

    return {arrayOf(Uint8Array.new_(emscripten::typed_memory_view(size, data))),
            std::move(blob_opts)};
  }
};

GPContext *Context::context;

EMSCRIPTEN_BINDINGS(gphoto2_js_api) {
  emscripten::class_<Context>("Context")
      .constructor<>()
      .function("configToJS", &Context::configToJS)
      .function("setConfigValue", &Context::setConfigValue)
      .function("capturePreviewAsBlob", &Context::capturePreviewAsBlob)
      .function("captureImageAsFile", &Context::captureImageAsFile)
      .function("consumeEvents", &Context::consumeEvents)
      .function("supportedOps", &Context::supportedOps)
      .function("destroyContext", &Context::destroyContext)
      .class_function("listAvailableCameras", &Context::listAvailableCameras);
}
