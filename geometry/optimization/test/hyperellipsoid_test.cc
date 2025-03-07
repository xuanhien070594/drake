#include "drake/geometry/optimization/hyperellipsoid.h"

#include <limits>

#include <gtest/gtest.h>

#include "drake/common/eigen_types.h"
#include "drake/common/test_utilities/eigen_matrix_compare.h"
#include "drake/common/test_utilities/expect_throws_message.h"
#include "drake/common/yaml/yaml_io.h"
#include "drake/geometry/geometry_frame.h"
#include "drake/geometry/optimization/hpolyhedron.h"
#include "drake/geometry/optimization/test_utilities.h"
#include "drake/geometry/scene_graph.h"
#include "drake/math/random_rotation.h"
#include "drake/math/rigid_transform.h"
#include "drake/solvers/solve.h"

namespace drake {
namespace geometry {
namespace optimization {

using Eigen::Matrix2d;
using Eigen::Matrix3d;
using Eigen::MatrixXd;
using Eigen::RowVector2d;
using Eigen::Vector2d;
using Eigen::Vector3d;
using Eigen::Vector4d;
using Eigen::VectorXd;
using internal::CheckAddPointInSetConstraints;
using internal::MakeSceneGraphWithShape;
using math::RigidTransformd;
using math::RotationMatrixd;
using solvers::Binding;
using solvers::Constraint;
using solvers::MathematicalProgram;

GTEST_TEST(HyperellipsoidTest, UnitSphereTest) {
  // Test constructor.
  const Eigen::Matrix3d A = Eigen::Matrix3d::Identity();
  const Vector3d center = Vector3d::Zero();
  Hyperellipsoid E(A, center);
  EXPECT_EQ(E.ambient_dimension(), 3);
  EXPECT_TRUE(CompareMatrices(A, E.A()));
  EXPECT_TRUE(CompareMatrices(center, E.center()));

  // Test SceneGraph constructor.
  auto [scene_graph, geom_id] =
      MakeSceneGraphWithShape(Sphere(1.0), RigidTransformd::Identity());
  auto context = scene_graph->CreateDefaultContext();
  auto query =
      scene_graph->get_query_output_port().Eval<QueryObject<double>>(*context);

  Hyperellipsoid E_scene_graph(query, geom_id);
  EXPECT_TRUE(CompareMatrices(A, E_scene_graph.A()));
  EXPECT_TRUE(CompareMatrices(center, E_scene_graph.center()));

  // Test MaybeGetPoint.
  EXPECT_FALSE(E.MaybeGetPoint().has_value());

  // Test IsEmpty (which is trivially false for Hyperellipsoid).
  EXPECT_FALSE(E.IsEmpty());

  // Test PointInSet.
  const Vector3d in1_W{.99, 0, 0}, in2_W{.5, .5, .5}, out1_W{1.01, 0, 0},
      out2_W{1.0, 1.0, 1.0};

  EXPECT_LE(query.ComputeSignedDistanceToPoint(in1_W)[0].distance, 0.0);
  EXPECT_LE(query.ComputeSignedDistanceToPoint(in2_W)[0].distance, 0.0);
  EXPECT_GE(query.ComputeSignedDistanceToPoint(out1_W)[0].distance, 0.0);
  EXPECT_GE(query.ComputeSignedDistanceToPoint(out2_W)[0].distance, 0.0);

  EXPECT_TRUE(E.PointInSet(in1_W));
  EXPECT_TRUE(E.PointInSet(in2_W));
  EXPECT_FALSE(E.PointInSet(out1_W));
  EXPECT_FALSE(E.PointInSet(out2_W));

  EXPECT_TRUE(CheckAddPointInSetConstraints(E, in1_W));
  EXPECT_TRUE(CheckAddPointInSetConstraints(E, in2_W));
  EXPECT_FALSE(CheckAddPointInSetConstraints(E, out1_W));
  EXPECT_FALSE(CheckAddPointInSetConstraints(E, out2_W));

  // Test ToShapeWithPose.
  auto [shape, X_WG] = E.ToShapeWithPose();
  Ellipsoid* ellipsoid = dynamic_cast<Ellipsoid*>(shape.get());
  EXPECT_TRUE(ellipsoid != NULL);
  EXPECT_NEAR(ellipsoid->a(), 1.0, 1e-16);
  EXPECT_NEAR(ellipsoid->b(), 1.0, 1e-16);
  EXPECT_NEAR(ellipsoid->c(), 1.0, 1e-16);
  EXPECT_TRUE(CompareMatrices(X_WG.translation(), center, 1e-16));
  // Note: Any rotation in X_WG yields a correct solution; since the unit
  // sphere is rotationally symmetric.
}

GTEST_TEST(HyperellipsoidTest, DefaultCtor) {
  const Hyperellipsoid dut;
  EXPECT_EQ(dut.A().rows(), 0);
  EXPECT_EQ(dut.A().cols(), 0);
  EXPECT_EQ(dut.center().size(), 0);
  EXPECT_EQ(dut.Volume(), 0.0);
  EXPECT_THROW(dut.MinimumUniformScalingToTouch(dut), std::exception);
  EXPECT_NO_THROW(dut.Clone());
  EXPECT_EQ(dut.ambient_dimension(), 0);
  EXPECT_FALSE(dut.IntersectsWith(dut));
  EXPECT_TRUE(dut.IsBounded());
  EXPECT_THROW(dut.IsEmpty(), std::exception);
  EXPECT_FALSE(dut.PointInSet(Eigen::VectorXd::Zero(0)));
}

GTEST_TEST(HyperellipsoidTest, Move) {
  const Eigen::Matrix3d A = Eigen::Matrix3d::Identity();
  const Vector3d center = Vector3d::Zero();
  Hyperellipsoid orig(A, center);

  // A move-constructed Hyperellipsoid takes over the original data.
  Hyperellipsoid dut(std::move(orig));
  EXPECT_EQ(dut.ambient_dimension(), 3);
  EXPECT_TRUE(CompareMatrices(dut.A(), A));
  EXPECT_TRUE(CompareMatrices(dut.center(), center));

  // The old Hyperellipsoid is in a valid but unspecified state.
  EXPECT_EQ(orig.A().cols(), orig.ambient_dimension());
  EXPECT_EQ(orig.center().size(), orig.ambient_dimension());
  EXPECT_NO_THROW(orig.Clone());
}

GTEST_TEST(HyperellipsoidTest, ScaledSphereTest) {
  const double radius = 0.1;
  auto [scene_graph, geom_id] =
      MakeSceneGraphWithShape(Sphere(radius), RigidTransformd::Identity());
  auto context = scene_graph->CreateDefaultContext();
  auto query =
      scene_graph->get_query_output_port().Eval<QueryObject<double>>(*context);

  Hyperellipsoid E(query, geom_id);
  EXPECT_TRUE(
      CompareMatrices(E.A(), Eigen::Matrix3d::Identity() / radius, 1e-16));
  EXPECT_TRUE(E.center().isZero(1e-16));

  auto [shape, X_WS] = E.ToShapeWithPose();

  Ellipsoid* ellipsoid = dynamic_cast<Ellipsoid*>(shape.get());
  EXPECT_TRUE(ellipsoid != nullptr);
  EXPECT_NEAR(ellipsoid->a(), radius, 1e-16);
  EXPECT_NEAR(ellipsoid->b(), radius, 1e-16);
  EXPECT_NEAR(ellipsoid->c(), radius, 1e-16);

  EXPECT_TRUE(X_WS.IsNearlyIdentity(1e-16));
}

GTEST_TEST(HyperellipsoidTest, ArbitraryEllipsoidTest) {
  const Eigen::Matrix3d D = Eigen::DiagonalMatrix<double, 3>(1.0, 2.0, 3.0);
  const RotationMatrixd R = RotationMatrixd::MakeZRotation(M_PI / 2.0);
  const Eigen::Matrix3d A = D * R.matrix();
  const Vector3d center{4.0, 5.0, 6.0};

  Hyperellipsoid E(A, center);
  EXPECT_EQ(E.ambient_dimension(), 3);
  EXPECT_TRUE(CompareMatrices(A, E.A()));
  EXPECT_TRUE(CompareMatrices(center, E.center()));

  // Test SceneGraph constructor.
  auto [scene_graph, geom_id] = MakeSceneGraphWithShape(
      Ellipsoid(1.0 / D(0, 0), 1.0 / D(1, 1), 1.0 / D(2, 2)),
      RigidTransformd{R.inverse(), center});
  auto context = scene_graph->CreateDefaultContext();
  auto query =
      scene_graph->get_query_output_port().Eval<QueryObject<double>>(*context);

  Hyperellipsoid E_scene_graph(query, geom_id);
  EXPECT_TRUE(CompareMatrices(A, E_scene_graph.A()));
  EXPECT_TRUE(CompareMatrices(center, E_scene_graph.center(), 1e-15));

  // Test PointInSet.
  Vector3d in1_W{.99, 0, 0}, in2_W{.5, .5, .5}, out1_W{1.01, 0, 0},
      out2_W{1.0, 1.0, 1.0};
  const Eigen::Matrix3d B = R.inverse() * D.inverse();
  in1_W = B * in1_W + center;
  in2_W = B * in2_W + center;
  out1_W = B * out1_W + center;
  out2_W = B * out2_W + center;

  EXPECT_LE(query.ComputeSignedDistanceToPoint(in1_W)[0].distance, 0.0);
  EXPECT_LE(query.ComputeSignedDistanceToPoint(in2_W)[0].distance, 0.0);
  EXPECT_GE(query.ComputeSignedDistanceToPoint(out1_W)[0].distance, 0.0);
  EXPECT_GE(query.ComputeSignedDistanceToPoint(out2_W)[0].distance, 0.0);

  EXPECT_TRUE(E.PointInSet(in1_W));
  EXPECT_TRUE(E.PointInSet(in2_W));
  EXPECT_FALSE(E.PointInSet(out1_W));
  EXPECT_FALSE(E.PointInSet(out2_W));

  EXPECT_TRUE(CheckAddPointInSetConstraints(E, in1_W));
  EXPECT_TRUE(CheckAddPointInSetConstraints(E, in2_W));
  EXPECT_FALSE(CheckAddPointInSetConstraints(E, out1_W));
  EXPECT_FALSE(CheckAddPointInSetConstraints(E, out2_W));

  // Test reference_frame frame.
  SourceId source_id = scene_graph->RegisterSource("F");
  FrameId frame_id = scene_graph->RegisterFrame(source_id, GeometryFrame("F"));
  auto context2 = scene_graph->CreateDefaultContext();
  const RigidTransformd X_WF{math::RollPitchYawd(.1, .2, 3),
                             Vector3d{.5, .87, .1}};
  const FramePoseVector<double> pose_vector{{frame_id, X_WF}};
  scene_graph->get_source_pose_port(source_id).FixValue(context2.get(),
                                                        pose_vector);
  auto query2 =
      scene_graph->get_query_output_port().Eval<QueryObject<double>>(*context2);
  Hyperellipsoid E_F(query2, geom_id, frame_id);

  const RigidTransformd X_FW = X_WF.inverse();
  EXPECT_TRUE(E_F.PointInSet(X_FW * in1_W));
  EXPECT_TRUE(E_F.PointInSet(X_FW * in2_W));
  EXPECT_FALSE(E_F.PointInSet(X_FW * out1_W));
  EXPECT_FALSE(E_F.PointInSet(X_FW * out2_W));

  // Test ToShapeWithPose.
  auto [shape, X_WG] = E.ToShapeWithPose();
  Ellipsoid* ellipsoid = dynamic_cast<Ellipsoid*>(shape.get());
  EXPECT_TRUE(ellipsoid != NULL);
  // Confirm that the axes are ordered by magnitude as documented.
  EXPECT_GE(ellipsoid->a(), ellipsoid->b());
  EXPECT_GE(ellipsoid->b(), ellipsoid->c());
  // Note: D was intentionally ordered by magnitude, also.
  EXPECT_NEAR(ellipsoid->a(), 1.0 / D(0, 0), 1e-16);
  EXPECT_NEAR(ellipsoid->b(), 1.0 / D(1, 1), 1e-16);
  EXPECT_NEAR(ellipsoid->c(), 1.0 / D(2, 2), 1e-16);
  EXPECT_TRUE(CompareMatrices(X_WG.translation(), center, 1e-16));
  const Eigen::Matrix3d R_WG = X_WG.rotation().matrix();
  const Eigen::Matrix3d R_WG_expected = R.inverse().matrix();
  // We can only expect the transforms to match up to a symmetry.
  // Check that the columns are either aligned or antipolar.
  for (int col = 0; col < 3; col++) {
    EXPECT_GE(std::abs(R_WG.col(col).dot(R_WG_expected.col(col))), 1.0 - 1e-16);
  }
}

GTEST_TEST(HyperellipsoidTest, UnitBall6DTest) {
  Hyperellipsoid E = Hyperellipsoid::MakeUnitBall(6);
  EXPECT_EQ(E.ambient_dimension(), 6);

  const double kScale = sqrt(1.0 / 6.0);
  Vector6d in1_W{Vector6d::Constant(-.99 * kScale)},
      in2_W{Vector6d::Constant(.99 * kScale)},
      out1_W{Vector6d::Constant(-1.01 * kScale)},
      out2_W{Vector6d::Constant(1.01 * kScale)};

  EXPECT_TRUE(E.PointInSet(in1_W));
  EXPECT_TRUE(E.PointInSet(in2_W));
  EXPECT_FALSE(E.PointInSet(out1_W));
  EXPECT_FALSE(E.PointInSet(out2_W));
}

GTEST_TEST(HyperellipsoidTest, CloneTest) {
  Hyperellipsoid E(Eigen::Matrix<double, 6, 6>::Identity(), Vector6d::Zero());
  std::unique_ptr<ConvexSet> clone = E.Clone();
  EXPECT_EQ(clone->ambient_dimension(), E.ambient_dimension());
  Hyperellipsoid* pointer = dynamic_cast<Hyperellipsoid*>(clone.get());
  ASSERT_NE(pointer, nullptr);
  EXPECT_TRUE(CompareMatrices(E.A(), pointer->A()));
  EXPECT_TRUE(CompareMatrices(E.center(), pointer->center()));
}

GTEST_TEST(HyperellipsoidTest, NonnegativeScalingTest) {
  const Vector3d center{2, 2, 2};
  Hyperellipsoid E(Matrix3d(Vector3d{1, 2, 3}.asDiagonal()), center);

  MathematicalProgram prog;
  auto x = prog.NewContinuousVariables(3, "x");
  auto t = prog.NewContinuousVariables(1, "t")[0];

  std::vector<Binding<Constraint>> constraints =
      E.AddPointInNonnegativeScalingConstraints(&prog, x, t);

  EXPECT_EQ(constraints.size(), 2);

  // The point farthest from the origin on the set boundary on the line x=y=z.
  // x^2 + 4x^2 + 9x^2 = 1 => x = sqrt(1 / 14).
  const Vector3d ub = center + sqrt(1.0 / 14.0) * Vector3d::Ones();
  const double tol = 0;

  prog.SetInitialGuess(x, 0.99 * ub);
  prog.SetInitialGuess(t, 1.0);
  EXPECT_TRUE(prog.CheckSatisfiedAtInitialGuess(constraints, tol));

  prog.SetInitialGuess(x, 1.01 * ub);
  prog.SetInitialGuess(t, 1.0);
  EXPECT_FALSE(prog.CheckSatisfiedAtInitialGuess(constraints, tol));

  prog.SetInitialGuess(x, 0.99 * ub);
  prog.SetInitialGuess(t, -0.01);
  EXPECT_FALSE(prog.CheckSatisfiedAtInitialGuess(constraints, tol));

  prog.SetInitialGuess(x, 0.49 * ub);
  prog.SetInitialGuess(t, 0.5);
  EXPECT_TRUE(prog.CheckSatisfiedAtInitialGuess(constraints, tol));

  prog.SetInitialGuess(x, 0.51 * ub);
  prog.SetInitialGuess(t, 0.5);
  EXPECT_FALSE(prog.CheckSatisfiedAtInitialGuess(constraints, tol));

  prog.SetInitialGuess(x, 1.99 * ub);
  prog.SetInitialGuess(t, 2.0);
  EXPECT_TRUE(prog.CheckSatisfiedAtInitialGuess(constraints, tol));

  prog.SetInitialGuess(x, 2.01 * ub);
  prog.SetInitialGuess(t, 2.0);
  EXPECT_FALSE(prog.CheckSatisfiedAtInitialGuess(constraints, tol));
}

bool PointInScaledSet(const solvers::VectorXDecisionVariable& x_vars,
                      const solvers::VectorXDecisionVariable& t_vars,
                      const Vector2d& x, const Vector2d& t,
                      solvers::MathematicalProgram* prog,
                      const std::vector<Binding<Constraint>>& constraints) {
  const double tol = 0;
  prog->SetInitialGuess(x_vars, x);
  prog->SetInitialGuess(t_vars, t);
  return prog->CheckSatisfiedAtInitialGuess(constraints, tol);
}

GTEST_TEST(HyperellipsoidTest, NonnegativeScalingTest2) {
  const Vector3d center{2, 2, 2};
  Hyperellipsoid E(Matrix3d(Vector3d{1, 2, 3}.asDiagonal()), center);

  MathematicalProgram prog;
  Eigen::MatrixXd A(3, 2);
  // clang-format off
  A << 1, 0,
       0, 1,
       1, 0;
  // clang-format on
  Vector3d b = Vector3d::Zero();
  auto x = prog.NewContinuousVariables(2, "x");
  Eigen::Vector2d c(1, -1);
  double d = 0;
  auto t = prog.NewContinuousVariables(2, "t");

  std::vector<Binding<Constraint>> constraints =
      E.AddPointInNonnegativeScalingConstraints(&prog, A, b, c, d, x, t);

  EXPECT_EQ(constraints.size(), 2);

  // The point farthest from the origin on the set boundary on the line x=y=z.
  // x^2 + 4x^2 + 9x^2 = 1 => x = sqrt(1 / 14).
  const Vector2d ub = center.head(2) + sqrt(1.0 / 14.0) * Vector2d::Ones();

  EXPECT_TRUE(
      PointInScaledSet(x, t, 0.99 * ub, Vector2d(1.0, 0), &prog, constraints));
  EXPECT_TRUE(
      PointInScaledSet(x, t, 0.99 * ub, Vector2d(0, -1.0), &prog, constraints));
  EXPECT_FALSE(
      PointInScaledSet(x, t, 1.01 * ub, Vector2d(1.0, 0), &prog, constraints));
  EXPECT_FALSE(
      PointInScaledSet(x, t, 1.01 * ub, Vector2d(0, -1.0), &prog, constraints));
  EXPECT_FALSE(PointInScaledSet(x, t, 0.99 * ub, Vector2d(-0.01, 0), &prog,
                                constraints));
  EXPECT_FALSE(PointInScaledSet(x, t, 0.99 * ub, Vector2d(0, -0.01), &prog,
                                constraints));
  EXPECT_TRUE(
      PointInScaledSet(x, t, 0.49 * ub, Vector2d(0.5, 0), &prog, constraints));
  EXPECT_TRUE(
      PointInScaledSet(x, t, 0.49 * ub, Vector2d(0, -0.5), &prog, constraints));
  EXPECT_FALSE(
      PointInScaledSet(x, t, 0.51 * ub, Vector2d(0.5, 0), &prog, constraints));
  EXPECT_FALSE(
      PointInScaledSet(x, t, 0.51 * ub, Vector2d(0, -0.5), &prog, constraints));
  EXPECT_TRUE(
      PointInScaledSet(x, t, 1.99 * ub, Vector2d(2.0, 0), &prog, constraints));
  EXPECT_TRUE(
      PointInScaledSet(x, t, 1.99 * ub, Vector2d(0, -2.0), &prog, constraints));
  EXPECT_FALSE(
      PointInScaledSet(x, t, 2.01 * ub, Vector2d(2.0, 0), &prog, constraints));
  EXPECT_FALSE(
      PointInScaledSet(x, t, 2.01 * ub, Vector2d(0, -2.0), &prog, constraints));
}

GTEST_TEST(HyperellisoidTest, MakeUnitBallTest) {
  Hyperellipsoid E = Hyperellipsoid::MakeUnitBall(4);
  EXPECT_TRUE(CompareMatrices(E.A(), MatrixXd::Identity(4, 4)));
  EXPECT_TRUE(CompareMatrices(E.center(), VectorXd::Zero(4)));
}

GTEST_TEST(HyperellipsoidTest, MakeHypersphereTest) {
  const double kRadius = 3.0;
  Vector4d center;
  center << 1.3, 1.4, 7.2, 9.1;
  Hyperellipsoid E = Hyperellipsoid::MakeHypersphere(kRadius, center);
  EXPECT_TRUE(CompareMatrices(E.A(), MatrixXd::Identity(4, 4) / kRadius));
  EXPECT_TRUE(CompareMatrices(E.center(), center));
  const Vector4d xvec = Eigen::MatrixXd::Identity(4, 1);
  EXPECT_TRUE(E.PointInSet(center + kRadius * xvec, 1e-16));
  EXPECT_FALSE(E.PointInSet(center + 1.1 * kRadius * xvec, 1e-16));
}

GTEST_TEST(HyperellipsoidTest, MakeAxisAlignedTest) {
  const double a = 2.3, b = 4.5, c = 6.1;
  const Vector3d center{3.4, -2.3, 7.4};
  Hyperellipsoid E = Hyperellipsoid::MakeAxisAligned(Vector3d{a, b, c}, center);
  EXPECT_EQ(E.ambient_dimension(), 3);

  auto [scene_graph, geom_id] =
      MakeSceneGraphWithShape(Ellipsoid(a, b, c), RigidTransformd{center});
  auto context = scene_graph->CreateDefaultContext();
  auto query =
      scene_graph->get_query_output_port().Eval<QueryObject<double>>(*context);

  Hyperellipsoid E_scene_graph(query, geom_id);
  EXPECT_TRUE(CompareMatrices(E.A(), E_scene_graph.A(), 1e-16));
  EXPECT_TRUE(CompareMatrices(E.center(), E_scene_graph.center(), 1e-16));
}

GTEST_TEST(HyperellipsoidTest, IsBoundedAndVolumeTest) {
  // A is 2x3, so the ellipsoid is unbounded.
  Hyperellipsoid E(MatrixXd::Identity(2, 3), Vector3d::Zero());
  EXPECT_FALSE(E.IsBounded());
  EXPECT_EQ(E.Volume(), std::numeric_limits<double>::infinity());
}

GTEST_TEST(HyperellipsoidTest, IsBoundedAndVolumeTest2) {
  // A is 3x3 but low rank, so the ellipsoid is unbounded.
  Hyperellipsoid E(Matrix3d(Vector3d(1.0, 1.0, 0).asDiagonal()),
                   Vector3d::Zero());
  EXPECT_FALSE(E.IsBounded());
  EXPECT_EQ(E.Volume(), std::numeric_limits<double>::infinity());
}

GTEST_TEST(HyperellipsoidTest, IsBoundedAndVolumeTest3) {
  Hyperellipsoid E = Hyperellipsoid::MakeUnitBall(2);
  EXPECT_TRUE(E.IsBounded());
  EXPECT_NEAR(E.Volume(), M_PI, 1e-16);
}

GTEST_TEST(HyperellipsoidTest, IsBoundedAndVolumeTest4) {
  const double kRadius = 4.0;
  Hyperellipsoid E =
      Hyperellipsoid::MakeHypersphere(kRadius, VectorXd::Ones(8));
  EXPECT_TRUE(E.IsBounded());
  EXPECT_NEAR(E.Volume(), std::pow(M_PI, 4) / 24.0 * std::pow(kRadius, 8),
              1e-16);
}

GTEST_TEST(HyperellipsoidTest, IsBoundedAndVolumeTest5) {
  const double a = 2.3, b = 4.5, c = 6.1;
  const Vector3d center{3.4, -2.3, 7.4};
  Hyperellipsoid E = Hyperellipsoid::MakeAxisAligned(Vector3d{a, b, c}, center);
  EXPECT_TRUE(E.IsBounded());
  EXPECT_NEAR(E.Volume(), 4.0 / 3.0 * M_PI * a * b * c, 1e-16);
}

GTEST_TEST(HyperellipsoidTest, MinimumUniformScaling) {
  const double a = 2.5, b = 0.3;
  Hyperellipsoid E =
      Hyperellipsoid::MakeAxisAligned(Vector2d{a, b}, Vector2d::Ones());

  const double kTol = 1e-6;  // Solver tolerances are large.

  {
    // x ≥ 5 (aka -x ≤ -5).
    HPolyhedron H(RowVector2d(-1.0, 0.0), Vector1d(-5.0));
    auto [sigma, x] = E.MinimumUniformScalingToTouch(H);
    EXPECT_NEAR(sigma, (5.0 - 1.0) / a, kTol);
    EXPECT_TRUE(CompareMatrices(x, Vector2d{5.0, 1.0}, kTol));
  }

  {
    // y ≥ 5.
    HPolyhedron H(RowVector2d(0.0, -1.0), Vector1d(-5.0));
    auto [sigma, x] = E.MinimumUniformScalingToTouch(H);
    EXPECT_NEAR(sigma, (5.0 - 1.0) / b, kTol);
    EXPECT_TRUE(CompareMatrices(x, Vector2d{1.0, 5.0}, kTol));
  }
}

GTEST_TEST(HyperellipsoidTest, MinimumUniformScaling2) {
  Hyperellipsoid E = Hyperellipsoid::MakeUnitBall(2);

  const double kTol = 1e-6;  // Solver tolerances are large.
  // x+y ≥ 3.  x* = [1.5, 1.5], σ* = |x*|₂.
  const Vector2d xstar{1.5, 1.5};
  HPolyhedron H(RowVector2d(-1.0, -1.0), Vector1d(-3.0));
  auto [sigma, x] = E.MinimumUniformScalingToTouch(H);
  EXPECT_NEAR(sigma, xstar.norm(), kTol);
  EXPECT_TRUE(CompareMatrices(x, xstar, kTol));
}

// Confirm that the scaling is zero if E.center() is an element in the other
// set.
GTEST_TEST(HyperellipsoidTest, MinimumUniformScaling3) {
  Hyperellipsoid E = Hyperellipsoid::MakeUnitBall(2);
  HPolyhedron H = HPolyhedron::MakeUnitBox(2);

  const double kTol = 1e-6;  // Solver tolerances are large.
  auto [sigma, x] = E.MinimumUniformScalingToTouch(H);
  EXPECT_NEAR(sigma, 0, kTol);
  EXPECT_TRUE(CompareMatrices(x, Vector2d::Zero(), kTol));
}

// Check with another hyperellipsoid (to test SOCP constraint).
GTEST_TEST(HyperellipsoidTest, MinimumUniformScaling4) {
  Hyperellipsoid E = Hyperellipsoid::MakeUnitBall(2);
  Hyperellipsoid E2 = Hyperellipsoid::MakeHypersphere(1.0, Vector2d{4.0, 0.0});

  const double kTol = 1e-5;  // SCS requires this large tolerance.
  auto [sigma, x] = E.MinimumUniformScalingToTouch(E2);
  EXPECT_NEAR(sigma, 3.0, kTol);
  EXPECT_TRUE(CompareMatrices(x, Vector2d{3.0, 0.0}, kTol));
}

// Check the case when `other` has no interior.
GTEST_TEST(HyperellipsoidTest, MinimumUniformScaling5) {
  const Hyperellipsoid E = Hyperellipsoid::MakeUnitBall(2);
  // x₀ ≤ -1, x₀ ≥ 1.
  Matrix2d A;
  // clang-format off
  A <<  1, 0,
       -1, 0;
  // clang-format on
  const HPolyhedron H(A, Vector2d(-1, -1));
  DRAKE_EXPECT_THROWS_MESSAGE(E.MinimumUniformScalingToTouch(H),
                              ".*is empty.*");
}

GTEST_TEST(HyperellipsoidTest, Serialize) {
  Hyperellipsoid E = Hyperellipsoid::MakeUnitBall(2);
  const std::string yaml = yaml::SaveYamlString(E);
  const auto E2 = yaml::LoadYamlString<Hyperellipsoid>(yaml);
  EXPECT_EQ(E.ambient_dimension(), E2.ambient_dimension());
  EXPECT_TRUE(CompareMatrices(E.A(), E2.A()));
  EXPECT_TRUE(CompareMatrices(E.center(), E2.center()));
}

}  // namespace optimization
}  // namespace geometry
}  // namespace drake
