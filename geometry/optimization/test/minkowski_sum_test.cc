#include "drake/geometry/optimization/minkowski_sum.h"

#include <limits>

#include <gtest/gtest.h>

#include "drake/common/copyable_unique_ptr.h"
#include "drake/common/eigen_types.h"
#include "drake/common/test_utilities/eigen_matrix_compare.h"
#include "drake/geometry/geometry_frame.h"
#include "drake/geometry/optimization/hpolyhedron.h"
#include "drake/geometry/optimization/point.h"
#include "drake/geometry/optimization/test_utilities.h"
#include "drake/geometry/optimization/vpolytope.h"
#include "drake/geometry/scene_graph.h"
#include "drake/math/rigid_transform.h"
#include "drake/solvers/solve.h"

namespace drake {
namespace geometry {
namespace optimization {

using Eigen::Matrix;
using Eigen::Vector2d;
using Eigen::Vector3d;
using math::RigidTransformd;

GTEST_TEST(MinkowskiSumTest, BasicTest) {
  const Point P1(Vector2d{1.2, 3.4}), P2(Vector2d{5.6, 7.8});
  const MinkowskiSum S(P1, P2);
  EXPECT_EQ(S.num_terms(), 2);
  EXPECT_EQ(S.ambient_dimension(), 2);

  // Test MaybeGetPoint.
  ASSERT_TRUE(S.MaybeGetPoint().has_value());
  EXPECT_TRUE(CompareMatrices(S.MaybeGetPoint().value(), P1.x() + P2.x()));

  // Test PointInSet.
  Vector2d in{P1.x() + P2.x()}, out{P1.x() + P2.x() + Vector2d::Constant(0.01)};
  EXPECT_TRUE(S.PointInSet(in));
  EXPECT_FALSE(S.PointInSet(out));

  EXPECT_TRUE(internal::CheckAddPointInSetConstraints(S, in));
  EXPECT_FALSE(internal::CheckAddPointInSetConstraints(S, out));
  {
    // Check new variables in AddPointInSetConstraints;
    solvers::MathematicalProgram prog;
    auto x = prog.NewContinuousVariables<2>();
    auto [new_vars, new_constraints] = S.AddPointInSetConstraints(&prog, x);
    EXPECT_EQ(new_vars.rows(), 4);
    // It has to contain at least 3 constraints, two constraints on y1 in set1
    // and y2 in set 2, and the third constraint x = y1 + y2.
    EXPECT_GE(new_constraints.size(), 3);
    const auto result = solvers::Solve(prog);
    const auto new_vars_val = result.GetSolution(new_vars);
    EXPECT_TRUE(CompareMatrices(new_vars_val.head<2>(), P1.x(), 1E-10));
    EXPECT_TRUE(CompareMatrices(new_vars_val.tail<2>(), P2.x(), 1E-10));
  }

  // Test IsBounded.
  EXPECT_TRUE(S.IsBounded());

  // Test IsEmpty.
  EXPECT_FALSE(S.IsEmpty());

  // Test ConvexSets constructor.
  ConvexSets sets;
  sets.emplace_back(P1);
  sets.emplace_back(P2);
  const MinkowskiSum S2(sets);
  EXPECT_EQ(S2.num_terms(), 2);
  EXPECT_EQ(S2.ambient_dimension(), 2);
  EXPECT_TRUE(S2.PointInSet(in));
  EXPECT_FALSE(S2.PointInSet(out));
}

GTEST_TEST(MinkowskiSumTest, DefaultCtor) {
  const MinkowskiSum dut;
  EXPECT_EQ(dut.num_terms(), 0);
  EXPECT_NO_THROW(dut.Clone());
  EXPECT_EQ(dut.ambient_dimension(), 0);
  EXPECT_FALSE(dut.IntersectsWith(dut));
  EXPECT_TRUE(dut.IsBounded());
  EXPECT_THROW(dut.IsEmpty(), std::exception);
  EXPECT_FALSE(dut.MaybeGetPoint().has_value());
  EXPECT_FALSE(dut.PointInSet(Eigen::VectorXd::Zero(0)));
}

GTEST_TEST(MinkowskiSumTest, Move) {
  const Point P1(Vector2d{1.2, 3.4}), P2(Vector2d{5.6, 7.8});
  MinkowskiSum orig(P1, P2);

  // A move-constructed MinkowskiSum takes over the original data.
  MinkowskiSum dut(std::move(orig));
  EXPECT_EQ(dut.num_terms(), 2);
  EXPECT_EQ(dut.ambient_dimension(), 2);

  // The old set is in a valid but unspecified state. For convenience we'll
  // assert that it's empty, but that's not the only valid implementation,
  // just the one we happen to currently use.
  EXPECT_EQ(orig.num_terms(), 0);
  EXPECT_EQ(orig.ambient_dimension(), 0);
  EXPECT_NO_THROW(orig.Clone());
}

GTEST_TEST(MinkowskiSumTest, FromSceneGraph) {
  const RigidTransformd X_WG{math::RollPitchYawd(.1, .2, 3),
                             Vector3d{.5, .87, .1}};

  // Test SceneGraph constructor.
  const double kRadius = 0.2;
  const double kLength = 0.5;
  auto [scene_graph, geom_id] =
      internal::MakeSceneGraphWithShape(Capsule(kRadius, kLength), X_WG);
  auto context = scene_graph->CreateDefaultContext();
  auto query =
      scene_graph->get_query_output_port().Eval<QueryObject<double>>(*context);

  MinkowskiSum S(query, geom_id, std::nullopt);
  Matrix<double, 3, 4> in_G, out_G;
  // clang-format off
  in_G << 0, 0,                    kRadius,     kRadius/std::sqrt(2.0),
          0, 0,                    0,           kRadius/std::sqrt(2.0),
          0, kRadius+kLength/2.0, -kLength/2.0, kLength/2.0;
  out_G << kRadius+.01, 0,          kRadius,     kRadius/std::sqrt(2.0),
           0, 0,                    0,           kRadius/std::sqrt(2.0),
           0, kRadius+kLength/2.0+.01, -kLength/2.0-.01, kLength/2.0+.01;
  // clang-format on
  const Matrix<double, 3, 4> in_W = X_WG * in_G, out_W = X_WG * out_G;
  const double kTol = 1e-14;
  for (int i = 0; i < 4; ++i) {
    EXPECT_LE(query.ComputeSignedDistanceToPoint(in_W.col(i))[0].distance,
              kTol);
    EXPECT_GE(query.ComputeSignedDistanceToPoint(out_W.col(i))[0].distance,
              -kTol);

    EXPECT_TRUE(S.PointInSet(in_W.col(i)));
    EXPECT_FALSE(S.PointInSet(out_W.col(i)));
  }

  // Test reference_frame frame.
  SourceId source_id = scene_graph->RegisterSource("F");
  FrameId frame_id = scene_graph->RegisterFrame(source_id, GeometryFrame("F"));
  auto context2 = scene_graph->CreateDefaultContext();
  const RigidTransformd X_WF{math::RollPitchYawd(.5, .26, -3),
                             Vector3d{.9, -2., .12}};
  const FramePoseVector<double> pose_vector{{frame_id, X_WF}};
  scene_graph->get_source_pose_port(source_id).FixValue(context2.get(),
                                                        pose_vector);
  auto query2 =
      scene_graph->get_query_output_port().Eval<QueryObject<double>>(*context2);
  MinkowskiSum S2(query2, geom_id, frame_id);

  const RigidTransformd X_FW = X_WF.inverse();
  const Matrix<double, 3, 4> in_F = X_FW * in_W, out_F = X_FW * out_W;
  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(S2.PointInSet(in_F.col(i)));
    EXPECT_FALSE(S2.PointInSet(out_F.col(i)));
  }
}

GTEST_TEST(MinkowskiSumTest, TwoBoxes) {
  HPolyhedron H = HPolyhedron::MakeBox(Vector2d{1, 1}, Vector2d{2, 2});
  VPolytope V = VPolytope::MakeBox(Vector2d{-2, 2}, Vector2d{0, 4});
  MinkowskiSum S(H, V);
  EXPECT_TRUE(S.PointInSet(Vector2d{-1, 3}));
  EXPECT_FALSE(S.PointInSet(Vector2d{-1, 2.9}));
  EXPECT_FALSE(S.PointInSet(Vector2d{-1.01, 3}));
  EXPECT_FALSE(S.MaybeGetPoint().has_value());
}

GTEST_TEST(MinkowskiSumTest, CloneTest) {
  const Point P1(Vector2d{1.2, 3.4}), P2(Vector2d{5.6, 7.8});
  const MinkowskiSum S(P1, P2);
  std::unique_ptr<ConvexSet> clone = S.Clone();
  EXPECT_EQ(clone->ambient_dimension(), 2);

  MinkowskiSum* s = dynamic_cast<MinkowskiSum*>(clone.get());
  ASSERT_NE(s, nullptr);
  ASSERT_EQ(s->num_terms(), 2);
  const Point* p = dynamic_cast<const Point*>(&s->term(0));
  ASSERT_NE(p, nullptr);
  EXPECT_TRUE(CompareMatrices(P1.x(), p->x()));
}

bool PointInScaledSet(const solvers::VectorXDecisionVariable& x_vars,
                      const symbolic::Variable& t_var, const Vector2d& x,
                      double t, solvers::MathematicalProgram* prog) {
  auto b1 = prog->AddBoundingBoxConstraint(x, x, x_vars);
  auto b2 = prog->AddBoundingBoxConstraint(t, t, t_var);
  auto result = solvers::Solve(*prog);
  prog->RemoveConstraint(b1);
  prog->RemoveConstraint(b2);
  return result.is_success();
}

GTEST_TEST(MinkowskiSumTest, NonnegativeScalingTest) {
  const Point P1(Vector2d{1.2, 3.4}), P2(Vector2d{5.6, 7.8});
  const MinkowskiSum S(P1, P2);

  solvers::MathematicalProgram prog;
  auto x = prog.NewContinuousVariables(2, "x");
  auto t = prog.NewContinuousVariables(1, "t")[0];

  std::vector<solvers::Binding<solvers::Constraint>> constraints =
      S.AddPointInNonnegativeScalingConstraints(&prog, x, t);

  // 1 from P1, 1 from P2, 2 from S, and t>=0 3 times.
  EXPECT_EQ(constraints.size(), 7);

  const Vector2d p = P1.x() + P2.x();
  for (const double scale : {0.5, 1.0, 2.0}) {
    EXPECT_TRUE(PointInScaledSet(x, t, scale * p, scale, &prog));
    EXPECT_FALSE(PointInScaledSet(x, t, 0.99 * scale * p, scale, &prog));
  }
  EXPECT_FALSE(PointInScaledSet(x, t, p, -1.0, &prog));
}

bool PointInScaledSet(const solvers::VectorXDecisionVariable& x_vars,
                      const solvers::VectorXDecisionVariable& t_var,
                      const Vector2d& x, const Vector2d& t,
                      solvers::MathematicalProgram* prog) {
  auto b1 = prog->AddBoundingBoxConstraint(x, x, x_vars);
  auto b2 = prog->AddBoundingBoxConstraint(t, t, t_var);
  auto result = solvers::Solve(*prog);
  prog->RemoveConstraint(b1);
  prog->RemoveConstraint(b2);
  return result.is_success();
}

GTEST_TEST(MinkowskiSumTest, NonnegativeScalingTest2) {
  const Point P1(Vector3d{1.2, 3.4, 2.4}), P2(Vector3d{5.6, 7.8, 11.2});
  const MinkowskiSum S(P1, P2);

  solvers::MathematicalProgram prog;
  Eigen::MatrixXd A(3, 2);
  // clang-format off
  A << 1, 0,
       0, 1,
       2, 0;
  // clang-format on
  Vector3d b = Vector3d::Zero();
  auto x = prog.NewContinuousVariables(2, "x");
  Eigen::Vector2d c(1, -1);
  double d = 0;
  auto t = prog.NewContinuousVariables(2, "t");

  std::vector<solvers::Binding<solvers::Constraint>> constraints =
      S.AddPointInNonnegativeScalingConstraints(&prog, A, b, c, d, x, t);

  // 1 from P1, 1 from P2, 2 from S, and c'*t+d>=0 3 times.
  EXPECT_EQ(constraints.size(), 7);

  Eigen::Vector2d x_solution = P1.x().head(2) + P2.x().head(2);
  Eigen::Vector3d scale(0.5, 1.0, 2.0);
  Eigen::MatrixXd t_solutions(2, 9);
  // clang-format off
  t_solutions << 0.5,  0,  0.25, 1,  0,  0.5, 2,  0,  1,
                 0, -0.5, -0.25, 0, -1, -0.5, 0, -2, -1;
  // clang-format on
  for (int ii = 0; ii < t_solutions.cols(); ++ii) {
    EXPECT_TRUE(PointInScaledSet(x, t, scale[ii / 3] * x_solution,
                                 t_solutions.col(ii), &prog));
    EXPECT_FALSE(PointInScaledSet(x, t, 0.99 * scale[ii / 3] * x_solution,
                                  t_solutions.col(ii), &prog));
  }

  EXPECT_FALSE(
      PointInScaledSet(x, t, x_solution, Eigen::Vector2d(0, 1), &prog));
  EXPECT_FALSE(
      PointInScaledSet(x, t, x_solution, Eigen::Vector2d(-1, 0), &prog));
}

GTEST_TEST(MinkowskiSumTest, EmptyMinkowskiSumTest) {
  Eigen::MatrixXd A{5, 3};
  Eigen::VectorXd b{5};
  // Rows 1-3 define an infeasible set of inequalities.
  // clang-format off
  A << 1, 0, 0,
       1, -1, 0,
       -1, 0, 1,
       0, 1, -1,
       0, 0, -1;
  b << 1, -1, -1, -1, 0;
  // clang-format off

  const HPolyhedron H1{A, b};
  const Point P1(Vector3d{0.1, 1.2, 0.3});
  const MinkowskiSum S(P1, H1);

  EXPECT_TRUE(S.IsEmpty());
}

}  // namespace optimization
}  // namespace geometry
}  // namespace drake
