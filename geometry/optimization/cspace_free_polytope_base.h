#pragma once

#include <array>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "drake/geometry/optimization/c_iris_collision_geometry.h"
#include "drake/geometry/optimization/c_iris_separating_plane.h"
#include "drake/multibody/rational/rational_forward_kinematics.h"
#include "drake/solvers/mathematical_program.h"

namespace drake {
namespace geometry {
namespace optimization {
/**
 This virtual class is the base of CspaceFreePolytope and CspaceFreeBox. We take
 the common functionality between these concrete derived class to this shared
 parent class.
 */
class CspaceFreePolytopeBase {
 public:
  DRAKE_NO_COPY_NO_MOVE_NO_ASSIGN(CspaceFreePolytopeBase)

  using IgnoredCollisionPairs =
      std::unordered_set<SortedPair<geometry::GeometryId>>;

  virtual ~CspaceFreePolytopeBase();

  /** Optional argument for constructing CspaceFreePolytopeBase */
  struct Options {
    Options() {}

    /**
     For non-polytopic collision geometries, we will impose a matrix-sos
     constraint X(s) being psd, with a slack indeterminates y, such that the
     polynomial
     <pre>
     p(s, y) = ⌈ 1 ⌉ᵀ * X(s) * ⌈ 1 ⌉
               ⌊ y ⌋           ⌊ y ⌋
     </pre>
     is positive. This p(s, y) polynomial doesn't contain the cross term of y
     (namely it doesn't have y(i)*y(j), i≠j). When we select the monomial
     basis for this polynomial, we can also exclude the cross term of y in the
     monomial basis.

     To illustrate the idea, let's consider the following toy example: if we
     want to certify that
     a(0) + a(1)*y₀ + a(2)*y₁ + a(3)*y₀² + a(4)*y₁² is positive
     (this polynomial doesn't have the cross term y₀*y₁), we can write it as
     <pre>
     ⌈ 1⌉ᵀ * A₀ * ⌈ 1⌉ + ⌈ 1⌉ᵀ * A₁ * ⌈ 1⌉
     ⌊y₀⌋         ⌊y₀⌋   ⌊y₁⌋         ⌊y₁⌋
     </pre>
     with two small psd matrices A₀, A₁
     Instead of
     <pre>
     ⌈ 1⌉ᵀ * A * ⌈ 1⌉
     |y₀|        |y₀|
     ⌊y₁⌋        ⌊y₁⌋
     </pre>
     with one large psd matrix A. The first parameterization won't have the
     cross term y₀*y₁ by construction, while the second parameterization
     requires imposing extra constraints on certain off-diagonal terms in A
     so that the cross term vanishes.

     If we set with_cross_y = false, then we will use the monomial basis that
     doesn't generate cross terms of y, leading to smaller size sos problems.
     If we set with_cross_y = true, then we will use the monomial basis that
     will generate cross terms of y, causing larger size sos problems, but
     possibly able to certify a larger C-space polytope.
     */
    bool with_cross_y{false};
  };

  /** Getter for the rational forward kinematics object that computes the
   * forward kinematics as rational functions. */
  [[nodiscard]] const multibody::RationalForwardKinematics&
  rational_forward_kin() const {
    return rational_forward_kin_;
  }

  /**
   separating_planes()[map_geometries_to_separating_planes.at(geometry1_id,
   geometry2_id)] is the separating plane that separates geometry 1 and
   geometry 2.
   */
  [[nodiscard]] const std::unordered_map<SortedPair<geometry::GeometryId>, int>&
  map_geometries_to_separating_planes() const {
    return map_geometries_to_separating_planes_;
  }

  /** All the separating planes between each pair of geometries. */
  [[nodiscard]] const std::vector<CIrisSeparatingPlane<symbolic::Variable>>&
  separating_planes() const {
    return separating_planes_;
  }

  /** Get the slack variable used for non-polytopic collision geometries. Check
   Options class for more details.
   */
  [[nodiscard]] const Vector3<symbolic::Variable>& y_slack() const {
    return y_slack_;
  }

 protected:
  /** When we set up the separating plane {x | a(s)ᵀx + b(s) = 0} between a pair
   of geometries, we need to determine which s are used in a(s) and b(s).
   */
  enum class SForPlane {
    kAll,      ///< Use all s in the robot tangent-configuration space.
    kOnChain,  ///< Use s on the kinematics chain between the pair of
               ///< geometries.
  };

