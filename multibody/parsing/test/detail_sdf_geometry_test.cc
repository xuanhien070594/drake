#include "drake/multibody/parsing/detail_sdf_geometry.h"

#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <sdf/Root.hh>
#include <sdf/parser.hh>

#include "drake/common/find_resource.h"
#include "drake/common/fmt_eigen.h"
#include "drake/common/test_utilities/diagnostic_policy_test_base.h"
#include "drake/common/test_utilities/eigen_matrix_compare.h"
#include "drake/common/test_utilities/expect_throws_message.h"
#include "drake/common/text_logging.h"
#include "drake/geometry/geometry_instance.h"
#include "drake/geometry/proximity_properties.h"
#include "drake/geometry/rgba.h"
#include "drake/geometry/scene_graph.h"
#include "drake/math/roll_pitch_yaw.h"
#include "drake/math/rotation_matrix.h"
#include "drake/multibody/parsing/detail_common.h"
#include "drake/multibody/parsing/detail_ignition.h"
#include "drake/multibody/parsing/detail_sdf_diagnostic.h"

namespace drake {
namespace multibody {
namespace internal {
namespace {

using drake::internal::DiagnosticDetail;
using drake::internal::DiagnosticPolicy;
using Eigen::Matrix3d;
using Eigen::Vector3d;
using geometry::Box;
using geometry::Capsule;
using geometry::Convex;
using geometry::Cylinder;
using geometry::Ellipsoid;
using geometry::GeometryInstance;
using geometry::HalfSpace;
using geometry::IllustrationProperties;
using geometry::Mesh;
using geometry::ProximityProperties;
using geometry::Rgba;
using geometry::SceneGraph;
using geometry::Shape;
using geometry::Sphere;
using math::RigidTransformd;
using math::RollPitchYaw;
using math::RotationMatrix;
using multibody::internal::MakeCoulombFrictionFromSdfCollisionOde;
using multibody::internal::MakeGeometryInstanceFromSdfVisual;
using multibody::internal::MakeGeometryPoseFromSdfCollision;
using std::make_unique;
using std::unique_ptr;
using systems::Context;
using systems::LeafSystem;

sdf::ParserConfig MakeStrictConfig() {
  sdf::ParserConfig result;
  result.SetWarningsPolicy(sdf::EnforcementPolicy::ERR);
  result.SetDeprecatedElementsPolicy(sdf::EnforcementPolicy::ERR);
  result.SetUnrecognizedElementsPolicy(sdf::EnforcementPolicy::ERR);
  return result;
}

sdf::SDFPtr ReadString(const std::string& input) {
  sdf::SDFPtr result(new sdf::SDF());
  // TODO(azeey): Use newer DOM API (eg sdf::Root::LoadString) instead of
  // sdf::init and sdf::readString.
  sdf::init(result, sdf::ParserConfig{});
  sdf::ParserConfig config = MakeStrictConfig();
  sdf::Errors errors;
  const bool success = sdf::readString(input, config, result, errors);
  if (!success) {
    for (const auto& error : errors) {
      drake::log()->error("Parse error: {}", fmt_streamed(error));
    }
    // Note that we don't throw here, we just spam the console.  This is not
    // great, but it matches the pre-existing behavior which wants this helper
    // to return a default-constructed value in the case of syntax errors.
  }

  return result;
}

// Helper to create an sdf::Geometry object from its SDF specification given
// as a string. Example of what the string should contain:
//   <cylinder>
//     <radius>0.5</radius>
//     <length>1.2</length>
//   </cylinder>
// and similarly for other SDF geometries.
unique_ptr<sdf::Geometry> MakeSdfGeometryFromString(
    const std::string& geometry_spec) {
  const std::string sdf_str =
      "<?xml version='1.0'?>"
      "<sdf version='1.7'>"
      "  <model name='my_model'>"
      "    <link name='link'>"
      "      <visual name='link_visual'>"
      "        <geometry>" +
      geometry_spec +
      "        </geometry>"
      "      </visual>"
      "    </link>"
      "  </model>"
      "</sdf>";
  sdf::SDFPtr sdf_parsed = ReadString(sdf_str);
  sdf::ElementPtr geometry_element = sdf_parsed->Root()
                                         ->GetElement("model")
                                         ->GetElement("link")
                                         ->GetElement("visual")
                                         ->GetElement("geometry");
  auto sdf_geometry = make_unique<sdf::Geometry>();
  sdf::ParserConfig config = MakeStrictConfig();
  sdf_geometry->Load(geometry_element, config);
  return sdf_geometry;
}

// Helper to create an sdf::Visual object from its SDF specification given
// as a string. Example of what the string should contain:
//       <visual name = 'some_link_visual'>
//         <pose>1.0 2.0 3.0 3.14 6.28 1.57</pose>
//         <geometry>
//           <cylinder>
//             <radius>0.5</radius>
//             <length>1.2</length>
//           </cylinder>
//         </geometry>
//       </visual>
unique_ptr<sdf::Visual> MakeSdfVisualFromString(
    const std::string& visual_spec) {
  const std::string sdf_str =
      "<?xml version='1.0'?>"
      "<sdf version='1.7'>"
      "  <model name='my_model'>"
      "    <link name='link'>" +
      visual_spec +
      "    </link>"
      "  </model>"
      "</sdf>";
  sdf::SDFPtr sdf_parsed = ReadString(sdf_str);
  sdf::ElementPtr visual_element =
      sdf_parsed->Root()->GetElement("model")->GetElement("link")->GetElement(
          "visual");
  auto sdf_visual = make_unique<sdf::Visual>();
  sdf::ParserConfig config = MakeStrictConfig();
  sdf_visual->Load(visual_element, config);
  return sdf_visual;
}

// Helper to create an sdf::Collision object from its SDF specification given
// as a string. Example of what the string should contain:
//       <collision name = 'some_link_collision'>
//         <pose>1.0 2.0 3.0 3.14 6.28 1.57</pose>
//         <geometry>
//           <cylinder>
//             <radius>0.5</radius>
//             <length>1.2</length>
//           </cylinder>
//         </geometry>
//         <drake_friction>
//           <static_friction>0.8</static_friction>
//           <dynamic_friction>0.3</dynamic_friction>
//         </drake_friction>
//       </collision>
unique_ptr<sdf::Collision> MakeSdfCollisionFromString(
    const std::string& collision_spec) {
  const std::string sdf_str =
      "<?xml version='1.0'?>"
      "<sdf version='1.7'>"
      "  <model name='my_model'>"
      "    <link name='link'>" +
      collision_spec +
      "    </link>"
      "  </model>"
      "</sdf>";
  sdf::SDFPtr sdf_parsed = ReadString(sdf_str);
  sdf::ElementPtr collision_element =
      sdf_parsed->Root()->GetElement("model")->GetElement("link")->GetElement(
          "collision");
  auto sdf_collision = make_unique<sdf::Collision>();
  sdf::ParserConfig config = MakeStrictConfig();
  sdf_collision->Load(collision_element, config);
  return sdf_collision;
}

// Define a pass-through functor for testing.
std::string NoopResolveFilename(const SDFormatDiagnostic&,
                                std::string filename) {
  return filename;
}

class SceneGraphParserDetail : public test::DiagnosticPolicyTestBase {
 public:
  // Wraps a function under test with helpful defaults.
  std::unique_ptr<geometry::Shape> MakeShapeFromSdfGeometry(
      const sdf::Geometry& sdf_geometry,
      const ResolveFilename& resolve_filename = &NoopResolveFilename) {
    return internal::MakeShapeFromSdfGeometry(sdf_diagnostic_, sdf_geometry,
                                              resolve_filename);
  }

  // Wraps a function under test with helpful defaults.
  VisualProperties MakeVisualPropertiesFromSdfVisual(
      const sdf::Visual& sdf_visual,
      const ResolveFilename& resolve_filename = &NoopResolveFilename) {
    return internal::MakeVisualPropertiesFromSdfVisual(
        sdf_diagnostic_, sdf_visual, resolve_filename);
  }

