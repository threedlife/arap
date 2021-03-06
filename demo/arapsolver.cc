#include "arapsolver.h"

// C++ standard library
#include <iostream>
#include <vector>

#include "igl/slice.h"
#include "igl/svd3x3/polar_svd3x3.h"

namespace arap {
namespace demo {

const double kMatrixDiffThreshold = 1e-6;

ArapSolver::ArapSolver(const Eigen::MatrixXd& vertices,
    const Eigen::MatrixXi& faces, const Eigen::VectorXi& fixed,
    int max_iteration)
  : Solver(vertices, faces, fixed, max_iteration) {
}

void ArapSolver::Precompute() {
  int vertex_num = vertices_.rows();
  int face_num = faces_.rows();

  // Compute weight_.
  weight_.resize(vertex_num, vertex_num);
  // An index map to help mapping from one vertex to the corresponding edge.
  int index_map[3][2] = { {1, 2}, {2, 0}, {0, 1} };
  // Loop over all the faces.
  for (int f = 0; f < face_num; ++f) {
    // Get the cotangent value with that face.
    Eigen::Vector3d cotangent = ComputeCotangent(f);
    // Loop over the three vertices within the same triangle.
    // i = 0 => A.
    // i = 1 => B.
    // i = 2 => C.
    for (int i = 0; i < 3; ++i) {
      // Indices of the two vertices in the edge corresponding to vertex i.
      int first = faces_(f, index_map[i][0]);
      int second = faces_(f, index_map[i][1]);
      double half_cot = cotangent(i) / 2.0;
      weight_.coeffRef(first, second) += half_cot;
      weight_.coeffRef(second, first) += half_cot;
      // Note that weight_(i, i) is the sum of all the -weight_(i, j).
      weight_.coeffRef(first, first) -= half_cot;
      weight_.coeffRef(second, second) -= half_cot;
    }
  }

  // Compute neighbors.
  neighbors_.resize(vertex_num, Neighbors());
  for (int f = 0; f < face_num; ++f) {
    for (int i = 0; i < 3; ++i) {
      int first = faces_(f, index_map[i][0]);
      int second = faces_(f, index_map[i][1]);
      neighbors_[first][second] = second;
      neighbors_[second][first] = first;
    }
  }

  // Compute lb_operator_.
  int free_num = free_.size();
  lb_operator_.resize(free_num, free_num);
  for (int i = 0; i < free_num; ++i) {
    // pos is the vertex's position in vertices_.
    int pos = free_(i);
    // Loop over all the neighbors.
    for (auto& neighbor : neighbors_[pos]) {
      // Get the index of the neighbor.
      int neighbor_pos = neighbor.first;
      double weight = weight_.coeff(pos, neighbor_pos);
      lb_operator_.coeffRef(i, i) += weight;
      if (vertex_info_[neighbor_pos].type == VertexType::Free) {
        lb_operator_.coeffRef(i, vertex_info_[neighbor_pos].pos) -= weight;
      }
    }
  }
  lb_operator_.makeCompressed();
  // FYI: another simple method to compute lb_operator_ is below.
  // Compute lb_operator_. This matrix can be computed by extracting free_ rows
  // and columns from -weight_.
  // igl::slice(weight_, free_, free_, lb_operator_);
  // lb_operator_ *= -1.0;

  // LU factorization.
  solver_.compute(lb_operator_);
  if (solver_.info() != Eigen::Success) {
    // Failed to decompose lb_operator_.
    std::cout << "Fail to do LU factorization." << std::endl;
    exit(EXIT_FAILURE);
  }
}

void ArapSolver::SolvePreprocess(const Eigen::MatrixXd& fixed_vertices) {
  // Initialize fixed_vertices_.
  fixed_vertices_ = fixed_vertices;

  // Initialize vertices_updated_ with Naive Laplacian editing. This method
  // tries to minimize ||Lp' - Lp||^2 with fixed vertices constraints.
  // Get all the dimensions.
  int vertex_num = vertices_.rows();
  int fixed_num = fixed_.size();
  int free_num = free_.size();
  int dims = vertices_.cols();
  // Initialize vertices_updated_.
  vertices_updated_.resize(vertex_num, dims);

  // Note that L = -weight_.
  // Denote y to be the fixed vertices:
  // ||Lp' - Lp|| => ||-Ax - By - Lp|| => ||Ax - (-By + weight_ * p)||
  // => A'Ax = A'(-By + weight_ * p).
  // Build A and B first.
  Eigen::SparseMatrix<double> A, B;
  Eigen::VectorXi r;
  igl::colon<int>(0, vertices_.rows() - 1, r);
  igl::slice(weight_, r, free_, A);
  igl::slice(weight_, r, fixed_, B);

  // Build A' * A.
  Eigen::SparseLU<Eigen::SparseMatrix<double>> naive_lap_solver;
  Eigen::SparseMatrix<double> left = A.transpose() * A;
  left.makeCompressed();
  naive_lap_solver.compute(left);
  if (naive_lap_solver.info() != Eigen::Success) {
    std::cout << "Fail to decompose naive Laplacian function." << std::endl;
    exit(EXIT_FAILURE);
  }

  // Build A' * (-By + weight_ * p).
  for (int c = 0; c < dims; ++c) {
    Eigen::VectorXd b = weight_ * vertices_.col(c) - B * fixed_vertices_.col(c);
    Eigen::VectorXd right = A.transpose() * b;
    Eigen::VectorXd x = naive_lap_solver.solve(right);
    if (naive_lap_solver.info() != Eigen::Success) {
      std::cout << "Fail to solve naive Laplacian function." << std::endl;
      exit(EXIT_FAILURE);
    }
    // Sanity check the solution.
    if ((left * x - right).squaredNorm() > kMatrixDiffThreshold) {
      std::cout << "Wrong in the naive Laplacian solver." << std::endl;
      return;
    }
    // Write back the solution.
    for (int i = 0; i < free_num; ++i) {
      vertices_updated_(free_(i), c) = x(i);
    }
  }

  // Write back fixed vertices constraints.
  for (int i = 0; i < fixed_num; ++i) {
    vertices_updated_.row(fixed_(i)) = fixed_vertices_.row(i);
  }

  // Initialize rotations_ with vertices_updated_.
  std::vector<Eigen::Matrix3d> edge_product(vertex_num,
      Eigen::Matrix3d::Zero());
  for (int i = 0; i < vertex_num; ++i) {
    for (auto& neighbor : neighbors_[i]) {
      int j = neighbor.first;
      double weight = weight_.coeff(i, j);
      Eigen::Vector3d edge = vertices_.row(i) - vertices_.row(j);
      Eigen::Vector3d edge_update =
        vertices_updated_.row(i) - vertices_updated_.row(j);
      edge_product[i] += weight * edge * edge_update.transpose();
    }
  }
  rotations_.clear();
  for (int v = 0; v < vertex_num; ++v) {
    Eigen::Matrix3d rotation;
    igl::polar_svd3x3(edge_product[v], rotation);
    rotations_.push_back(rotation.transpose());
  }
}

void ArapSolver::SolveOneIteration() {
  int fixed_num = fixed_.size();
  int vertex_num = vertices_.rows();
  int face_num = faces_.rows();

  // A temporary vector to hold all the edge products for all the vertices.
  // This is the S matrix in equation (5).
  std::vector<Eigen::Matrix3d> edge_product;
  // Step 1: solve rotations by polar_svd.
  // Clear edge_product.
  edge_product.clear();
  edge_product.resize(vertex_num, Eigen::Matrix3d::Zero());
  for (int i = 0; i < vertex_num; ++i) {
    for (auto& neighbor : neighbors_[i]) {
      int j = neighbor.first;
      double weight = weight_.coeff(i, j);
      Eigen::Vector3d edge = vertices_.row(i) - vertices_.row(j);
      Eigen::Vector3d edge_update =
        vertices_updated_.row(i) - vertices_updated_.row(j);
      edge_product[i] += weight * edge * edge_update.transpose();
    }
  }
  for (int v = 0; v < vertex_num; ++v) {
    Eigen::Matrix3d rotation;
    igl::polar_svd3x3(edge_product[v], rotation);
    rotations_[v] = rotation.transpose();
  }
  // Step 2: compute the rhs in equation (9).
  int free_num = free_.size();
  // The right hand side of equation (9). The x, y and z coordinates are
  // computed separately.
  Eigen::MatrixXd rhs = Eigen::MatrixXd::Zero(free_num, 3);
  for (int i = 0; i < free_num; ++i) {
    int i_pos = free_(i);
    for (auto& neighbor : neighbors_[i_pos]) {
      int j_pos = neighbor.first;
      double weight = weight_.coeff(i_pos, j_pos);
      Eigen::Vector3d vec = weight / 2.0
        * (rotations_[i_pos] + rotations_[j_pos])
        * (vertices_.row(i_pos) - vertices_.row(j_pos)).transpose();
      rhs.row(i) += vec;
      if (vertex_info_[j_pos].type == VertexType::Fixed) {
        rhs.row(i) += weight * vertices_updated_.row(j_pos);
      }
    }
  }
  // Solve for free_.
  Eigen::MatrixXd solution = solver_.solve(rhs);
  if (solver_.info() != Eigen::Success) {
    std::cout << "Fail to solve the sparse linear system." << std::endl;
    exit(EXIT_FAILURE);
  }
  for (int i = 0; i < free_num; ++i) {
    int pos = free_(i);
    vertices_updated_.row(pos) = solution.row(i);
  }
}

void ArapSolver::SolvePostprocess() {
  // Simply do nothing.
  return;
}

Eigen::Vector3d ArapSolver::ComputeCotangent(int face_id) const {
  Eigen::Vector3d cotangent(0.0, 0.0, 0.0);
  // The triangle is defined as follows:
  //            A
  //           /  -
  //        c /     - b
  //         /        -
  //        /    a      -
  //       B--------------C
  // where A, B, C corresponds to faces_(face_id, 0), faces_(face_id, 1) and
  // faces_(face_id, 2). The return value is (cotA, cotB, cotC).
  // Compute the triangle area first.
  Eigen::Vector3d A = vertices_.row(faces_(face_id, 0));
  Eigen::Vector3d B = vertices_.row(faces_(face_id, 1));
  Eigen::Vector3d C = vertices_.row(faces_(face_id, 2));
  double a_squared = (B - C).squaredNorm();
  double b_squared = (C - A).squaredNorm();
  double c_squared = (A - B).squaredNorm();
  // Compute the area of the triangle. area = 1/2bcsinA.
  double area = (B - A).cross(C - A).norm() / 2;
  // Compute cotA = cosA / sinA.
  // b^2 + c^2 -2bccosA = a^2, or cosA = (b^2 + c^2 - a^2) / 2bc.
  // 1/2bcsinA = area, or sinA = 2area/bc.
  // cotA = (b^2 + c^2 -a^2) / 4area.
  double four_area = 4 * area;
  cotangent(0) = (b_squared + c_squared - a_squared) / four_area;
  cotangent(1) = (c_squared + a_squared - b_squared) / four_area;
  cotangent(2) = (a_squared + b_squared - c_squared) / four_area;
  return cotangent;
}

Energy ArapSolver::ComputeEnergy() const {
  // Compute the energy.
  double total = 0.0;
  int vertex_num = vertices_.rows();
  for (int i = 0; i < vertex_num; ++i) {
    for (auto& neighbor : neighbors_[i]) {
      int j = neighbor.first;
      double weight = weight_.coeff(i, j);
      double edge_energy = 0.0;
      Eigen::Vector3d vec = (vertices_updated_.row(i) -
          vertices_updated_.row(j)).transpose() -
          rotations_[i] * (vertices_.row(i) -
          vertices_.row(j)).transpose();
      edge_energy = weight * vec.squaredNorm();
      total += edge_energy;
    }
  }
  Energy energy;
  energy.AddEnergyType("Total", total);
  return energy;
}

}  // namespace demo
}  // namespace arap