  /** Constructor.
   We put the constructor in protected method to make sure that the user
   cannot instantiate a CspaceFreePolytopeBase instance.
   @pre plant and scene_graph should be non-null pointers.
   */
  CspaceFreePolytopeBase(const multibody::MultibodyPlant<double>* plant,
                         const geometry::SceneGraph<double>* scene_graph,
                         SeparatingPlaneOrder plane_order,
                         SForPlane s_for_plane_enum,
                         const Options& options = Options{});

  /** Computes s-s_lower and s_upper - s as polynomials of s. */
  void CalcSBoundsPolynomial(
      const Eigen::VectorXd& s_lower, const Eigen::VectorXd& s_upper,
      VectorX<symbolic::Polynomial>* s_minus_s_lower,
      VectorX<symbolic::Polynomial>* s_upper_minus_s) const;

  /** Returns the index of the plane which will separate the geometry pair.
  Returns -1 if the pair is not in map_geometries_to_separating_planes_.
  */
  int GetSeparatingPlaneIndex(
      const SortedPair<geometry::GeometryId>& pair) const;

  [[nodiscard]] const symbolic::Variables& get_s_set() const { return s_set_; }

  [[nodiscard]] const geometry::SceneGraph<double>& scene_graph() const {
    return *scene_graph_;
  }

  [[nodiscard]] const std::map<
      multibody::BodyIndex,
      std::vector<std::unique_ptr<CIrisCollisionGeometry>>>&
  link_geometries() const {
    return link_geometries_;
  }

  [[nodiscard]] SeparatingPlaneOrder plane_order() const {
    return plane_order_;
  }

  /** Maps a pair of body (body1, body2) to an array of monomial basis
   `monomial_basis_array`. monomial_basis_array[0] contains all the monomials
   of form ∏ᵢ pow(sᵢ, dᵢ), dᵢ=0 or 1, sᵢ correspond to the revolute/prismatic
   joint on the kinematic chain between body1 and body2.
   monomial_basis_array[i+1] = y_slack_[i] * monomial_basis_array[0]
   */
  [[nodiscard]] const std::unordered_map<
      SortedPair<multibody::BodyIndex>,
      std::array<VectorX<symbolic::Monomial>, 4>>&
  map_body_to_monomial_basis_array() const {
    return map_body_to_monomial_basis_array_;
  }

  /** Check Options::with_cross_y for more details. */
  [[nodiscard]] bool with_cross_y() const { return with_cross_y_; }

  /** For a pair of bodies body_pair, returns the indices of all s on the
   kinematics chain from body_pair.first() to body_pair.second().
   For each pair of collidable collision geometry (A, B), we denote their body
   as (bodyA, bodyB). This keys in this map include all these (bodyA, bodyB),
   together with (body_middle, bodyA) and (body_middle, bodyB), where
   body_middle is the body in the middle of the kinematics chain between bodyA
   and bodyB.
   */
  [[nodiscard]] const std::unordered_map<SortedPair<multibody::BodyIndex>,
                                         std::vector<int>>&
  map_body_pair_to_s_on_chain() const {
    return map_body_pair_to_s_on_chain_;
  }

  /** Returns a vector of s variable used in a(s), b(s), which parameterize the
   separating plane {x | a(s)ᵀx+b(s) = 0}.
   */
  [[nodiscard]] VectorX<symbolic::Variable> GetSForPlane(
      const SortedPair<multibody::BodyIndex>& body_pair,
      SForPlane s_for_plane_enum) const;

 private:
  // Forward declare the tester class to test the private members.
  friend class CspaceFreePolytopeBaseTester;
  /*
   Computes the monomial basis for each pair of bodies.

   There can be multiple collision geometries on the same body, and their SOS
   problem will all share the same monomial basis. Hence we can first compute
   the monomial basis for each body, and reuse the result for all the collision
   geometries on the same body pair.
   */
  void CalcMonomialBasis();

  /* Set map_body_pair_to_s_on_chain.
   We compute the indices of s on the kinematics chain from body_pair.first() to
   body_pair.second().
   */
  void SetIndicesOfSOnChainForBodyPair(
      const SortedPair<multibody::BodyIndex>& body_pair);

  multibody::RationalForwardKinematics rational_forward_kin_;
  const geometry::SceneGraph<double>* scene_graph_;
  std::map<multibody::BodyIndex,
           std::vector<std::unique_ptr<CIrisCollisionGeometry>>>
      link_geometries_;

