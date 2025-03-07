#include <memory>
#include <string>
#include <vector>

#include "drake/bindings/pydrake/common/cpp_template_pybind.h"
#include "drake/bindings/pydrake/common/deprecation_pybind.h"
#include "drake/bindings/pydrake/common/eigen_geometry_pybind.h"
#include "drake/bindings/pydrake/common/eigen_pybind.h"
#include "drake/bindings/pydrake/common/serialize_pybind.h"
#include "drake/bindings/pydrake/common/type_pack.h"
#include "drake/bindings/pydrake/common/value_pybind.h"
#include "drake/bindings/pydrake/documentation_pybind.h"
#include "drake/bindings/pydrake/pydrake_pybind.h"
#include "drake/common/drake_throw.h"
#include "drake/common/eigen_types.h"
#include "drake/systems/sensors/camera_config_functions.h"
#include "drake/systems/sensors/camera_info.h"
#include "drake/systems/sensors/image.h"
#include "drake/systems/sensors/image_to_lcm_image_array_t.h"
#include "drake/systems/sensors/image_writer.h"
#include "drake/systems/sensors/lcm_image_array_to_images.h"
#include "drake/systems/sensors/pixel_types.h"
#include "drake/systems/sensors/rgbd_sensor.h"
#include "drake/systems/sensors/rgbd_sensor_async.h"
#include "drake/systems/sensors/rgbd_sensor_discrete.h"

using std::string;
using std::unique_ptr;
using std::vector;