 protected:
  const std::string dummy_file_path_{"dummy_test_file.sdf"};
  DataSource data_source_{DataSource::kFilename, &dummy_file_path_};
  SDFormatDiagnostic sdf_diagnostic_{&diagnostic_policy_, &data_source_};
};

// Verify MakeShapeFromSdfGeometry returns nullptr when we specify an <empty>
// sdf::Geometry.
TEST_F(SceneGraphParserDetail, MakeEmptyFromSdfGeometry) {
  unique_ptr<sdf::Geometry> sdf_geometry =
      MakeSdfGeometryFromString("<empty/>");
  unique_ptr<Shape> shape = MakeShapeFromSdfGeometry(*sdf_geometry);
  EXPECT_EQ(shape, nullptr);
}

// Verify MakeShapeFromSdfGeometry can make a box from an sdf::Geometry.
TEST_F(SceneGraphParserDetail, MakeBoxFromSdfGeometry) {
  unique_ptr<sdf::Geometry> sdf_geometry = MakeSdfGeometryFromString(
      "<box>"
      "  <size>1.0 2.0 3.0</size>"
      "</box>");
  unique_ptr<Shape> shape = MakeShapeFromSdfGeometry(*sdf_geometry);
  const Box* box = dynamic_cast<const Box*>(shape.get());
  ASSERT_NE(box, nullptr);
  EXPECT_EQ(box->size(), Vector3d(1.0, 2.0, 3.0));
}

// Verify MakeShapeFromSdfGeometry can make a Drake capsule from an
// sdf::Geometry.
// TODO(azeey): We should deprecate use of <drake:capsule> per
// https://github.com/RobotLocomotion/drake/issues/14837
TEST_F(SceneGraphParserDetail, MakeDrakeCapsuleFromSdfGeometry) {
  unique_ptr<sdf::Geometry> sdf_geometry = MakeSdfGeometryFromString(
      "<drake:capsule>"
      "  <radius>0.5</radius>"
      "  <length>1.2</length>"
      "</drake:capsule>");
  unique_ptr<Shape> shape = MakeShapeFromSdfGeometry(*sdf_geometry);
  const Capsule* capsule = dynamic_cast<const Capsule*>(shape.get());
  ASSERT_NE(capsule, nullptr);
  EXPECT_EQ(capsule->radius(), 0.5);
  EXPECT_EQ(capsule->length(), 1.2);
}

// Verify MakeShapeFromSdfGeometry checks for invalid capsules.
// TODO(azeey): We should deprecate use of <drake:capsule> per
// https://github.com/RobotLocomotion/drake/issues/14837
TEST_F(SceneGraphParserDetail, CheckInvalidDrakeCapsules) {
  unique_ptr<sdf::Geometry> no_radius_geometry = MakeSdfGeometryFromString(
      "<drake:capsule>"
      "  <length>1.2</length>"
      "</drake:capsule>");
  unique_ptr<Shape> shape_no_radius =
      MakeShapeFromSdfGeometry(*no_radius_geometry);
  EXPECT_EQ(shape_no_radius, nullptr);
  EXPECT_THAT(
      TakeError(),
      ::testing::MatchesRegex(
          ".*Element <radius> is required within element <drake:capsule>."));

  unique_ptr<sdf::Geometry> no_length_geometry = MakeSdfGeometryFromString(
      "<drake:capsule>"
      "  <radius>0.5</radius>"
      "</drake:capsule>");
  unique_ptr<Shape> shape_no_length =
      MakeShapeFromSdfGeometry(*no_length_geometry);
  EXPECT_EQ(shape_no_length, nullptr);
  EXPECT_THAT(
      TakeError(),
      ::testing::MatchesRegex(
          ".*Element <length> is required within element <drake:capsule>."));
}

// Verify MakeShapeFromSdfGeometry can make a capsule from an sdf::Geometry.
TEST_F(SceneGraphParserDetail, MakeCapsuleFromSdfGeometry) {
  unique_ptr<sdf::Geometry> sdf_geometry = MakeSdfGeometryFromString(
      "<capsule>"
      "  <radius>0.5</radius>"
      "  <length>1.2</length>"
      "</capsule>");
  unique_ptr<Shape> shape = MakeShapeFromSdfGeometry(*sdf_geometry);
  const Capsule* capsule = dynamic_cast<const Capsule*>(shape.get());
  ASSERT_NE(capsule, nullptr);
  EXPECT_EQ(capsule->radius(), 0.5);
  EXPECT_EQ(capsule->length(), 1.2);
}

// Verify MakeShapeFromSdfGeometry can make a cylinder from an sdf::Geometry.
TEST_F(SceneGraphParserDetail, MakeCylinderFromSdfGeometry) {
  unique_ptr<sdf::Geometry> sdf_geometry = MakeSdfGeometryFromString(
      "<cylinder>"
      "  <radius>0.5</radius>"
      "  <length>1.2</length>"
      "</cylinder>");
  unique_ptr<Shape> shape = MakeShapeFromSdfGeometry(*sdf_geometry);
  const Cylinder* cylinder = dynamic_cast<const Cylinder*>(shape.get());
  ASSERT_NE(cylinder, nullptr);
  EXPECT_EQ(cylinder->radius(), 0.5);
  EXPECT_EQ(cylinder->length(), 1.2);
}

// Verify MakeShapeFromSdfGeometry can make a Drake ellipsoid from an
// sdf::Geometry.
// TODO(azeey): We should deprecate use of <drake:ellipsoid> per
// https://github.com/RobotLocomotion/drake/issues/14837
TEST_F(SceneGraphParserDetail, MakeDrakeEllipsoidFromSdfGeometry) {
  unique_ptr<sdf::Geometry> sdf_geometry = MakeSdfGeometryFromString(
      "<drake:ellipsoid>"
      "  <a>0.5</a>"
      "  <b>1.2</b>"
      "  <c>0.9</c>"
      "</drake:ellipsoid>");
  unique_ptr<Shape> shape = MakeShapeFromSdfGeometry(*sdf_geometry);
  const Ellipsoid* ellipsoid = dynamic_cast<const Ellipsoid*>(shape.get());
  ASSERT_NE(ellipsoid, nullptr);
  EXPECT_EQ(ellipsoid->a(), 0.5);
  EXPECT_EQ(ellipsoid->b(), 1.2);
  EXPECT_EQ(ellipsoid->c(), 0.9);
}

// Verify MakeShapeFromSdfGeometry checks for invalid ellispoids.
// TODO(azeey): We should deprecate use of <drake:ellipsoid> per
// https://github.com/RobotLocomotion/drake/issues/14837
TEST_F(SceneGraphParserDetail, CheckInvalidEllipsoids) {
  unique_ptr<sdf::Geometry> no_a_geometry = MakeSdfGeometryFromString(
      "<drake:ellipsoid>"
      "  <b>1.2</b>"
      "  <c>0.9</c>"
      "</drake:ellipsoid>");
  unique_ptr<Shape> shape_no_a = MakeShapeFromSdfGeometry(*no_a_geometry);
  EXPECT_EQ(shape_no_a, nullptr);
  EXPECT_THAT(
      TakeError(),
      ::testing::MatchesRegex(
          ".*Element <a> is required within element <drake:ellipsoid>."));

  unique_ptr<sdf::Geometry> no_b_geometry = MakeSdfGeometryFromString(
      "<drake:ellipsoid>"
      "  <a>0.5</a>"
      "  <c>0.9</c>"
      "</drake:ellipsoid>");
  unique_ptr<Shape> shape_no_b = MakeShapeFromSdfGeometry(*no_b_geometry);
  EXPECT_EQ(shape_no_b, nullptr);
  EXPECT_THAT(
      TakeError(),
      ::testing::MatchesRegex(
          ".*Element <b> is required within element <drake:ellipsoid>."));

  unique_ptr<sdf::Geometry> no_c_geometry = MakeSdfGeometryFromString(
      "<drake:ellipsoid>"
      "  <a>0.5</a>"
      "  <b>1.2</b>"
      "</drake:ellipsoid>");
  unique_ptr<Shape> shape_no_c = MakeShapeFromSdfGeometry(*no_c_geometry);
  EXPECT_EQ(shape_no_c, nullptr);
  EXPECT_THAT(
      TakeError(),
      ::testing::MatchesRegex(
          ".*Element <c> is required within element <drake:ellipsoid>."));
}

// Verify MakeShapeFromSdfGeometry can make an ellipsoid from an sdf::Geometry.
TEST_F(SceneGraphParserDetail, MakeEllipsoidFromSdfGeometry) {
  unique_ptr<sdf::Geometry> sdf_geometry = MakeSdfGeometryFromString(
      "<ellipsoid>"
      "  <radii>0.5 1.2 0.9</radii>"
      "</ellipsoid>");
  unique_ptr<Shape> shape = MakeShapeFromSdfGeometry(*sdf_geometry);
  const Ellipsoid* ellipsoid = dynamic_cast<const Ellipsoid*>(shape.get());
  ASSERT_NE(ellipsoid, nullptr);
  EXPECT_EQ(ellipsoid->a(), 0.5);
  EXPECT_EQ(ellipsoid->b(), 1.2);
  EXPECT_EQ(ellipsoid->c(), 0.9);
}

// Verify MakeShapeFromSdfGeometry can make a sphere from an sdf::Geometry.
TEST_F(SceneGraphParserDetail, MakeSphereFromSdfGeometry) {
  unique_ptr<sdf::Geometry> sdf_geometry = MakeSdfGeometryFromString(
      "<sphere>"
      "  <radius>0.5</radius>"
      "</sphere>");
  unique_ptr<Shape> shape = MakeShapeFromSdfGeometry(*sdf_geometry);
  const Sphere* sphere = dynamic_cast<const Sphere*>(shape.get());
  ASSERT_NE(sphere, nullptr);
  EXPECT_EQ(sphere->radius(), 0.5);
}

// Verify MakeShapeFromSdfGeometry can make a half space from an sdf::Geometry.
TEST_F(SceneGraphParserDetail, MakeHalfSpaceFromSdfGeometry) {
  unique_ptr<sdf::Geometry> sdf_geometry = MakeSdfGeometryFromString(
      "<plane>"
      "  <normal>1.0 0.0 0.0</normal>"
      "  <size>1.0 1.0 1.0</size>"
      "</plane>");
  // MakeShapeFromSdfGeometry() ignores <normal> and <size> to create the
  // HalfSpace. Therefore we only verify it created the right object.
  unique_ptr<Shape> shape = MakeShapeFromSdfGeometry(*sdf_geometry);
  EXPECT_TRUE(dynamic_cast<const HalfSpace*>(shape.get()) != nullptr);
}

// Verify MakeShapeFromSdfGeometry can make a mesh from an sdf::Geometry.
TEST_F(SceneGraphParserDetail, MakeMeshFromSdfGeometry) {
  // TODO(amcastro-tri): Be warned, the result of this test might (should)
  // change as we add support allowing to specify paths relative to the SDF file
  // location.
  const std::string absolute_file_path = "/path/to/some/mesh.obj";
  unique_ptr<sdf::Geometry> sdf_geometry = MakeSdfGeometryFromString(
      "<mesh>"
      "  <uri>" +
      absolute_file_path +
      "</uri>"
      "  <scale> 3 2 1 </scale>"
      "</mesh>");
  unique_ptr<Shape> shape = MakeShapeFromSdfGeometry(*sdf_geometry);
  const Mesh* mesh = dynamic_cast<const Mesh*>(shape.get());
  ASSERT_NE(mesh, nullptr);
  ASSERT_TRUE(mesh->source().is_path());
  EXPECT_EQ(mesh->source().path(), absolute_file_path);
  EXPECT_TRUE(CompareMatrices(mesh->scale3(), Vector3d(3, 2, 1)));
}

// Verify MakeShapeFromSdfGeometry can make a convex mesh from an sdf::Geometry.
TEST_F(SceneGraphParserDetail, MakeConvexFromSdfGeometry) {
  const std::string absolute_file_path = "/path/to/some/mesh.obj";
  unique_ptr<sdf::Geometry> sdf_geometry = MakeSdfGeometryFromString(
      "<mesh xmlns:drake='http://drake.mit.edu'>"
      "  <drake:declare_convex/>"
      "  <uri>" +
      absolute_file_path +
      "</uri>"
      "  <scale> 3 2 1 </scale>"
      "</mesh>");
  unique_ptr<Shape> shape = MakeShapeFromSdfGeometry(*sdf_geometry);
  const Convex* convex = dynamic_cast<const Convex*>(shape.get());
  ASSERT_NE(convex, nullptr);
  EXPECT_TRUE(convex->source().is_path());
  EXPECT_EQ(convex->source().path(), absolute_file_path);
  // EXPECT_EQ(convex->scale(), 3);
  EXPECT_TRUE(CompareMatrices(convex->scale3(), Vector3d(3, 2, 1)));
}

// Verify that MakeShapeFromSdfGeometry does nothing with a heightmap.
TEST_F(SceneGraphParserDetail, MakeHeightmapFromSdfGeometry) {
  unique_ptr<sdf::Geometry> sdf_geometry = MakeSdfGeometryFromString(
      "<heightmap>"
      "  <uri>/path/to/some/heightmap.png</uri>"
      "</heightmap>");
  unique_ptr<Shape> shape = MakeShapeFromSdfGeometry(*sdf_geometry);
  EXPECT_EQ(shape, nullptr);
}

// Verify that MakeShapeFromSdfGeometry does nothing with a polyline.
TEST_F(SceneGraphParserDetail, MakePolylineFromSdfGeometry) {
  unique_ptr<sdf::Geometry> sdf_geometry = MakeSdfGeometryFromString(
      "<polyline>"
      "  <polyline>"
      "    <point>0 0</point>"
      "    <point>0 1</point>"
      "    <point>1 1</point>"
      "    <point>1 0</point>"
      "    <height>1</height>"
      "  </polyline>"
      "</polyline>");
  std::optional<unique_ptr<Shape>> shape =
      MakeShapeFromSdfGeometry(*sdf_geometry);
  EXPECT_EQ(shape, nullptr);
}

// Verify MakeGeometryInstanceFromSdfVisual can make a GeometryInstance from an
// sdf::Visual.
// Since we test MakeShapeFromSdfGeometry separately, there is no need to unit
// test every combination of a <visual> with a different <geometry>.
TEST_F(SceneGraphParserDetail, MakeGeometryInstanceFromSdfVisual) {
  unique_ptr<sdf::Visual> sdf_visual = MakeSdfVisualFromString(
      "<visual name = 'some_link_visual'>"
      "  <pose>1.0 2.0 3.0 3.14 6.28 1.57</pose>"
      "  <geometry>"
      "    <cylinder>"
      "      <radius>0.5</radius>"
      "      <length>1.2</length>"
      "    </cylinder>"
      "  </geometry>"
      "</visual>");

  unique_ptr<GeometryInstance> geometry_instance =
      MakeGeometryInstanceFromSdfVisual(
          sdf_diagnostic_, *sdf_visual, NoopResolveFilename,
          ToRigidTransform(sdf_visual->RawPose()));

  EXPECT_NE(geometry_instance->perception_properties(), nullptr);
  EXPECT_NE(geometry_instance->illustration_properties(), nullptr);

  const RigidTransformd X_LC(geometry_instance->pose());

  // These are the expected values as specified by the string above.
  const RollPitchYaw<double> expected_rpy(3.14, 6.28, 1.57);
  const RotationMatrix<double> R_LC_expected(expected_rpy);
  const Vector3d p_LCo_expected(1.0, 2.0, 3.0);

  // Verify results to precision given by kTolerance.
  const double kTolerance = 10 * std::numeric_limits<double>::epsilon();
  EXPECT_TRUE(X_LC.rotation().IsNearlyEqualTo(R_LC_expected, kTolerance));
  EXPECT_TRUE(CompareMatrices(X_LC.translation(), p_LCo_expected, kTolerance,
                              MatrixCompareType::relative));
}

// Verify MakeGeometryInstanceFromSdfVisual() creates an instance such that only
// the perception properties have the ("renderer", "accepting") property.
TEST_F(SceneGraphParserDetail,
       MakeGeometryInstanceFromSdfVisualAcceptingRenderer) {
  unique_ptr<sdf::Visual> sdf_visual = MakeSdfVisualFromString(
      "<visual name = 'some_link_visual'>"
      "  <pose>1.0 2.0 3.0 3.14 6.28 1.57</pose>"
      "  <geometry><sphere><radius>1.0</radius></sphere></geometry>"
      "  <drake:accepting_renderer>renderer1</drake:accepting_renderer>"
      "</visual>");

  unique_ptr<GeometryInstance> geometry_instance =
      MakeGeometryInstanceFromSdfVisual(
          sdf_diagnostic_, *sdf_visual, NoopResolveFilename,
          ToRigidTransform(sdf_visual->RawPose()));

  ASSERT_NE(geometry_instance->perception_properties(), nullptr);
  ASSERT_NE(geometry_instance->illustration_properties(), nullptr);

  EXPECT_FALSE(geometry_instance->illustration_properties()->HasProperty(
      "renderer", "accepting"));
  EXPECT_TRUE(geometry_instance->perception_properties()->HasProperty(
      "renderer", "accepting"));
  const auto& names =
      geometry_instance->perception_properties()
          ->GetProperty<std::set<std::string>>("renderer", "accepting");
  EXPECT_EQ(names.size(), 1);
  EXPECT_TRUE(names.contains("renderer1"));
}

// Verify MakeGeometryInstanceFromSdfVisual()'s GeometryInstance reflects the
// enabled visual roles. All we care about is the presence/absence of geometry
// properties based on what was enabled.
TEST_F(SceneGraphParserDetail, MakeGeometryInstanceFromSdfVisualPartialRoles) {
  constexpr std::string_view sdf_format_str = R"""(
    <visual name = 'some_link_visual'>
      <geometry><sphere><radius>1</radius></sphere></geometry>
      <drake:perception_properties enabled="{p_on}"/>
      <drake:illustration_properties enabled="{i_on}"/>
    </visual>)""";

  // Case: Both roles enabled --> both sets of properties.
  {
    unique_ptr<sdf::Visual> sdf_visual = MakeSdfVisualFromString(fmt::format(
        sdf_format_str, fmt::arg("p_on", true), fmt::arg("i_on", true)));
    unique_ptr<GeometryInstance> instance = MakeGeometryInstanceFromSdfVisual(
        sdf_diagnostic_, *sdf_visual, NoopResolveFilename,
        ToRigidTransform(sdf_visual->RawPose()));
    ASSERT_NE(instance, nullptr);
    ASSERT_NE(instance->perception_properties(), nullptr);
    ASSERT_NE(instance->illustration_properties(), nullptr);
  }

  // Case: Illustration disabled --> only perception properties.
  {
    unique_ptr<sdf::Visual> sdf_visual = MakeSdfVisualFromString(fmt::format(
        sdf_format_str, fmt::arg("p_on", true), fmt::arg("i_on", false)));
    unique_ptr<GeometryInstance> instance = MakeGeometryInstanceFromSdfVisual(
        sdf_diagnostic_, *sdf_visual, NoopResolveFilename,
        ToRigidTransform(sdf_visual->RawPose()));
    ASSERT_NE(instance, nullptr);
    ASSERT_NE(instance->perception_properties(), nullptr);
    ASSERT_EQ(instance->illustration_properties(), nullptr);
  }

  // Case: Perception disabled --> only illustration properties.
  {
    unique_ptr<sdf::Visual> sdf_visual = MakeSdfVisualFromString(fmt::format(
        sdf_format_str, fmt::arg("p_on", false), fmt::arg("i_on", true)));
    unique_ptr<GeometryInstance> instance = MakeGeometryInstanceFromSdfVisual(
        sdf_diagnostic_, *sdf_visual, NoopResolveFilename,
        ToRigidTransform(sdf_visual->RawPose()));
    ASSERT_NE(instance, nullptr);
    ASSERT_EQ(instance->perception_properties(), nullptr);
    ASSERT_NE(instance->illustration_properties(), nullptr);
  }

  // Case: Both roles disabled --> no instance returned (but we do get a warning
  // as tested below in DisablingVisualRoles).
  // We don't test the case where we've got both roles disabled for
  // <drake:visual>. The only difference is whether a warning gets emitted, and
  // that's tested in DisablingVisualRoles.
  {
    unique_ptr<sdf::Visual> sdf_visual = MakeSdfVisualFromString(fmt::format(
        sdf_format_str, fmt::arg("p_on", false), fmt::arg("i_on", false)));
    unique_ptr<GeometryInstance> instance = MakeGeometryInstanceFromSdfVisual(
        sdf_diagnostic_, *sdf_visual, NoopResolveFilename,
        ToRigidTransform(sdf_visual->RawPose()));
    ASSERT_EQ(instance, nullptr);
    EXPECT_THAT(TakeWarning(),
                ::testing::HasSubstr("all visual roles turned off for Drake"));
  }
}

// Confirms the failure conditions for SDFormat. SceneGraph requirements on
// geometry names are supposed to mirror the SDFormat behavior. If these tests
// no longer fail, the requirements in SceneGraph should become more relaxed.
// Alternatively, if more failure modes are learned, they should be encoded
// here and in the SceneGraph logic (start at the documentation of
// GeometryInstace).
// Note: This is only tested for visual geometries, but the same requirements
// are assumed for collision geometries.
TEST_F(SceneGraphParserDetail, VisualGeometryNameRequirements) {
  // It is necessary to do a full, deep parse from the root to reveal *all*
  // of the failure modes.

  // A fmt::format-compatible string for testing various permutations of visual
  // names.
  constexpr const char* visual_tag =
      "<visual name='{}'>"
      "  <pose>1.0 2.0 3.0 3.14 6.28 1.57</pose>"
      "  <geometry>"
      "    <cylinder>"
      "      <radius>0.5</radius>"
      "      <length>1.2</length>"
      "    </cylinder>"
      "  </geometry>"
      "</visual>";

  auto valid_parse = [](const std::string& visual_str) -> bool {
    const std::string sdf_str = fmt::format(
        "<?xml version='1.0'?>"
        "<sdf version='1.7'>"
        "  <model name='my_model'>"
        "    <link name='link'>{}"
        "    </link>"
        "  </model>"
        "</sdf>",
        visual_str);
    sdf::Root root;
    sdf::ParserConfig config = MakeStrictConfig();
    auto errors = root.LoadSdfString(sdf_str, config);
    return errors.empty();
  };

  // Allowable naming.
  // Case: control group - a simple valid name.
  EXPECT_TRUE(valid_parse(fmt::format(visual_tag, "visual")));

  // Case: Valid name with leading whitespace.
  EXPECT_TRUE(valid_parse(fmt::format(visual_tag, "  visual")));

  // Case: Valid name with trailing whitespace.
  EXPECT_TRUE(valid_parse(fmt::format(visual_tag, "visual   ")));

  // These whitespace characters are *not* considered to be whitespace by SDF.
  std::vector<std::pair<char, std::string>> ignored_whitespace{{'\v', "\\v"},
                                                               {'\f', "\\f"}};
  for (const auto& pair : ignored_whitespace) {
    // Case: Whitespace-only name.
    EXPECT_TRUE(valid_parse(fmt::format(visual_tag, pair.first)))
        << "Failed on " << pair.second;
  }

  {
    // Case: Same name for two different geometry types (collision vs visual).
    const std::string collision_tag =
        "<collision name='thing'>"
        "  <pose>1.0 2.0 3.0 3.14 6.28 1.57</pose>"
        "  <geometry>"
        "    <sphere/>"
        "  </geometry>"
        "</collision>";
    EXPECT_TRUE(valid_parse(fmt::format(visual_tag, "thing") + collision_tag));
  }

  // Invalid naming
  {
    // Case: Missing name element.
    const std::string missing_name_parameter =
        "<visual>"
        "  <pose>1.0 2.0 3.0 3.14 6.28 1.57</pose>"
        "  <geometry>"
        "    <cylinder>"
        "      <radius>0.5</radius>"
        "      <length>1.2</length>"
        "    </cylinder>"
        "  </geometry>"
        "</visual>";
    EXPECT_FALSE(valid_parse(missing_name_parameter));
  }

  // Case: Empty name element.
  EXPECT_FALSE(valid_parse(fmt::format(visual_tag, "")));

  std::vector<std::pair<char, std::string>> invalid_whitespace{{' ', "space"},
                                                               {'\t', "\\t"}};
  for (const auto& pair : invalid_whitespace) {
    // Case: Whitespace-only name.
    EXPECT_FALSE(valid_parse(fmt::format(visual_tag, pair.first)))
        << "Failed on " << pair.second;
  }

  // Case: Duplicate names.
  EXPECT_FALSE(valid_parse(fmt::format(visual_tag, "visual") +
                           fmt::format(visual_tag, "visual")));

  // Case: Duplicate names which arise from trimming whitespace.
  EXPECT_FALSE(valid_parse(fmt::format(visual_tag, "visual  ") +
                           fmt::format(visual_tag, "  visual")));
}

// Verify MakeGeometryInstanceFromSdfVisual can make a GeometryInstance from an
// sdf::Visual with a <plane> geometry.
// We test this case separately since, while geometry::HalfSpace is defined in a
// canonical frame C whose pose needs to be specified at a GeometryInstance
// level, the SDF specification does not define this pose at the <geometry>
// level but at the <visual> (or <collision>) level.
TEST_F(SceneGraphParserDetail, MakeHalfSpaceGeometryInstanceFromSdfVisual) {
  unique_ptr<sdf::Visual> sdf_visual = MakeSdfVisualFromString(
      "<visual name = 'some_link_visual'>"
      "  <pose>0.0 0.0 0.0 0.0 0.0 0.0</pose>"
      "  <geometry>"
      "    <plane>"
      "      <normal>1.0 2.0 3.0</normal>"
      "    </plane>"
      "  </geometry>"
      "</visual>");

  std::optional<unique_ptr<GeometryInstance>> geometry_instance =
      MakeGeometryInstanceFromSdfVisual(
          sdf_diagnostic_, *sdf_visual, NoopResolveFilename,
          ToRigidTransform(sdf_visual->RawPose()));

  // Verify we do have a plane geometry.
  const HalfSpace* shape =
      dynamic_cast<const HalfSpace*>(&(*geometry_instance)->shape());
  ASSERT_TRUE(shape != nullptr);

  // The expected coordinates of the normal vector in the link frame L.
  const Vector3d normal_L_expected = Vector3d(1.0, 2.0, 3.0).normalized();

  // The expected orientation of the canonical frame C (in which the plane's
  // normal aligns with Cz) in the link frame L.
  const RotationMatrix<double> R_LC_expected =
      HalfSpace::MakePose(normal_L_expected, Vector3d::Zero()).rotation();

  // Retrieve the GeometryInstance pose as parsed from the sdf::Visual.
  const RotationMatrix<double> R_LC = (*geometry_instance)->pose().rotation();
  const Vector3d normal_L = R_LC.col(2);

  // Verify results to precision given by kTolerance.
  const double kTolerance = 10 * std::numeric_limits<double>::epsilon();
  EXPECT_TRUE(R_LC.IsNearlyEqualTo(R_LC_expected, kTolerance));
  EXPECT_TRUE(CompareMatrices(normal_L, normal_L_expected, kTolerance,
                              MatrixCompareType::relative));
}

// Verify MakeSdfVisualFromString() returns nullptr when the visual specifies
// an <empty/> geometry.
TEST_F(SceneGraphParserDetail, MakeEmptyGeometryInstanceFromSdfVisual) {
  unique_ptr<sdf::Visual> sdf_visual = MakeSdfVisualFromString(
      "<visual name = 'some_link_visual'>"
      "  <pose>0.0 0.0 0.0 0.0 0.0 0.0</pose>"
      "  <geometry>"
      "    <empty/>"
      "  </geometry>"
      "</visual>");

  std::optional<unique_ptr<GeometryInstance>> geometry_instance =
      MakeGeometryInstanceFromSdfVisual(
          sdf_diagnostic_, *sdf_visual, NoopResolveFilename,
          ToRigidTransform(sdf_visual->RawPose()));
  EXPECT_EQ(*geometry_instance, nullptr);
}

// Verify that MakeGeometryInstanceFromSdfVisual does nothing with a heightmap.
TEST_F(SceneGraphParserDetail, MakeHeightmapGeometryInstanceFromSdfVisual) {
  unique_ptr<sdf::Visual> sdf_visual = MakeSdfVisualFromString(
      "<visual name='some_link_visual'>"
      "  <pose>1.0 2.0 3.0 3.14 6.28 1.57</pose>"
      "  <geometry>"
      "    <heightmap>"
      "      <uri>/path/to/some/heightmap.png</uri>"
      "    </heightmap>"
      "  </geometry>"
      "</visual>");
  std::optional<unique_ptr<GeometryInstance>> geometry_instance =
      MakeGeometryInstanceFromSdfVisual(
          sdf_diagnostic_, *sdf_visual, NoopResolveFilename,
          ToRigidTransform(sdf_visual->RawPose()));
  EXPECT_EQ(*geometry_instance, nullptr);
}

// Reports if the indicated typed geometry property matches expectations.
// The expectation is given by an optional `expected_value`. If nullopt, we
// expect the property to be absent. If provided, we expect the property to
// exist with the given type and have the same value.
template <typename T, typename Compare>
::testing::AssertionResult HasExpectedProperty(
    const char* group, const char* property, std::optional<T> expected_value,
    const IllustrationProperties& properties, Compare matches) {
  ::testing::AssertionResult failure = ::testing::AssertionFailure();
  const bool has_property = properties.HasProperty(group, property);
  if (expected_value.has_value()) {
    if (has_property) {
      // This will throw if the property is of the wrong type.
      const T& value = properties.GetProperty<T>(group, property);
      if (matches(value, *expected_value)) {
        return ::testing::AssertionSuccess();
      } else {
        std::string value_str;
        std::string expected_str;
        if constexpr (is_eigen_type<T>::value) {
          value_str = fmt::to_string(fmt_eigen(value));
          expected_str = fmt::to_string(fmt_eigen(*expected_value));
        } else {
          value_str = fmt::to_string(value);
          expected_str = fmt::to_string(*expected_value);
        }
        failure << "\nIncorrect values for "
                << "('" << group << "', " << property << "'):"
                << "\n  expected: " << expected_str
                << "\n  found:    " << value_str;
      }
    } else {
      failure << "\n  missing expected property ('" << group << "', '"
              << property << "')";
    }
  } else {
    if (!has_property) return ::testing::AssertionSuccess();

    failure << "\n  found unexpected property ('" << group << "', '" << property
            << "')";
  }
  failure << "\n";
  return failure;
}

// Verify visual material parsing: default for unspecified, and diffuse color
// given where specified in the SDF.
TEST_F(SceneGraphParserDetail, ParseVisualMaterial) {
  using Eigen::Vector4d;

  // Searches the illustration properties for an optional phong material
  // specification with optional color values.
  auto expect_phong = [](const IllustrationProperties& dut,
                         bool must_have_group,
                         const std::optional<Vector4d> diffuse,
                         const std::optional<Vector4d> specular,
                         const std::optional<Vector4d> ambient,
                         const std::optional<Vector4d> emissive,
                         const std::optional<std::string> diffuse_map)
      -> ::testing::AssertionResult {
    ::testing::AssertionResult failure = ::testing::AssertionFailure();
    failure << "\nKnown failure conditions:";
    bool success = true;
    auto test_color = [&failure, &success, &dut](
                          const char* name,
                          const std::optional<Vector4d> ref_color) {
      auto result =
          HasExpectedProperty("phong", name, ref_color, dut,
                              [](const Vector4d& a, const Vector4d& b) {
                                return static_cast<bool>(CompareMatrices(a, b));
                              });
      if (!result) {
        failure << result.message();
        success = false;
      }
    };
    if (must_have_group) {
      if (dut.HasGroup("phong")) {
        test_color("diffuse", diffuse);
        test_color("specular", specular);
        test_color("ambient", ambient);
        test_color("emissive", emissive);
        auto result =
            HasExpectedProperty("phong", "diffuse_map", diffuse_map, dut,
                                [](const std::string& a, const std::string& b) {
                                  return a == b;
                                });
        if (!result) {
          failure << result;
          success = false;
        }
      } else {
        failure << "\n  missing the expected 'phong' group";
        success = false;
      }
    } else {
      if (dut.HasGroup("phong")) {
        failure << "\n  found unexpected 'phong' group";
        success = false;
      }
    }
    if (success) {
      return ::testing::AssertionSuccess();
    } else {
      failure << "\n";
      return failure;
    }
  };

  // Builds a visual XML tag with an optional <material> tag and optional
  // color values.
  auto make_xml = [](bool has_material, Vector4d* diffuse, Vector4d* specular,
                     Vector4d* ambient, Vector4d* emissive,
                     const std::string& texture_name) {
    std::stringstream ss;
    ss << "<visual name='some_link_visual'>"
       << "  <pose>0 0 0 0 0 0</pose>"
       << "  <geometry>"
       << "    <sphere>"
       << "      <radius>1</radius>"
       << "    </sphere>"
       << "  </geometry>";
    if (has_material) {
      auto write_color = [&ss](const char* name, const Vector4d* color) {
        if (color) {
          const Vector4d& c = *color;
          ss << fmt::format("    <{0}>{1} {2} {3} {4}</{0}>", name, c(0), c(1),
                            c(2), c(3));
        }
      };
      ss << "  <material>";
      if (!texture_name.empty()) {
        ss << "    <drake:diffuse_map>" << texture_name
           << "</drake:diffuse_map>";
      }
      write_color("diffuse", diffuse);
      write_color("specular", specular);
      write_color("ambient", ambient);
      write_color("emissive", emissive);
      ss << "  </material>";
    }
    ss << "</visual>";

    return ss.str();
  };

  // We need a root directory to an actually existing file we can reference.
  const std::string file_path = FindResourceOrThrow(
      "drake/multibody/parsing/test/urdf_parser_test/empty.png");
  const std::string root_dir =
      std::filesystem::path(file_path).parent_path().string();

  // Case: No material defined -- empty illustration properties.
  {
    unique_ptr<sdf::Visual> sdf_visual = MakeSdfVisualFromString(
        make_xml(false, nullptr, nullptr, nullptr, nullptr, ""));
    VisualProperties vis_props = MakeVisualPropertiesFromSdfVisual(*sdf_visual);
    ASSERT_TRUE(vis_props.illustration.has_value());
    EXPECT_TRUE(
        expect_phong(*vis_props.illustration, false, {}, {}, {}, {}, {}));
  }

  // Case: Material tag defined, but no material properties -- empty
  // illustration properties.
  {
    unique_ptr<sdf::Visual> sdf_visual = MakeSdfVisualFromString(
        make_xml(true, nullptr, nullptr, nullptr, nullptr, ""));
    VisualProperties vis_props = MakeVisualPropertiesFromSdfVisual(*sdf_visual);
    ASSERT_TRUE(vis_props.illustration.has_value());
    EXPECT_TRUE(
        expect_phong(*vis_props.illustration, false, {}, {}, {}, {}, {}));
  }

  Vector4<double> diffuse{0.25, 0.5, 0.75, 1.0};
  Vector4<double> specular{0.5, 0.75, 1.0, 0.25};
  Vector4<double> ambient{0.75, 1.0, 0.25, 0.5};
  Vector4<double> emissive{1.0, 0.25, 0.5, 0.75};

  // Case: Only valid diffuse material.
  {
    unique_ptr<sdf::Visual> sdf_visual = MakeSdfVisualFromString(
        make_xml(true, &diffuse, nullptr, nullptr, nullptr, ""));
    VisualProperties vis_props = MakeVisualPropertiesFromSdfVisual(*sdf_visual);
    ASSERT_TRUE(vis_props.illustration.has_value());
    EXPECT_TRUE(
        expect_phong(*vis_props.illustration, true, diffuse, {}, {}, {}, {}));
  }

  // Case: Only valid specular defined.
  {
    unique_ptr<sdf::Visual> sdf_visual = MakeSdfVisualFromString(
        make_xml(true, nullptr, &specular, nullptr, nullptr, ""));
    VisualProperties vis_props = MakeVisualPropertiesFromSdfVisual(*sdf_visual);
    ASSERT_TRUE(vis_props.illustration.has_value());
    EXPECT_TRUE(
        expect_phong(*vis_props.illustration, true, {}, specular, {}, {}, {}));
  }

  // Case: Only valid ambient defined.
  {
    unique_ptr<sdf::Visual> sdf_visual = MakeSdfVisualFromString(
        make_xml(true, nullptr, nullptr, &ambient, nullptr, ""));
    VisualProperties vis_props = MakeVisualPropertiesFromSdfVisual(*sdf_visual);
    ASSERT_TRUE(vis_props.illustration.has_value());
    EXPECT_TRUE(
        expect_phong(*vis_props.illustration, true, {}, {}, ambient, {}, {}));
  }

  // Case: Only valid emissive defined.
  {
    unique_ptr<sdf::Visual> sdf_visual = MakeSdfVisualFromString(
        make_xml(true, nullptr, nullptr, nullptr, &emissive, ""));
    VisualProperties vis_props = MakeVisualPropertiesFromSdfVisual(*sdf_visual);
    ASSERT_TRUE(vis_props.illustration.has_value());
    EXPECT_TRUE(
        expect_phong(*vis_props.illustration, true, {}, {}, {}, emissive, {}));
  }

  // Case: All four.
  {
    unique_ptr<sdf::Visual> sdf_visual = MakeSdfVisualFromString(
        make_xml(true, &diffuse, &specular, &ambient, &emissive, ""));
    VisualProperties vis_props = MakeVisualPropertiesFromSdfVisual(*sdf_visual);
    ASSERT_TRUE(vis_props.illustration.has_value());
    EXPECT_TRUE(expect_phong(*vis_props.illustration, true, diffuse, specular,
                             ambient, emissive, {}));
  }

  // Case: With diffuse map.
  {
    // Note: we only test with a local map; we rely on the tests for resolving
    // URIs to do the right thing with other URI formats.
    const std::string kLocalMap = "empty.png";
    const std::string xml =
        make_xml(true, &diffuse, &specular, &ambient, &emissive, kLocalMap);
    unique_ptr<sdf::Visual> sdf_visual = MakeSdfVisualFromString(
        make_xml(true, &diffuse, &specular, &ambient, &emissive, kLocalMap));
    VisualProperties vis_props = MakeVisualPropertiesFromSdfVisual(*sdf_visual);
    ASSERT_TRUE(vis_props.illustration.has_value());
    // Note: The "no-op" filename resolver will just return kLocalMap as the
    // property name.
    EXPECT_TRUE(expect_phong(*vis_props.illustration, true, diffuse, specular,
                             ambient, emissive, kLocalMap));
  }

  // Case: Diffuse map file not found.
  {
    const std::string kLocalMap = "empty.png";
    const std::string xml =
        make_xml(true, &diffuse, &specular, &ambient, &emissive, kLocalMap);
    unique_ptr<sdf::Visual> sdf_visual = MakeSdfVisualFromString(
        make_xml(true, &diffuse, &specular, &ambient, &emissive, kLocalMap));
    internal::MakeVisualPropertiesFromSdfVisual(
        sdf_diagnostic_, *sdf_visual,
        [](const SDFormatDiagnostic&, std::string filename) -> std::string {
          return {};
        });
    EXPECT_THAT(TakeError(),
                ::testing::MatchesRegex(
                    ".*Unable to locate the texture file: empty.png"));
  }

  // Case: drake:{illustration|perception}_properties is missing the enabled
  // attribute. The result has the named property.
  {
    unique_ptr<sdf::Visual> sdf_visual = MakeSdfVisualFromString(
        "<visual name='v'>"
        " <geometry><sphere><radius>1</radius></sphere></geometry>"
        " <drake:illustration_properties/>"
        "</visual>");
    VisualProperties vis_props = MakeVisualPropertiesFromSdfVisual(*sdf_visual);
    ASSERT_TRUE(vis_props.illustration.has_value());
  }

  // Case: drake:{illustration|perception}_properties explicitly says true for
  // enabled attribute. The result has the named property.
  {
    unique_ptr<sdf::Visual> sdf_visual = MakeSdfVisualFromString(
        "<visual name='v'>"
        " <geometry><sphere><radius>1</radius></sphere></geometry>"
        " <drake:illustration_properties enabled='true'/>"
        "</visual>");
    VisualProperties vis_props = MakeVisualPropertiesFromSdfVisual(*sdf_visual);
    ASSERT_TRUE(vis_props.illustration.has_value());
  }

  // Case: drake:{illustration|perception}_properties have the wrong value
  // type for the "enabled" attribute.
  {
    unique_ptr<sdf::Visual> sdf_visual = MakeSdfVisualFromString(
        "<visual name='v'>"
        " <geometry><sphere><radius>1</radius></sphere></geometry>"
        " <drake:illustration_properties enabled=\"bob\"/>"
        "</visual>");
    internal::MakeVisualPropertiesFromSdfVisual(
        sdf_diagnostic_, *sdf_visual,
        [](const SDFormatDiagnostic&, std::string) -> std::string {
          return {};
        });
    EXPECT_THAT(TakeError(),
                ::testing::MatchesRegex(
                    ".*'enabled' attribute with the wrong value type.*"));
  }

  // Note: As of https://github.com/osrf/sdformat/pull/519, sdformat is doing
  // more work in validating otherwise invalid color declarations. When sdformat
  // deems a color to be invalid, we don't get the corresponding property. This
  // confirms such cases.
  std::vector<std::string> bad_diffuse_strings{
      "    <diffuse>0.25 1 0.5 0.25 2</diffuse>",  // Too many values.
      "    <diffuse>0 1</diffuse>",                // Too few values.
      "    <diffuse>-0.1 255 65025 2</diffuse>"};  // Out of range values.
  for (const auto& bad_diffuse : bad_diffuse_strings) {
    unique_ptr<sdf::Visual> sdf_visual = MakeSdfVisualFromString(
        "<visual name='some_link_visual'>"
        "  <pose>0 0 0 0 0 0</pose>"
        "  <geometry>"
        "    <sphere>"
        "      <radius>1</radius>"
        "    </sphere>"
        "  </geometry>"
        "  <material>" +
        bad_diffuse +
        "  </material>"
        "</visual>");
    VisualProperties vis_props = MakeVisualPropertiesFromSdfVisual(*sdf_visual);
    ASSERT_TRUE(vis_props.illustration.has_value());
    EXPECT_TRUE(
        expect_phong(*vis_props.illustration, false, {}, {}, {}, {}, {}));
  }
}

// Confirms that the <drake:accepting_renderer> tag gets properly parsed. The
// property gets assigned to perception properties but not illustration.
TEST_F(SceneGraphParserDetail, AcceptingRenderers) {
  const std::string group = "renderer";
  const std::string property = "accepting";

  // Case: no <drake:accepting_renderer> tag.
  {
    unique_ptr<sdf::Visual> sdf_visual = MakeSdfVisualFromString(
        "<visual name='some_link_visual'>"
        "  <pose>0 0 0 0 0 0</pose>"
        "  <geometry>"
        "    <sphere>"
        "      <radius>1</radius>"
        "    </sphere>"
        "  </geometry>"
        "  <material>"
        "    <diffuse>0.25 1 0.5 0.25</diffuse>"
        "  </material>"
        "</visual>");
    VisualProperties vis_props = MakeVisualPropertiesFromSdfVisual(*sdf_visual);
    ASSERT_TRUE(vis_props.perception.has_value());
    EXPECT_FALSE(vis_props.perception->HasProperty(group, property));
    ASSERT_TRUE(vis_props.illustration.has_value());
    EXPECT_FALSE(vis_props.illustration->HasProperty(group, property));
  }

  // Case: single <drake:accepting_renderer> tag.
  {
    unique_ptr<sdf::Visual> sdf_visual = MakeSdfVisualFromString(
        "<visual name='some_link_visual'>"
        "  <pose>0 0 0 0 0 0</pose>"
        "  <geometry>"
        "    <sphere>"
        "      <radius>1</radius>"
        "    </sphere>"
        "  </geometry>"
        "  <material>"
        "    <diffuse>0.25 1 0.5 0.25</diffuse>"
        "  </material>"
        "  <drake:accepting_renderer>renderer1</drake:accepting_renderer>"
        "</visual>");
    VisualProperties vis_props = MakeVisualPropertiesFromSdfVisual(*sdf_visual);
    ASSERT_TRUE(vis_props.perception.has_value());
    EXPECT_TRUE(vis_props.perception->HasProperty(group, property));
    const auto& names =
        vis_props.perception->GetProperty<std::set<std::string>>(group,
                                                                 property);
    EXPECT_EQ(names.size(), 1);
    EXPECT_TRUE(names.contains("renderer1"));
    ASSERT_TRUE(vis_props.illustration.has_value());
    EXPECT_FALSE(vis_props.illustration->HasProperty(group, property));
  }

  // Case: Multiple <drake:accepting_renderer> tag.
  {
    unique_ptr<sdf::Visual> sdf_visual = MakeSdfVisualFromString(
        "<visual name='some_link_visual'>"
        "  <pose>0 0 0 0 0 0</pose>"
        "  <geometry>"
        "    <sphere>"
        "      <radius>1</radius>"
        "    </sphere>"
        "  </geometry>"
        "  <material>"
        "    <diffuse>0.25 1 0.5 0.25</diffuse>"
        "  </material>"
        "  <drake:accepting_renderer>renderer1</drake:accepting_renderer>"
        "  <drake:accepting_renderer>renderer2</drake:accepting_renderer>"
        "</visual>");
    VisualProperties vis_props = MakeVisualPropertiesFromSdfVisual(*sdf_visual);
    ASSERT_TRUE(vis_props.perception.has_value());
    EXPECT_TRUE(vis_props.perception->HasProperty(group, property));
    const auto& names =
        vis_props.perception->GetProperty<std::set<std::string>>(group,
                                                                 property);
    EXPECT_EQ(names.size(), 2);
    EXPECT_TRUE(names.contains("renderer1"));
    EXPECT_TRUE(names.contains("renderer2"));
    ASSERT_TRUE(vis_props.illustration.has_value());
    EXPECT_FALSE(vis_props.illustration->HasProperty(group, property));
  }

  // Case: Missing names throws exception.
  {
    unique_ptr<sdf::Visual> sdf_visual = MakeSdfVisualFromString(
        "<visual name='some_link_visual'>"
        "  <pose>0 0 0 0 0 0</pose>"
        "  <geometry>"
        "    <sphere>"
        "      <radius>1</radius>"
        "    </sphere>"
        "  </geometry>"
        "  <material>"
        "    <diffuse>0.25 1 0.5 0.25</diffuse>"
        "  </material>"
        "  <drake:accepting_renderer> </drake:accepting_renderer>"
        "</visual>");
    MakeVisualPropertiesFromSdfVisual(*sdf_visual);
    EXPECT_THAT(TakeError(),
                ::testing::MatchesRegex(
                    ".*<drake:accepting_renderer> tag given without any name"));
  }

  // Case: specifying accepting renderers with disabled perception role warns.
  {
    unique_ptr<sdf::Visual> sdf_visual = MakeSdfVisualFromString(
        "<visual name='some_link_visual'>"
        "  <geometry><sphere><radius>1</radius></sphere></geometry>"
        "  <drake:perception_properties enabled=\"false\"/>"
        "  <drake:accepting_renderer> </drake:accepting_renderer>"
        "</visual>");
    MakeVisualPropertiesFromSdfVisual(*sdf_visual);
    EXPECT_THAT(TakeWarning(), ::testing::MatchesRegex(
                                   ".*<drake:accepting_renderer> specified .* "
                                   "disabled perception role."));
  }
}

// Verify that the <drake:*_properties> are accounted for in disabling sets of
// visual properties.
TEST_F(SceneGraphParserDetail, DisablingVisualRoles) {
  const Rgba expected_diffuse(0.25, 1, 0.5, 0.25);

  static constexpr char visual_format[] = R"""(
  <visual name='visual'>
    <geometry><sphere><radius>1</radius></sphere></geometry>
    <material><diffuse>0.25, 1 0.5 0.25</diffuse></material>
    {}
  </visual>)""";

  // Case: Saying nothing produces both illustration and perception properties
  // with the same material properties.
  {
    unique_ptr<sdf::Visual> sdf_visual =
        MakeSdfVisualFromString(fmt::format(visual_format, ""));
    VisualProperties vis_props = MakeVisualPropertiesFromSdfVisual(*sdf_visual);
    ASSERT_TRUE(vis_props.illustration.has_value());
    EXPECT_EQ(vis_props.illustration->GetPropertyOrDefault("phong", "diffuse",
                                                           Rgba(0, 0, 0)),
              expected_diffuse);
    ASSERT_TRUE(vis_props.perception.has_value());
    EXPECT_EQ(vis_props.perception->GetPropertyOrDefault("phong", "diffuse",
                                                         Rgba(0, 0, 0)),
              expected_diffuse);
  }

  // Case: Saying enabled is the same as saying nothing.
  {
    unique_ptr<sdf::Visual> sdf_visual =
        MakeSdfVisualFromString(fmt::format(visual_format, R"""(
        <drake:perception_properties enabled="true"/>
        <drake:illustration_properties enabled="true"/>)"""));
    VisualProperties vis_props = MakeVisualPropertiesFromSdfVisual(*sdf_visual);
    ASSERT_TRUE(vis_props.illustration.has_value());
    EXPECT_EQ(vis_props.illustration->GetPropertyOrDefault("phong", "diffuse",
                                                           Rgba(0, 0, 0)),
              expected_diffuse);
    ASSERT_TRUE(vis_props.perception.has_value());
    EXPECT_EQ(vis_props.perception->GetPropertyOrDefault("phong", "diffuse",
                                                         Rgba(0, 0, 0)),
              expected_diffuse);
  }

  // Case: Disabling perception leaves only illustration.
  {
    unique_ptr<sdf::Visual> sdf_visual = MakeSdfVisualFromString(
        fmt::format(visual_format,
                    R"""(<drake:perception_properties enabled="false"/>)"""));
    VisualProperties vis_props = MakeVisualPropertiesFromSdfVisual(*sdf_visual);
    ASSERT_TRUE(vis_props.illustration.has_value());
    EXPECT_EQ(vis_props.illustration->GetPropertyOrDefault("phong", "diffuse",
                                                           Rgba(0, 0, 0)),
              expected_diffuse);
    ASSERT_FALSE(vis_props.perception.has_value());
  }

  // Case: Disabling illustration leaves only perception.
  {
    unique_ptr<sdf::Visual> sdf_visual = MakeSdfVisualFromString(
        fmt::format(visual_format,
                    R"""(<drake:illustration_properties enabled="false"/>)"""));
    VisualProperties vis_props = MakeVisualPropertiesFromSdfVisual(*sdf_visual);
    ASSERT_FALSE(vis_props.illustration.has_value());
    ASSERT_TRUE(vis_props.perception.has_value());
    EXPECT_EQ(vis_props.perception->GetPropertyOrDefault("phong", "diffuse",
                                                         Rgba(0, 0, 0)),
              expected_diffuse);
  }

  // Case: Disabling both reports no geometry properties, but does spew a
  // warning.
  {
    unique_ptr<sdf::Visual> sdf_visual =
        MakeSdfVisualFromString(fmt::format(visual_format, R"""(
        <drake:perception_properties enabled="false"/>
        <drake:illustration_properties enabled="false"/>)"""));
    VisualProperties vis_props = MakeVisualPropertiesFromSdfVisual(*sdf_visual);
    ASSERT_FALSE(vis_props.illustration.has_value());
    ASSERT_FALSE(vis_props.perception.has_value());
    EXPECT_THAT(TakeWarning(),
                ::testing::HasSubstr("all visual roles turned off for Drake"));
  }

  // Case: Disabling both reports no geometry properties. If the visual came
  // from a <drake:visual> originally, there's no warning.
  {
    unique_ptr<sdf::Visual> sdf_visual = MakeSdfVisualFromString(R"""(
        <visual name='visual'>
          <geometry><sphere><radius>1</radius></sphere></geometry>
          <drake:perception_properties enabled="false"/>
          <drake:illustration_properties enabled="false"/>
        </visual>)""");
    // This is the attribute that gets added in detail_sdf_parser.cc when a
    // <drake:visual> gets converted to <visual>.
    sdf_visual->Element()->AddAttribute(kIsDrakeNamespaceAttr, "bool", "true",
                                        /* required */ false);
    VisualProperties vis_props = MakeVisualPropertiesFromSdfVisual(*sdf_visual);
    ASSERT_FALSE(vis_props.illustration.has_value());
    ASSERT_FALSE(vis_props.perception.has_value());
    // Because it ostensibly came from <drake:visual>, there is no warning.
    ASSERT_EQ(NumWarnings(), 0);
  }
}

// Verify MakeGeometryPoseFromSdfCollision() makes the pose X_LG of geometry
// frame G in the link frame L.
// Since we test MakeShapeFromSdfGeometry separately, there is no need to unit
// test every combination of a <collision> with a different <geometry>.
TEST_F(SceneGraphParserDetail, MakeGeometryPoseFromSdfCollision) {
  unique_ptr<sdf::Collision> sdf_collision = MakeSdfCollisionFromString(
      "<collision name = 'some_link_collision'>"
      "  <pose>1.0 2.0 3.0 3.14 6.28 1.57</pose>"
      "  <geometry>"
      "    <sphere/>"
      "  </geometry>"
      "</collision>");
  const RigidTransformd X_LG = MakeGeometryPoseFromSdfCollision(
      *sdf_collision, ToRigidTransform(sdf_collision->RawPose()));

  // These are the expected values as specified by the string above.
  const RollPitchYaw<double> expected_rpy(3.14, 6.28, 1.57);
  const RotationMatrix<double> R_LG_expected(expected_rpy);
  const Vector3d p_LGo_expected(1.0, 2.0, 3.0);

  // Verify results to precision given by kTolerance.
  const double kTolerance = 10 * std::numeric_limits<double>::epsilon();
  EXPECT_TRUE(X_LG.rotation().IsNearlyEqualTo(R_LG_expected, kTolerance));
  EXPECT_TRUE(CompareMatrices(X_LG.translation(), p_LGo_expected, kTolerance,
                              MatrixCompareType::relative));
}

// Verify MakeGeometryPoseFromSdfCollision can make the pose X_LG of the
// geometry frame G in the link frame L when the specified shape is a plane.
// We test this case separately since, while geometry::HalfSpace is defined in a
// canonical frame C whose pose needs to be specified at a GeometryInstance
// level, the SDF specification does not define this pose at the <geometry>
// level but at the <collision> level.
TEST_F(SceneGraphParserDetail, MakeGeometryPoseFromSdfCollisionForHalfSpace) {
  unique_ptr<sdf::Collision> sdf_collision = MakeSdfCollisionFromString(
      "<collision name = 'some_link_collision'>"
      "  <pose>0.0 0.0 0.0 0.0 0.0 0.0</pose>"
      "  <geometry>"
      "    <plane>"
      "      <normal>1.0 2.0 3.0</normal>"
      "    </plane>"
      "  </geometry>"
      "</collision>");
  const RigidTransformd X_LG = MakeGeometryPoseFromSdfCollision(
      *sdf_collision, ToRigidTransform(sdf_collision->RawPose()));

  // The expected coordinates of the normal vector in the link frame L.
  const Vector3d normal_L_expected = Vector3d(1.0, 2.0, 3.0).normalized();

  // The expected orientation of the canonical frame C (in which the plane's
  // normal aligns with Cz) in the link frame L.
  const RotationMatrix<double> R_LG_expected =
      HalfSpace::MakePose(normal_L_expected, Vector3d::Zero()).rotation();

  // Verify results to precision given by kTolerance.
  const double kTolerance = 10 * std::numeric_limits<double>::epsilon();
  EXPECT_TRUE(X_LG.rotation().IsNearlyEqualTo(R_LG_expected, kTolerance));
  EXPECT_TRUE(CompareMatrices(X_LG.translation(), Vector3d::Zero(), kTolerance,
                              MatrixCompareType::relative));
}

// Verify we can parse drake collision properties from a <collision> element.
TEST_F(SceneGraphParserDetail, MakeProximityPropertiesForCollision) {
  // This string represents the generic XML spelling of a <collision> element.
  // It contains a `{}` place holder such that child tags of <collision> can be
  // injected to test various expressions of collision properties --
  // substitution via fmt::format.
  constexpr const char* collision_xml = R"""(
<collision name="some_geo">
  <pose>0.0 0.0 0.0 0.0 0.0 0.0</pose>
  <geometry>
    <plane>
      <normal>1.0 2.0 3.0</normal>
    </plane>
  </geometry>{}
</collision>
)""";

  auto make_sdf_collision = [&collision_xml](const char* material_string) {
    return MakeSdfCollisionFromString(
        fmt::format(collision_xml, material_string));
  };

  auto assert_friction = [](const ProximityProperties& properties,
                            const CoulombFriction<double>& expected_friction) {
    ASSERT_TRUE(properties.HasProperty(geometry::internal::kMaterialGroup,
                                       geometry::internal::kFriction));
    const auto& friction = properties.GetProperty<CoulombFriction<double>>(
        geometry::internal::kMaterialGroup, geometry::internal::kFriction);
    EXPECT_EQ(friction.static_friction(), expected_friction.static_friction());
    EXPECT_EQ(friction.dynamic_friction(),
              expected_friction.dynamic_friction());
  };

  auto assert_single_property = [](const ProximityProperties& properties,
                                   const char* group, const char* property,
                                   double value) {
    SCOPED_TRACE(fmt::format("testing group {} property {} value {}", group,
                             property, value));
    ASSERT_TRUE(properties.HasProperty(group, property));
    EXPECT_EQ(properties.GetProperty<double>(group, property), value);
  };

  // This parser uses the ParseProximityProperties found in detail_common
  // (which already has exhaustive tests). So, we'll put in a smoke test to
  // confirm that all of the basic tags get parsed and focus on the logic that
  // is unique to `MakeProximityPropertiesForCollision()`.
  {
    unique_ptr<sdf::Collision> sdf_collision = make_sdf_collision(R"""(
  <drake:proximity_properties>
    <drake:mesh_resolution_hint>2.5</drake:mesh_resolution_hint>
    <drake:hydroelastic_modulus>3.5</drake:hydroelastic_modulus>
    <drake:hydroelastic_margin>1.3</drake:hydroelastic_margin>
    <drake:hunt_crossley_dissipation>4.5</drake:hunt_crossley_dissipation>
    <drake:relaxation_time>3.1</drake:relaxation_time>
    <drake:mu_dynamic>4.25</drake:mu_dynamic>
    <drake:mu_static>4.75</drake:mu_static>
  </drake:proximity_properties>)""");
    std::optional<ProximityProperties> properties =
        MakeProximityPropertiesForCollision(sdf_diagnostic_, *sdf_collision);
    ASSERT_TRUE(properties.has_value());
    assert_single_property(*properties, geometry::internal::kHydroGroup,
                           geometry::internal::kRezHint, 2.5);
    assert_single_property(*properties, geometry::internal::kHydroGroup,
                           geometry::internal::kElastic, 3.5);
    assert_single_property(*properties, geometry::internal::kHydroGroup,
                           geometry::internal::kMargin, 1.3);
    assert_single_property(*properties, geometry::internal::kMaterialGroup,
                           geometry::internal::kHcDissipation, 4.5);
    assert_single_property(*properties, geometry::internal::kMaterialGroup,
                           geometry::internal::kRelaxationTime, 3.1);
    assert_friction(*properties, {4.75, 4.25});
  }

  // Case: specifies rigid hydroelastic.
  {
    unique_ptr<sdf::Collision> sdf_collision = make_sdf_collision(R"""(
  <drake:proximity_properties>
    <drake:rigid_hydroelastic/>
  </drake:proximity_properties>)""");
    std::optional<ProximityProperties> properties =
        MakeProximityPropertiesForCollision(sdf_diagnostic_, *sdf_collision);
    ASSERT_TRUE(properties.has_value());
    ASSERT_TRUE(properties->HasProperty(geometry::internal::kHydroGroup,
                                        geometry::internal::kComplianceType));
    EXPECT_EQ(properties->GetProperty<geometry::internal::HydroelasticType>(
                  geometry::internal::kHydroGroup,
                  geometry::internal::kComplianceType),
              geometry::internal::HydroelasticType::kRigid);
  }

  // Case: specifies compliant hydroelastic.
  {
    unique_ptr<sdf::Collision> sdf_collision = make_sdf_collision(R"""(
  <drake:proximity_properties>
    <drake:compliant_hydroelastic/>
  </drake:proximity_properties>)""");
    std::optional<ProximityProperties> properties =
        MakeProximityPropertiesForCollision(sdf_diagnostic_, *sdf_collision);
    ASSERT_TRUE(properties.has_value());
    ASSERT_TRUE(properties->HasProperty(geometry::internal::kHydroGroup,
                                        geometry::internal::kComplianceType));
    EXPECT_EQ(properties->GetProperty<geometry::internal::HydroelasticType>(
                  geometry::internal::kHydroGroup,
                  geometry::internal::kComplianceType),
              geometry::internal::HydroelasticType::kSoft);
  }

  // TODO(16229): Remove this ad-hoc input sanitization when we resolve
  //  issue 16229 "Diagnostics for unsupported SDFormat and URDF stanzas."
  // Case: specifies unsupported drake:soft_hydroelastic -- should be an error.
  {
    unique_ptr<sdf::Collision> sdf_collision = make_sdf_collision(R"""(
  <drake:proximity_properties>
    <drake:soft_hydroelastic/>
  </drake:proximity_properties>)""");
    std::optional<ProximityProperties> properties =
        MakeProximityPropertiesForCollision(sdf_diagnostic_, *sdf_collision);
    EXPECT_FALSE(properties.has_value());
    EXPECT_THAT(TakeError(),
                ::testing::MatchesRegex(
                    ".*A <collision> geometry has defined the unsupported tag "
                    "<drake:soft_hydroelastic>. Please change it to "
                    "<drake:compliant_hydroelastic>."));
  }

  // Case: specifies both -- should be an error.
  {
    unique_ptr<sdf::Collision> sdf_collision = make_sdf_collision(R"""(
  <drake:proximity_properties>
    <drake:rigid_hydroelastic/>
    <drake:compliant_hydroelastic/>
  </drake:proximity_properties>)""");
    std::optional<ProximityProperties> properties =
        MakeProximityPropertiesForCollision(sdf_diagnostic_, *sdf_collision);
    EXPECT_FALSE(properties.has_value());
    EXPECT_THAT(
        TakeError(),
        ::testing::MatchesRegex(
            ".*A <collision> geometry has defined mutually-exclusive tags "
            ".*rigid.* and .*compliant.*"));
  }

  // Case: has no drake coefficients, only mu & m2 in ode: contains mu, mu2
  // friction.
  {
    unique_ptr<sdf::Collision> sdf_collision = make_sdf_collision(R"""(
  <surface>
    <friction>
      <ode>
        <mu>0.8</mu>
        <mu2>0.3</mu2>
      </ode>
    </friction>
  </surface>)""");
    std::optional<ProximityProperties> properties =
        MakeProximityPropertiesForCollision(sdf_diagnostic_, *sdf_collision);
    ASSERT_TRUE(properties.has_value());
    assert_friction(*properties, {0.8, 0.3});
  }

  // Case: has both ode (mu, mu2) and drake (dynamic): contains
  // drake::mu_dynamic wins.
  {
    unique_ptr<sdf::Collision> sdf_collision = make_sdf_collision(R"""(
  <drake:proximity_properties>
    <drake:mu_dynamic>0.3</drake:mu_dynamic>
  </drake:proximity_properties>
  <surface>
    <friction>
      <ode>
        <mu>1.8</mu>
        <mu2>1.3</mu2>
      </ode>
    </friction>
  </surface>)""");
    DiagnosticPolicy diagnostic;
    DiagnosticDetail warning;
    diagnostic.SetActionForWarnings([&](const DiagnosticDetail& detail) {
      warning = detail;
    });
    const std::string file_path("file.txt");
    DataSource data_source(DataSource::kFilename, &file_path);
    SDFormatDiagnostic sdf_diagnostic(&diagnostic, &data_source);
    std::optional<ProximityProperties> properties =
        MakeProximityPropertiesForCollision(sdf_diagnostic, *sdf_collision);
    ASSERT_TRUE(properties.has_value());
    EXPECT_THAT(warning.message, ::testing::MatchesRegex(
                                     ".*collision.*some_geo.*ode.*ignored.*"));
    assert_friction(*properties, {0.3, 0.3});
  }
  // Note: we're not explicitly testing negative friction coefficients or
  // dynamic > static because we rely on the CoulombFriction constructor to
  // handle that.
}

// Verify we can parse friction coefficients from an <ode> element in
// <collision><surface><friction>. Drake understands <mu> to be the static
// coefficient and <mu2> the dynamic coefficient of friction.
TEST_F(SceneGraphParserDetail, MakeCoulombFrictionFromSdfCollisionOde) {
  unique_ptr<sdf::Collision> sdf_collision = MakeSdfCollisionFromString(
      "<collision name = 'some_link_collision'>"
      "  <pose>0.0 0.0 0.0 0.0 0.0 0.0</pose>"
      "  <geometry>"
      "    <plane>"
      "      <normal>1.0 2.0 3.0</normal>"
      "    </plane>"
      "  </geometry>"
      "  <surface>"
      "    <friction>"
      "      <ode>"
      "        <mu>0.8</mu>"
      "        <mu2>0.3</mu2>"
      "      </ode>"
      "    </friction>"
      "  </surface>"
      "</collision>");
  std::optional<CoulombFriction<double>> friction =
      MakeCoulombFrictionFromSdfCollisionOde(sdf_diagnostic_, *sdf_collision);
  ASSERT_TRUE(friction.has_value());
  EXPECT_EQ(friction->static_friction(), 0.8);
  EXPECT_EQ(friction->dynamic_friction(), 0.3);
}

// Verify that if no <surface> tag is present, we return default friction
// coefficients.
TEST_F(SceneGraphParserDetail,
       MakeCoulombFrictionFromSdfCollisionOde_NoSurface) {
  unique_ptr<sdf::Collision> sdf_collision = MakeSdfCollisionFromString(
      "<collision name = 'some_link_collision'>"
      "  <pose>0.0 0.0 0.0 0.0 0.0 0.0</pose>"
      "  <geometry>"
      "    <plane>"
      "      <normal>1.0 2.0 3.0</normal>"
      "    </plane>"
      "  </geometry>"
      "</collision>");
  std::optional<CoulombFriction<double>> friction =
      MakeCoulombFrictionFromSdfCollisionOde(sdf_diagnostic_, *sdf_collision);
  ASSERT_TRUE(friction.has_value());
  std::optional<CoulombFriction<double>> expected_friction = default_friction();
  ASSERT_TRUE(expected_friction.has_value());
  EXPECT_EQ(friction->static_friction(), expected_friction->static_friction());
  EXPECT_EQ(friction->dynamic_friction(),
            expected_friction->dynamic_friction());
}

// Verify MakeCoulombFrictionFromSdfCollisionOde() throws an exception if
// provided a dynamic friction coefficient larger than the static friction
// coefficient.
// We do not need testing for each possible case of an invalid input such as:
// - negative coefficients.
// - dynamic > static.
// - only one coefficient is negative.
// Since class CoulombFriction performs these tests at construction and its unit
// tests provide coverage for these cases. In that regard, this following
// test is not needed but we provide it just to show how the exception message
// thrown from CoulombFriction gets concatenated and re-thrown by
// MakeCoulombFrictionFromSdfCollisionOde().
TEST_F(SceneGraphParserDetail,
       MakeCoulombFrictionFromSdfCollisionOde_DynamicLargerThanStatic) {
  unique_ptr<sdf::Collision> sdf_collision = MakeSdfCollisionFromString(
      "<collision name = 'some_link_collision'>"
      "  <pose>0.0 0.0 0.0 0.0 0.0 0.0</pose>"
      "  <geometry>"
      "    <plane>"
      "      <normal>1.0 2.0 3.0</normal>"
      "    </plane>"
      "  </geometry>"
      "  <surface>"
      "    <friction>"
      "      <ode>"
      "        <mu>0.3</mu>"
      "        <mu2>0.8</mu2>"
      "      </ode>"
      "    </friction>"
      "  </surface>"
      "</collision>");
  DRAKE_EXPECT_THROWS_MESSAGE(
      MakeCoulombFrictionFromSdfCollisionOde(sdf_diagnostic_, *sdf_collision),
      "The given dynamic friction \\(.*\\) is greater than the given static "
      "friction \\(.*\\); dynamic friction must be less than or equal to "
      "static friction.");
}

TEST_F(SceneGraphParserDetail,
       MakeCoulombFrictionFromSdfCollisionOde_MuMissing) {
  unique_ptr<sdf::Collision> sdf_collision = MakeSdfCollisionFromString(
      "<collision name = 'some_link_collision'>"
      "  <pose>0.0 0.0 0.0 0.0 0.0 0.0</pose>"
      "  <geometry>"
      "    <plane>"
      "      <normal>1.0 2.0 3.0</normal>"
      "    </plane>"
      "  </geometry>"
      "  <surface>"
      "    <friction>"
      "      <ode>"
      "        <mu2>0.8</mu2>"
      "      </ode>"
      "    </friction>"
      "  </surface>"
      "</collision>");
  EXPECT_EQ(
      MakeCoulombFrictionFromSdfCollisionOde(sdf_diagnostic_, *sdf_collision),
      CoulombFriction<double>(default_friction().static_friction(), 0.8));
}

TEST_F(SceneGraphParserDetail,
       MakeCoulombFrictionFromSdfCollisionOde_Mu2Missing) {
  unique_ptr<sdf::Collision> sdf_collision = MakeSdfCollisionFromString(
      "<collision name = 'some_link_collision'>"
      "  <pose>0.0 0.0 0.0 0.0 0.0 0.0</pose>"
      "  <geometry>"
      "    <plane>"
      "      <normal>1.0 2.0 3.0</normal>"
      "    </plane>"
      "  </geometry>"
      "  <surface>"
      "    <friction>"
      "      <ode>"
      "        <mu>1.1</mu>"
      "      </ode>"
      "    </friction>"
      "  </surface>"
      "</collision>");
  EXPECT_EQ(
      MakeCoulombFrictionFromSdfCollisionOde(sdf_diagnostic_, *sdf_collision),
      CoulombFriction<double>(1.1, default_friction().dynamic_friction()));
}

TEST_F(SceneGraphParserDetail,
       MakeCoulombFrictionFromSdfCollisionOde_FrictionMissing) {
  unique_ptr<sdf::Collision> sdf_collision = MakeSdfCollisionFromString(
      "<collision name = 'some_link_collision'>"
      "  <pose>0.0 0.0 0.0 0.0 0.0 0.0</pose>"
      "  <geometry>"
      "    <plane>"
      "      <normal>1.0 2.0 3.0</normal>"
      "    </plane>"
      "  </geometry>"
      "  <surface>"
      "    <ode>"  // WRONG: This should be //surface/friction/ode.
      "      <mu>0.3</mu>"
      "      <mu2>0.8</mu2>"
      "    </ode>"
      "  </surface>"
      "</collision>");
  // TODO(jwnimmer-tri) Ideally, the misplaced <ode/> element above would
  // report a parsing error and/or raise an exception.  For now though, we
  // ignore it and use the defaults.
  EXPECT_EQ(
      MakeCoulombFrictionFromSdfCollisionOde(sdf_diagnostic_, *sdf_collision),
      default_friction());
}

TEST_F(SceneGraphParserDetail,
       MakeCoulombFrictionFromSdfCollisionOde_OdeMissing) {
  unique_ptr<sdf::Collision> sdf_collision = MakeSdfCollisionFromString(
      "<collision name = 'some_link_collision'>"
      "  <pose>0.0 0.0 0.0 0.0 0.0 0.0</pose>"
      "  <geometry>"
      "    <plane>"
      "      <normal>1.0 2.0 3.0</normal>"
      "    </plane>"
      "  </geometry>"
      "  <surface>"
      "    <friction>"  // WRONG: This should be //surface/friction/ode.
      "      <mu>0.3</mu>"
      "      <mu2>0.8</mu2>"
      "    </friction>"
      "  </surface>"
      "</collision>");
  // TODO(jwnimmer-tri) Ideally, the misplaced <friction/> element above would
  // report a parsing error and/or raise an exception.  For now though, we
  // ignore it and use the defaults.
  EXPECT_EQ(
      MakeCoulombFrictionFromSdfCollisionOde(sdf_diagnostic_, *sdf_collision),
      default_friction());
}

}  // namespace
}  // namespace internal
}  // namespace multibody
}  // namespace drake