  SeparatingPlaneOrder plane_order_;
  std::vector<CIrisSeparatingPlane<symbolic::Variable>> separating_planes_;
  std::unordered_map<SortedPair<geometry::GeometryId>, int>
      map_geometries_to_separating_planes_;

  // Sometimes we need to impose that a certain matrix of polynomials are always
  // psd (for example with sphere or capsule collision geometries). We will use
  // this slack variable to help us impose the matrix-sos constraint.
  Vector3<symbolic::Variable> y_slack_;

  symbolic::Variables s_set_;

  // Maps a pair of body (body1, body2) to an array of monomial basis
  // `monomial_basis_array`. monomial_basis_array[0] contains all the monomials
  // of form ∏ᵢ pow(sᵢ, dᵢ), dᵢ=0 or 1, sᵢ correspond to the revolute/prismatic
  // joint on the kinematic chain between body1 and body2.
  // monomial_basis_array[i+1] = y_slack_[i] * monomial_basis_array[0]
  std::unordered_map<SortedPair<multibody::BodyIndex>,
                     std::array<VectorX<symbolic::Monomial>, 4>>
      map_body_to_monomial_basis_array_;

  // See Options::with_cross_y for its meaning.
  bool with_cross_y_;

  // For a pair of bodies body_pair, returns the indices of all s on the
  // kinematics chain from body_pair.first() to body_pair.second().
  // For each pair of collidable collision geometry (A, B), we denote their body
  // as (bodyA, bodyB). This keys in this map include all these (bodyA, bodyB),
  // together with (body_middle, bodyA) and (body_middle, bodyB), where
  // body_middle is the body in the middle of the kinematics chain between bodyA
  // and bodyB.
  std::unordered_map<SortedPair<multibody::BodyIndex>, std::vector<int>>
      map_body_pair_to_s_on_chain_;
};

namespace internal {
// Return the total size of the lower triangular variables in the Gram
// matrices. Each Gram matrix should match with the monomial basis in
// `monomial_basis_array`. Depending on whether we include y in the
// indeterminates (see Options::with_cross_y for more details) and the size of
// y, the number of Gram matrices will change.
// @param monomial_basis The candidate monomial_basis for all gram matricies.
// @param with_cross_y See Options::with_cross_y
// @param num_y The size of y indterminates.
int GetGramVarSize(
    const std::array<VectorX<symbolic::Monomial>, 4>& monomial_basis_array,
    bool with_cross_y, int num_y);

// Given the monomial_basis_array, compute the sos polynomial.
// monomial_basis_array contains [m(s), y₀*m(s), y₁*m(s), y₂*m(s)].
//
// If num_y == 0, then the sos polynomial is just
// m(s)ᵀ * X * m(s)
// where X is a Gram matrix, `grams` is a length-1 vector containing X.
//
// If num_y != 0 and with_cross_y = true, then the sos polynomial is
// ⌈    m(s)⌉ᵀ * Y * ⌈    m(s)⌉
// | y₀*m(s)|        | y₀*m(s)|
// |   ...  |        |   ...  |
// ⌊ yₙ*m(s)⌋        ⌊ yₙ*m(s)⌋
// where n = num_y-1. Y is a Gram matrix, `grams` is a length-1 vector
// containing Y.
//
// if num_y != 0 and with_cross_y = false, then the sos polynomial is
// ∑ᵢ ⌈    m(s)⌉ᵀ * Zᵢ * ⌈    m(s)⌉
//    ⌊ yᵢ*m(s)⌋         ⌊ yᵢ*m(s)⌋
// where Zᵢ is a Gram matrix, i = 0, ..., num_y-1.  `grams` is a vector of
// length `num_y`, and grams[i] = Zᵢ
struct GramAndMonomialBasis {
  DRAKE_DEFAULT_COPY_AND_MOVE_AND_ASSIGN(GramAndMonomialBasis)

  GramAndMonomialBasis(
      const std::array<VectorX<symbolic::Monomial>, 4>& monomial_basis_array,
      bool with_cross_y, int num_y);

  // Adds the constraint that the polynomial represented by this Gram and
  // monomial basis is sos.
  void AddSos(solvers::MathematicalProgram* prog,
              const Eigen::Ref<const VectorX<symbolic::Variable>>& gram_lower,
              symbolic::Polynomial* poly);

  int gram_var_size;
  std::vector<MatrixX<symbolic::Variable>> grams;
  std::vector<VectorX<symbolic::Monomial>> monomial_basis;
};

}  // namespace internal
}  // namespace optimization
}  // namespace geometry
}  // namespace drake