namespace drake {
namespace pydrake {

template <typename T, T kPixelType>
using constant = std::integral_constant<T, kPixelType>;

template <typename T, T... kPixelTypes>
using constant_pack = type_pack<type_pack<constant<T, kPixelTypes>>...>;

using Eigen::Map;
using Eigen::Matrix3d;
using Eigen::Vector3d;
using geometry::FrameId;
using geometry::render::ColorRenderCamera;
using geometry::render::DepthRenderCamera;
using math::RigidTransformd;
using math::RollPitchYawd;

PYBIND11_MODULE(sensors, m) {
  PYDRAKE_PREVENT_PYTHON3_MODULE_REIMPORT(m);

  // NOLINTNEXTLINE(build/namespaces): Emulate placement in namespace.
  using namespace drake::systems;
  // NOLINTNEXTLINE(build/namespaces): Emulate placement in namespace.
  using namespace drake::systems::sensors;
  constexpr auto& doc = pydrake_doc.drake.systems.sensors;

  m.doc() = "Bindings for the sensors portion of the Systems framework.";

  py::module::import("pydrake.common.eigen_geometry");
  py::module::import("pydrake.common.schema");
  py::module::import("pydrake.geometry");
  py::module::import("pydrake.systems.framework");

  // Expose only types that are used.
  py::enum_<PixelFormat>(m, "PixelFormat")
      .value("kRgba", PixelFormat::kRgba)
      .value("kDepth", PixelFormat::kDepth)
      .value("kLabel", PixelFormat::kLabel);

  vector<string> pixel_type_names = {
      "kRgba8U",
      "kDepth16U",
      "kDepth32F",
      "kLabel16I",
  };

  // This list should match pixel_type_names.
  using PixelTypeList = constant_pack<PixelType,  //
      PixelType::kRgba8U,                         //
      PixelType::kDepth16U,                       //
      PixelType::kDepth32F,                       //
      PixelType::kLabel16I>;

  {
    // Expose image types and their traits.
    py::enum_<PixelType> pixel_type(m, "PixelType");

    // This uses the `type_visit` pattern for looping. See `type_pack_test.cc`
    // for more information on the pattern.
    int pixel_type_index = 0;
    auto instantiation_visitor = [&](auto param) {
      // Extract information from inferred parameter.
      constexpr PixelType kPixelType =
          decltype(param)::template type_at<0>::value;
      using ImageT = Image<kPixelType>;
      using ImageTraitsT = ImageTraits<kPixelType>;
      using T = typename ImageTraitsT::ChannelType;

      // Get associated properties, and iterate.
      const std::string pixel_type_name = pixel_type_names[pixel_type_index];
      ++pixel_type_index;

      // Add definition to enum, before requesting the Python parameter.
      pixel_type.value(pixel_type_name.c_str(), kPixelType);
      py::tuple py_param = GetPyParam(param);

      // Add traits.
      py::class_<ImageTraitsT> traits(
          m, TemporaryClassName<ImageTraitsT>().c_str());
      traits.attr("ChannelType") = GetPyParam<T>()[0];
      traits.attr("kNumChannels") = int{ImageTraitsT::kNumChannels};
      traits.attr("kPixelFormat") = PixelFormat{ImageTraitsT::kPixelFormat};
      AddTemplateClass(m, "ImageTraits", traits, py_param);

      auto at = [](ImageT* self, int x, int y) {
        // Since Image<>::at(...) uses DRAKE_ASSERT for performance reasons,
        // rewrite the checks here using DRAKE_THROW_UNLESS so that it will not
        // segfault in Python.
        DRAKE_THROW_UNLESS(x >= 0 && x < self->width());
        DRAKE_THROW_UNLESS(y >= 0 && y < self->height());
        Map<VectorX<T>> pixel(self->at(x, y), int{ImageTraitsT::kNumChannels});
        return pixel;
      };
      // Shape for use with NumPy, OpenCV, etc. Using same shape as what is
      // present in `show_images.py`.
      auto get_shape = [](const ImageT* self) {
        return py::make_tuple(
            self->height(), self->width(), int{ImageTraitsT::kNumChannels});
      };
      auto get_data = [=](const ImageT* self) {
        py::object array = ToArray(self->at(0, 0), self->size(),
            get_shape(self), py_rvp::reference_internal, py::cast(self));
        return array;
      };
      auto get_mutable_data = [=](ImageT* self) {
        py::object array = ToArray(self->at(0, 0), self->size(),
            get_shape(self), py_rvp::reference_internal, py::cast(self));
        return array;
      };

      py::class_<ImageT> image(m, TemporaryClassName<ImageT>().c_str());
      AddTemplateClass(m, "Image", image, py_param);
      image  // BR
          .def(py::init<>(), doc.Image.ctor.doc_0args)
          .def(py::init<int, int>(), py::arg("width"), py::arg("height"),
              doc.Image.ctor.doc_2args)
          .def(py::init<int, int, T>(), py::arg("width"), py::arg("height"),
              py::arg("initial_value"), doc.Image.ctor.doc_3args)
          .def("width", &ImageT::width, doc.Image.width.doc)
          .def("height", &ImageT::height, doc.Image.height.doc)
          .def("size", &ImageT::size, doc.Image.size.doc)
          .def("resize", &ImageT::resize, doc.Image.resize.doc)
          .def("at", at, py::arg("x"), py::arg("y"), py_rvp::reference_internal,
              doc.Image.at.doc_2args_x_y_nonconst)
          // Non-C++ properties. Make them Pythonic.
          .def_property_readonly("shape", get_shape)
          .def_property_readonly("data", get_data)
          .def_property_readonly("mutable_data", get_mutable_data);
      // Constants.
      image.attr("Traits") = traits;
      // - Do not duplicate aliases (e.g. `kNumChannels`) for now.
      // Add type alias for instantiation.
      const std::string suffix = pixel_type_name.substr(1);
      m.attr(("Image" + suffix).c_str()) = image;
      // Add abstract values.
      AddValueInstantiation<ImageT>(m);
    };
    type_visit(instantiation_visitor, PixelTypeList{});
  }

  // Image conversion functions.
  m  // BR
      .def("ConvertDepth32FTo16U", &ConvertDepth32FTo16U, py::arg("input"),
          py::arg("output"))
      .def("ConvertDepth16UTo32F", &ConvertDepth16UTo32F, py::arg("input"),
          py::arg("output"));

  using T = double;

  // Systems.

  auto def_camera_ports = [](auto* ppy_class, auto cls_doc) {
    auto& py_class = *ppy_class;
    using PyClass = std::decay_t<decltype(py_class)>;
    using Class = typename PyClass::type;
    py_class
        .def("query_object_input_port", &Class::query_object_input_port,
            py_rvp::reference_internal, cls_doc.query_object_input_port.doc)
        .def("color_image_output_port", &Class::color_image_output_port,
            py_rvp::reference_internal, cls_doc.color_image_output_port.doc)
        .def("depth_image_32F_output_port", &Class::depth_image_32F_output_port,
            py_rvp::reference_internal, cls_doc.depth_image_32F_output_port.doc)
        .def("depth_image_16U_output_port", &Class::depth_image_16U_output_port,
            py_rvp::reference_internal, cls_doc.depth_image_16U_output_port.doc)
        .def("label_image_output_port", &Class::label_image_output_port,
            py_rvp::reference_internal, cls_doc.label_image_output_port.doc)
        .def("body_pose_in_world_output_port",
            &Class::body_pose_in_world_output_port, py_rvp::reference_internal,
            cls_doc.body_pose_in_world_output_port.doc)
        .def("image_time_output_port", &Class::image_time_output_port,
            py_rvp::reference_internal, cls_doc.image_time_output_port.doc);
  };

  py::class_<RgbdSensor, LeafSystem<T>> rgbd_sensor(
      m, "RgbdSensor", doc.RgbdSensor.doc);

  rgbd_sensor
      .def(py::init<FrameId, const RigidTransformd&, ColorRenderCamera,
               DepthRenderCamera>(),
          py::arg("parent_id"), py::arg("X_PB"), py::arg("color_camera"),
          py::arg("depth_camera"),
          doc.RgbdSensor.ctor.doc_individual_intrinsics)
      .def(py::init<FrameId, const RigidTransformd&, const DepthRenderCamera&,
               bool>(),
          py::arg("parent_id"), py::arg("X_PB"), py::arg("depth_camera"),
          py::arg("show_window") = false,
          doc.RgbdSensor.ctor.doc_combined_intrinsics)
      .def("color_camera_info", &RgbdSensor::color_camera_info,
          py_rvp::reference_internal, doc.RgbdSensor.color_camera_info.doc)
      .def("depth_camera_info", &RgbdSensor::depth_camera_info,
          py_rvp::reference_internal, doc.RgbdSensor.depth_camera_info.doc)
      .def("X_BC", &RgbdSensor::X_BC, doc.RgbdSensor.X_BC.doc)
      .def("X_BD", &RgbdSensor::X_BD, doc.RgbdSensor.X_BD.doc)
      .def("parent_frame_id", &RgbdSensor::parent_frame_id,
          py_rvp::reference_internal, doc.RgbdSensor.parent_frame_id.doc);
  def_camera_ports(&rgbd_sensor, doc.RgbdSensor);

  py::class_<RgbdSensorDiscrete, Diagram<T>> rgbd_camera_discrete(
      m, "RgbdSensorDiscrete", doc.RgbdSensorDiscrete.doc);
  rgbd_camera_discrete
      .def(py::init<unique_ptr<RgbdSensor>, double, bool>(), py::arg("sensor"),
          py::arg("period") = double{RgbdSensorDiscrete::kDefaultPeriod},
          py::arg("render_label_image") = true,
          // Keep alive, ownership: `sensor` keeps `self` alive.
          py::keep_alive<2, 1>(), doc.RgbdSensorDiscrete.ctor.doc)
      // N.B. Since `camera` is already connected, we do not need additional
      // `keep_alive`s.
      .def("sensor", &RgbdSensorDiscrete::sensor, py_rvp::reference_internal,
          doc.RgbdSensorDiscrete.sensor.doc)
      .def("period", &RgbdSensorDiscrete::period,
          doc.RgbdSensorDiscrete.period.doc);
  def_camera_ports(&rgbd_camera_discrete, doc.RgbdSensorDiscrete);
  rgbd_camera_discrete.attr("kDefaultPeriod") =
      double{RgbdSensorDiscrete::kDefaultPeriod};

  {
    using Class = RgbdSensorAsync;
    constexpr auto& cls_doc = doc.RgbdSensorAsync;
    py::class_<Class, LeafSystem<T>>(m, "RgbdSensorAsync", cls_doc.doc)
        .def(py::init<const geometry::SceneGraph<double>*, FrameId,
                 const math::RigidTransformd&, double, double, double,
                 std::optional<ColorRenderCamera>,
                 std::optional<DepthRenderCamera>, bool>(),
            py::arg("scene_graph"), py::arg("parent_id"), py::arg("X_PB"),
            py::arg("fps"), py::arg("capture_offset"), py::arg("output_delay"),
            py::arg("color_camera"), py::arg("depth_camera") = std::nullopt,
            py::arg("render_label_image") = false, cls_doc.ctor.doc)
        .def("parent_id", &Class::parent_id, cls_doc.parent_id.doc)
        .def("X_PB", &Class::X_PB, cls_doc.X_PB.doc)
        .def("fps", &Class::fps, cls_doc.fps.doc)
        .def("capture_offset", &Class::capture_offset,
            cls_doc.capture_offset.doc)
        .def("output_delay", &Class::output_delay, cls_doc.output_delay.doc)
        .def("color_camera", &Class::color_camera, cls_doc.color_camera.doc)
        .def("depth_camera", &Class::depth_camera, cls_doc.depth_camera.doc)
        .def("color_image_output_port", &Class::color_image_output_port,
            py_rvp::reference_internal, cls_doc.color_image_output_port.doc)
        .def("depth_image_32F_output_port", &Class::depth_image_32F_output_port,
            py_rvp::reference_internal, cls_doc.depth_image_32F_output_port.doc)
        .def("depth_image_16U_output_port", &Class::depth_image_16U_output_port,
            py_rvp::reference_internal, cls_doc.depth_image_16U_output_port.doc)
        .def("label_image_output_port", &Class::label_image_output_port,
            py_rvp::reference_internal, cls_doc.label_image_output_port.doc)
        .def("body_pose_in_world_output_port",
            &Class::body_pose_in_world_output_port, py_rvp::reference_internal,
            cls_doc.body_pose_in_world_output_port.doc)
        .def("image_time_output_port", &Class::image_time_output_port,
            py_rvp::reference_internal, cls_doc.image_time_output_port.doc);
  }

  {
    // To bind nested serializable structs without errors, we declare the outer
    // struct first, then bind its inner structs, then bind the outer struct.
    constexpr auto& config_cls_doc = doc.CameraConfig;
    py::class_<CameraConfig> config_cls(m, "CameraConfig", config_cls_doc.doc);

    // Inner struct.
    constexpr auto& fov_degrees_doc = doc.CameraConfig.FovDegrees;
    py::class_<CameraConfig::FovDegrees> fov_class(
        config_cls, "FovDegrees", fov_degrees_doc.doc);
    fov_class  // BR
        .def(ParamInit<CameraConfig::FovDegrees>());
    DefAttributesUsingSerialize(&fov_class, fov_degrees_doc);
    DefReprUsingSerialize(&fov_class);
    DefCopyAndDeepCopy(&fov_class);

    // Inner struct.
    constexpr auto& focal_doc = doc.CameraConfig.FocalLength;
    py::class_<CameraConfig::FocalLength> focal_class(
        config_cls, "FocalLength", focal_doc.doc);
    focal_class  // BR
        .def(ParamInit<CameraConfig::FocalLength>());
    DefAttributesUsingSerialize(&focal_class, focal_doc);
    DefReprUsingSerialize(&focal_class);
    DefCopyAndDeepCopy(&focal_class);

    // Now we can bind the outer struct (see above).
    config_cls  // BR
        .def(ParamInit<CameraConfig>())
        .def("focal_x", &CameraConfig::focal_x, config_cls_doc.focal_x.doc)
        .def("focal_y", &CameraConfig::focal_y, config_cls_doc.focal_y.doc)
        .def("principal_point", &CameraConfig::principal_point,
            config_cls_doc.principal_point.doc)
        .def("MakeCameras", &CameraConfig::MakeCameras,
            config_cls_doc.MakeCameras.doc);
    DefAttributesUsingSerialize(&config_cls, config_cls_doc);
    DefReprUsingSerialize(&config_cls);
    DefCopyAndDeepCopy(&config_cls);

    m.def("ApplyCameraConfig",
        py::overload_cast<const CameraConfig&, DiagramBuilder<double>*,
            const systems::lcm::LcmBuses*,
            const multibody::MultibodyPlant<double>*,
            geometry::SceneGraph<double>*, drake::lcm::DrakeLcmInterface*>(
            &ApplyCameraConfig),
        py::arg("config"), py::arg("builder"), py::arg("lcm_buses") = nullptr,
        py::arg("plant") = nullptr, py::arg("scene_graph") = nullptr,
        py::arg("lcm") = nullptr,
        // Keep alive, reference: `builder` keeps `lcm` alive.
        py::keep_alive<2, 6>(), doc.ApplyCameraConfig.doc);
  }

  {
    using Class = CameraInfo;
    constexpr auto& cls_doc = doc.CameraInfo;
    py::class_<Class> cls(m, "CameraInfo", cls_doc.doc);
    cls  // BR
        .def(py::init<int, int, double>(), py::arg("width"), py::arg("height"),
            py::arg("fov_y"), cls_doc.ctor.doc_3args_width_height_fov_y)
        .def(py::init<int, int, const Matrix3d&>(), py::arg("width"),
            py::arg("height"), py::arg("intrinsic_matrix"),
            cls_doc.ctor.doc_3args_width_height_intrinsic_matrix)
        .def(py::init<int, int, double, double, double, double>(),
            py::arg("width"), py::arg("height"), py::arg("focal_x"),
            py::arg("focal_y"), py::arg("center_x"), py::arg("center_y"),
            cls_doc.ctor
                .doc_6args_width_height_focal_x_focal_y_center_x_center_y)
        .def("width", &Class::width, cls_doc.width.doc)
        .def("height", &Class::height, cls_doc.height.doc)
        .def("focal_x", &Class::focal_x, cls_doc.focal_x.doc)
        .def("focal_y", &Class::focal_y, cls_doc.focal_y.doc)
        .def("fov_x", &Class::fov_x, cls_doc.fov_x.doc)
        .def("fov_y", &Class::fov_y, cls_doc.fov_y.doc)
        .def("center_x", &Class::center_x, cls_doc.center_x.doc)
        .def("center_y", &Class::center_y, cls_doc.center_y.doc)
        .def("intrinsic_matrix", &Class::intrinsic_matrix,
            cls_doc.intrinsic_matrix.doc)
        .def(py::pickle(
            [](const Class& self) {
              return py::make_tuple(self.width(), self.height(), self.focal_x(),
                  self.focal_y(), self.center_x(), self.center_y());
            },
            [](py::tuple t) {
              DRAKE_DEMAND(t.size() == 6);
              return Class(t[0].cast<int>(), t[1].cast<int>(),
                  t[2].cast<double>(), t[3].cast<double>(), t[4].cast<double>(),
                  t[5].cast<double>());
            }));
  }

  {
    using Class = LcmImageArrayToImages;
    constexpr auto& cls_doc = doc.LcmImageArrayToImages;
    py::class_<Class, LeafSystem<double>> cls(
        m, "LcmImageArrayToImages", cls_doc.doc);
    cls  // BR
        .def(py::init<>(), cls_doc.ctor.doc)
        .def("image_array_t_input_port", &Class::image_array_t_input_port,
            py_rvp::reference_internal, cls_doc.image_array_t_input_port.doc)
        .def("color_image_output_port", &Class::color_image_output_port,
            py_rvp::reference_internal, cls_doc.color_image_output_port.doc)
        .def("depth_image_output_port", &Class::depth_image_output_port,
            py_rvp::reference_internal, cls_doc.depth_image_output_port.doc);
  }

  {
    using Class = ImageToLcmImageArrayT;
    constexpr auto& cls_doc = doc.ImageToLcmImageArrayT;
    py::class_<Class, LeafSystem<T>> cls(
        m, "ImageToLcmImageArrayT", cls_doc.doc);
    cls  // BR
        .def(py::init<const string&, const string&, const string&, bool>(),
            py::arg("color_frame_name"), py::arg("depth_frame_name"),
            py::arg("label_frame_name"), py::arg("do_compress") = false,
            cls_doc.ctor.doc_4args)
        .def(py::init<bool>(), py::arg("do_compress") = false,
            cls_doc.ctor.doc_1args)
        .def("color_image_input_port", &Class::color_image_input_port,
            py_rvp::reference_internal, cls_doc.color_image_input_port.doc)
        .def("depth_image_input_port", &Class::depth_image_input_port,
            py_rvp::reference_internal, cls_doc.depth_image_input_port.doc)
        .def("label_image_input_port", &Class::label_image_input_port,
            py_rvp::reference_internal, cls_doc.label_image_input_port.doc)
        .def("image_array_t_msg_output_port",
            &Class::image_array_t_msg_output_port, py_rvp::reference_internal,
            cls_doc.image_array_t_msg_output_port.doc);
    // Because the public interface requires templates and it's hard to
    // reproduce the logic publicly (e.g. no overload that just takes
    // `AbstractValue` and the pixel type), go ahead and bind the templated
    // methods.
    auto def_image_input_port = [&cls, cls_doc](auto param) {
      constexpr PixelType kPixelType =
          decltype(param)::template type_at<0>::value;
      AddTemplateMethod(cls, "DeclareImageInputPort",
          &Class::DeclareImageInputPort<kPixelType>, GetPyParam(param),
          py::arg("name"), py_rvp::reference_internal,
          cls_doc.DeclareImageInputPort.doc);
    };
    type_visit(def_image_input_port, PixelTypeList{});
  }

  {
    using Class = ImageWriter;
    constexpr auto& cls_doc = doc.ImageWriter;
    py::class_<Class, LeafSystem<double>> cls(m, "ImageWriter", cls_doc.doc);
    cls  // BR
        .def(py::init<>(), cls_doc.ctor.doc)
        .def(
            "DeclareImageInputPort",
            [](Class& self, PixelType pixel_type, std::string port_name,
                std::string file_name_format, double publish_period,
                double start_time) {
              self.DeclareImageInputPort(pixel_type, std::move(port_name),
                  std::move(file_name_format), publish_period, start_time);
            },
            py::arg("pixel_type"), py::arg("port_name"),
            py::arg("file_name_format"), py::arg("publish_period"),
            py::arg("start_time"), py_rvp::reference_internal,
            cls_doc.DeclareImageInputPort.doc);
  }
}

}  // namespace pydrake
}  // namespace drake
