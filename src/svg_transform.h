#pragma once

#include <string>

namespace svg_squisher {

struct Matrix {
  double a = 1.0;
  double b = 0.0;
  double c = 0.0;
  double d = 1.0;
  double e = 0.0;
  double f = 0.0;
};

struct Point {
  double x = 0.0;
  double y = 0.0;
};

Matrix multiply(const Matrix& lhs, const Matrix& rhs);
Point apply_matrix(const Matrix& matrix, Point p);
bool matrix_is_identity(const Matrix& m, double eps = 1e-9);
bool matrix_is_scale_translate_only(const Matrix& m, double eps = 1e-9);
bool transform_is_valid(const std::string& transform_text);
Matrix parse_transform(const std::string& transform_text);
std::string combine_transform(const std::string& parent, const std::string& local);

}  // namespace svg_squisher
